module;

// Bridges Scene + Geometry + Rendering, so (like those) it uses classic includes.
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>

export module Saffron.Assets;

import Saffron.Core;
import Saffron.Geometry;
import Saffron.Rendering;
import Saffron.Scene;

export namespace se
{
    // Resolves mesh assets for the running scene. pathByUuid is the persisted
    // registry (id -> baked .smesh relative to root); meshHandleByUuid is the
    // in-memory cache of uploaded GPU meshes, so entities sharing an id upload once.
    struct AssetServer
    {
        std::string root;
        std::unordered_map<u64, std::string> pathByUuid;
        std::unordered_map<u64, u32> meshHandleByUuid;
    };

    void writeAssetRegistry(const AssetServer& assets)
    {
        nlohmann::json meshes = nlohmann::json::object();
        for (const auto& [uuid, path] : assets.pathByUuid)
        {
            meshes[std::to_string(uuid)] = path;
        }
        std::ofstream out(assets.root + "/asset_registry.json");
        if (out)
        {
            out << nlohmann::json{ { "version", 1 }, { "meshes", std::move(meshes) } }.dump(2);
        }
    }

    // Creates the asset root (+ meshes dir) and loads any existing registry.
    AssetServer newAssetServer(std::string root)
    {
        AssetServer assets;
        assets.root = std::move(root);
        std::error_code ec;
        std::filesystem::create_directories(assets.root + "/meshes", ec);

        std::ifstream in(assets.root + "/asset_registry.json");
        if (in)
        {
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
            if (!doc.is_discarded() && doc.contains("meshes") && doc["meshes"].is_object())
            {
                for (auto it = doc["meshes"].begin(); it != doc["meshes"].end(); ++it)
                {
                    if (it.value().is_string())
                    {
                        const u64 uuid = std::strtoull(it.key().c_str(), nullptr, 10);
                        assets.pathByUuid[uuid] = it.value().get<std::string>();
                    }
                }
            }
        }
        return assets;
    }

    // Imports a source model, bakes it to a .smesh under root, uploads it, registers
    // the id -> path mapping (persisting the registry), and returns the new id.
    std::expected<Uuid, std::string> importModel(AssetServer& assets, Renderer& renderer, const std::string& path)
    {
        std::expected<Mesh, std::string> mesh = importModelFile(path);
        if (!mesh)
        {
            return std::unexpected(mesh.error());
        }
        const Uuid id = newUuid();
        const std::string relativePath = "meshes/" + std::to_string(id.value) + ".smesh";
        if (std::expected<void, std::string> baked = saveMesh(*mesh, assets.root + "/" + relativePath); !baked)
        {
            return std::unexpected(baked.error());
        }
        std::expected<u32, std::string> handle = uploadMesh(renderer, *mesh);
        if (!handle)
        {
            return std::unexpected(handle.error());
        }
        assets.pathByUuid[id.value] = relativePath;
        assets.meshHandleByUuid[id.value] = *handle;
        writeAssetRegistry(assets);
        return id;
    }

    // Resolves an id to a GPU mesh handle, loading + uploading the baked .smesh on a
    // cache miss. Returns false for an unregistered or unreadable asset.
    bool loadMeshAsset(AssetServer& assets, Renderer& renderer, Uuid id, u32& outHandle)
    {
        constexpr u32 invalidHandle = ~0u;  // negative-cache marker for a failed load

        auto cached = assets.meshHandleByUuid.find(id.value);
        if (cached != assets.meshHandleByUuid.end())
        {
            if (cached->second == invalidHandle)
            {
                return false;
            }
            outHandle = cached->second;
            return true;
        }
        auto path = assets.pathByUuid.find(id.value);
        if (path == assets.pathByUuid.end())
        {
            return false;
        }
        std::expected<Mesh, std::string> mesh = loadMesh(assets.root + "/" + path->second);
        if (mesh)
        {
            std::expected<u32, std::string> handle = uploadMesh(renderer, *mesh);
            if (handle)
            {
                assets.meshHandleByUuid[id.value] = *handle;
                outHandle = *handle;
                return true;
            }
            logWarn(std::format("asset {}: {}", id.value, handle.error()));
        }
        else
        {
            logWarn(std::format("asset {}: {}", id.value, mesh.error()));
        }
        // Negative-cache so a broken registered asset is not retried + re-logged each frame.
        assets.meshHandleByUuid[id.value] = invalidHandle;
        return false;
    }

