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
        std::unordered_map<u64, std::string> pathByUuid;          // id -> baked .smesh
        std::unordered_map<u64, u32> meshHandleByUuid;            // cache of uploaded meshes
        std::unordered_map<u64, std::string> texturePathByUuid;   // id -> copied texture file
        std::unordered_map<u64, u32> textureHandleByUuid;         // cache of uploaded textures
    };

    // What importModel produces: the spawned mesh + its primary material.
    struct ImportResult
    {
        Uuid mesh;
        glm::vec4 baseColor{ 1.0f };
        Uuid albedoTexture;  // 0 == none
    };

    void writeAssetRegistry(const AssetServer& assets)
    {
        nlohmann::json meshes = nlohmann::json::object();
        for (const auto& [uuid, path] : assets.pathByUuid)
        {
            meshes[std::to_string(uuid)] = path;
        }
        nlohmann::json textures = nlohmann::json::object();
        for (const auto& [uuid, path] : assets.texturePathByUuid)
        {
            textures[std::to_string(uuid)] = path;
        }
        std::ofstream out(assets.root + "/asset_registry.json");
        if (out)
        {
            out << nlohmann::json{ { "version", 1 }, { "meshes", std::move(meshes) },
                                   { "textures", std::move(textures) } }.dump(2);
        }
    }

    // Creates the asset root (+ meshes dir) and loads any existing registry.
    AssetServer newAssetServer(std::string root)
    {
        AssetServer assets;
        assets.root = std::move(root);
        std::error_code ec;
        std::filesystem::create_directories(assets.root + "/meshes", ec);
        std::filesystem::create_directories(assets.root + "/textures", ec);

        std::ifstream in(assets.root + "/asset_registry.json");
        if (in)
        {
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
            if (!doc.is_discarded())
            {
                if (doc.contains("meshes") && doc["meshes"].is_object())
                {
                    for (auto it = doc["meshes"].begin(); it != doc["meshes"].end(); ++it)
                    {
                        if (it.value().is_string())
                        {
                            assets.pathByUuid[std::strtoull(it.key().c_str(), nullptr, 10)] = it.value().get<std::string>();
                        }
                    }
                }
                if (doc.contains("textures") && doc["textures"].is_object())
                {
                    for (auto it = doc["textures"].begin(); it != doc["textures"].end(); ++it)
                    {
                        if (it.value().is_string())
                        {
                            assets.texturePathByUuid[std::strtoull(it.key().c_str(), nullptr, 10)] = it.value().get<std::string>();
                        }
                    }
                }
            }
        }
        return assets;
    }

    // Writes encoded image bytes into assets/textures/<uuid>.<ext>, decodes + uploads
    // them, registers + persists the mapping, and returns the new texture id.
    std::expected<Uuid, std::string> registerTextureBytes(AssetServer& assets, Renderer& renderer,
                                                          const std::vector<u8>& encoded, const std::string& ext)
    {
        std::expected<DecodedImage, std::string> decoded = decodeImageFromMemory(encoded);
        if (!decoded)
        {
            return std::unexpected(decoded.error());
        }
        std::expected<u32, std::string> handle = uploadTexture(renderer, decoded->rgba.data(), decoded->width, decoded->height, true);
        if (!handle)
        {
            return std::unexpected(handle.error());
        }
        const Uuid id = newUuid();
        std::string extension = ext;
        if (extension.empty())
        {
            extension = "png";
        }
        const std::string relativePath = "textures/" + std::to_string(id.value) + "." + extension;
        std::ofstream out(assets.root + "/" + relativePath, std::ios::binary);
        if (!out)
        {
            return std::unexpected(std::format("cannot write texture '{}'", relativePath));
        }
        out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        assets.texturePathByUuid[id.value] = relativePath;
        assets.textureHandleByUuid[id.value] = *handle;
        writeAssetRegistry(assets);
        return id;
    }

    // Imports an external image file into the asset dir and registers it.
    std::expected<Uuid, std::string> importTexture(AssetServer& assets, Renderer& renderer, const std::string& path)
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
        {
            return std::unexpected(std::format("cannot open '{}'", path));
        }
        const std::streamsize size = in.tellg();
        in.seekg(0);
        std::vector<u8> encoded(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(encoded.data()), size);
        if (!in)
        {
            return std::unexpected(std::format("read failed for '{}'", path));
        }
        const std::size_t dot = path.find_last_of('.');
        std::string ext;
        if (dot != std::string::npos)
        {
            ext = path.substr(dot + 1);
        }
        return registerTextureBytes(assets, renderer, encoded, ext);
    }

    // Resolves a texture id to a GPU texture handle, decoding + uploading the copied
    // file on a cache miss. Returns false (negative-cached) for an unreadable asset.
    bool loadTextureAsset(AssetServer& assets, Renderer& renderer, Uuid id, u32& outHandle)
    {
        constexpr u32 invalidHandle = ~0u;
        auto cached = assets.textureHandleByUuid.find(id.value);
        if (cached != assets.textureHandleByUuid.end())
        {
            if (cached->second == invalidHandle)
            {
                return false;
            }
            outHandle = cached->second;
            return true;
        }
        auto path = assets.texturePathByUuid.find(id.value);
        if (path == assets.texturePathByUuid.end())
        {
            return false;
        }
        std::expected<DecodedImage, std::string> decoded = decodeImage(assets.root + "/" + path->second);
        if (decoded)
        {
            std::expected<u32, std::string> handle = uploadTexture(renderer, decoded->rgba.data(), decoded->width, decoded->height, true);
            if (handle)
            {
                assets.textureHandleByUuid[id.value] = *handle;
                outHandle = *handle;
                return true;
            }
            logWarn(std::format("texture {}: {}", id.value, handle.error()));
        }
        else
        {
            logWarn(std::format("texture {}: {}", id.value, decoded.error()));
        }
        assets.textureHandleByUuid[id.value] = invalidHandle;
        return false;
    }

    // Imports a source model: bakes its mesh to a .smesh, uploads it, imports its
    // primary material's albedo texture (if any), and registers + persists everything.
    std::expected<ImportResult, std::string> importModel(AssetServer& assets, Renderer& renderer, const std::string& path)
    {
        std::expected<ImportedModel, std::string> model = importModelWithMaterial(path);
        if (!model)
        {
            return std::unexpected(model.error());
        }
        const Uuid meshId = newUuid();
        const std::string relativePath = "meshes/" + std::to_string(meshId.value) + ".smesh";
        if (std::expected<void, std::string> baked = saveMesh(model->mesh, assets.root + "/" + relativePath); !baked)
        {
            return std::unexpected(baked.error());
        }
        std::expected<u32, std::string> handle = uploadMesh(renderer, model->mesh);
        if (!handle)
        {
            return std::unexpected(handle.error());
        }
        assets.pathByUuid[meshId.value] = relativePath;
        assets.meshHandleByUuid[meshId.value] = *handle;

        ImportResult result;
        result.mesh = meshId;
        result.baseColor = model->material.baseColor;
        if (model->material.hasAlbedo)
        {
            std::expected<Uuid, std::string> texture =
                registerTextureBytes(assets, renderer, model->material.albedoBytes, model->material.albedoExt);
            if (texture)
            {
                result.albedoTexture = *texture;
            }
            else
            {
                logWarn(std::format("model '{}': albedo texture failed: {}", path, texture.error()));
            }
        }
        writeAssetRegistry(assets);
        return result;
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

    // Creates an entity from an import: a mesh + a material (base color + albedo).
    Entity spawnModel(Scene& scene, std::string name, const ImportResult& result)
    {
        Entity entity = createEntity(scene, std::move(name));
        addComponent<MeshComponent>(scene, entity).mesh = result.mesh;
        MaterialComponent& material = addComponent<MaterialComponent>(scene, entity);
        material.baseColor = result.baseColor;
        material.albedoTexture = result.albedoTexture;
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
                u32 textureHandle = defaultTexture(renderer);
                if (hasComponent<MaterialComponent>(scene, entity))
                {
                    const MaterialComponent& material = getComponent<MaterialComponent>(scene, entity);
                    baseColor = material.baseColor;
                    if (material.albedoTexture.value != 0)
                    {
                        u32 resolved = 0;
                        if (loadTextureAsset(assets, renderer, material.albedoTexture, resolved))
                        {
                            textureHandle = resolved;
                        }
                    }
                }
                const glm::mat4 model = transformMatrix(transform);
                DrawParams params;
                params.mvp = viewProjection * model;
                params.normal0 = glm::vec4(glm::vec3(model[0]), 0.0f);
                params.normal1 = glm::vec4(glm::vec3(model[1]), 0.0f);
                params.normal2 = glm::vec4(glm::vec3(model[2]), 0.0f);
                params.baseColor = baseColor;
                drawMesh(renderer, handle, meshPipeline, textureHandle, params);
            });
    }
}