    // Creates an entity carrying the given mesh asset.
    Entity spawnMesh(Scene& scene, std::string name, Uuid mesh)
    {
        Entity entity = createEntity(scene, std::move(name));
        addComponent<MeshComponent>(scene, entity).mesh = mesh;
        return entity;
    }

    // Draws every entity with a Transform + Mesh, viewed through the first primary
    // camera, resolving each mesh on demand. A no-op without a camera or viewport.
    void renderScene(Renderer& renderer, Scene& scene, AssetServer& assets, u32 meshPipeline)
    {
        bool haveCamera = false;
        glm::mat4 view{ 1.0f };
        f32 fov = 45.0f;
        f32 nearPlane = 0.1f;
        f32 farPlane = 100.0f;
        forEach<TransformComponent, CameraComponent>(scene,
            [&](Entity, TransformComponent& transform, CameraComponent& camera)
            {
                if (haveCamera || !camera.primary)
                {
                    return;
                }
                const glm::mat4 cameraModel =
                    glm::translate(glm::mat4(1.0f), transform.translation) * glm::mat4_cast(transform.rotation);
                view = glm::inverse(cameraModel);
                fov = camera.fov;
                nearPlane = camera.nearPlane;
                farPlane = camera.farPlane;
                haveCamera = true;
            });
        if (!haveCamera)
        {
            return;
        }

        const u32 width = viewportWidth(renderer);
        const u32 height = viewportHeight(renderer);
        if (width == 0 || height == 0)
        {
            return;
        }
        const f32 aspect = static_cast<f32>(width) / static_cast<f32>(height);
        glm::mat4 proj = glm::perspective(glm::radians(fov), aspect, nearPlane, farPlane);
        proj[1][1] *= -1.0f;  // flip Y into Vulkan clip space
        const glm::mat4 viewProjection = proj * view;

        glm::vec3 lightDir{ -0.5f, -1.0f, -0.3f };
        glm::vec3 lightColor{ 1.0f };
        f32 lightIntensity = 1.0f;
        f32 lightAmbient = 0.15f;
        bool haveLight = false;
        forEach<DirectionalLightComponent>(scene, [&](Entity, DirectionalLightComponent& light)
        {
            if (haveLight)
            {
                return;
            }
            lightDir = light.direction;
            lightColor = light.color;
            lightIntensity = light.intensity;
            lightAmbient = light.ambient;
            haveLight = true;
        });
        setDirectionalLight(renderer, lightDir, lightColor, lightIntensity, lightAmbient);

        forEach<TransformComponent, MeshComponent>(scene,
            [&](Entity entity, TransformComponent& transform, MeshComponent& mesh)
            {
                u32 handle = 0;
                if (!loadMeshAsset(assets, renderer, mesh.mesh, handle))
                {
                    return;
                }
                glm::vec4 baseColor{ 1.0f };
                if (hasComponent<MaterialComponent>(scene, entity))
                {
                    baseColor = getComponent<MaterialComponent>(scene, entity).baseColor;
                }
                const glm::mat4 model = transformMatrix(transform);
                DrawParams params;
                params.mvp = viewProjection * model;
                params.normal0 = glm::vec4(glm::vec3(model[0]), 0.0f);
                params.normal1 = glm::vec4(glm::vec3(model[1]), 0.0f);
                params.normal2 = glm::vec4(glm::vec3(model[2]), 0.0f);
                params.baseColor = baseColor;
                drawMesh(renderer, handle, meshPipeline, defaultTexture(renderer), params);
            });
    }
}
