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
    // registry (id -> baked .smesh relative to root); meshRefByUuid is the in-memory
    // cache of uploaded GPU meshes, so entities sharing an id upload once. A cached
    // null Ref is the negative-cache marker — a failed asset is not retried each frame.
    struct AssetServer
    {
        std::string root;
        std::unordered_map<u64, std::string> pathByUuid;            // id -> baked .smesh
        std::unordered_map<u64, Ref<GpuMesh>> meshRefByUuid;        // cache of uploaded meshes
        std::unordered_map<u64, std::string> texturePathByUuid;     // id -> copied texture file
        std::unordered_map<u64, Ref<GpuTexture>> textureRefByUuid;  // cache of uploaded textures
    };

    // What importModel produces: the spawned mesh + its primary material.
    struct ImportResult
    {
        Uuid mesh;
        glm::vec4 baseColor{ 1.0f };
        Uuid albedoTexture;  // 0 == none
    };

    std::expected<void, std::string> writeAssetRegistry(const AssetServer& assets)
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
        if (!out)
        {
            return std::unexpected(std::string{ "cannot open asset_registry.json for writing" });
        }
        out << nlohmann::json{ { "version", 1 }, { "meshes", std::move(meshes) },
                               { "textures", std::move(textures) } }.dump(2);
        if (!out)
        {
            return std::unexpected(std::string{ "asset_registry.json write failed" });
        }
        return {};
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
        std::expected<Ref<GpuTexture>, std::string> texture = uploadTexture(renderer, decoded->rgba.data(), decoded->width, decoded->height, true);
        if (!texture)
        {
            return std::unexpected(texture.error());
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
        if (!out)
        {
            return std::unexpected(std::format("write failed for texture '{}'", relativePath));
        }
        assets.texturePathByUuid[id.value] = relativePath;
        assets.textureRefByUuid[id.value] = *texture;
        return id;  // the caller persists the registry (so importModel writes it once)
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
        std::expected<Uuid, std::string> id = registerTextureBytes(assets, renderer, encoded, ext);
        if (!id)
        {
            return std::unexpected(id.error());
        }
        if (std::expected<void, std::string> persisted = writeAssetRegistry(assets); !persisted)
        {
            logWarn(persisted.error());
        }
        return id;
    }

    // Resolves a texture id to a GPU texture, decoding + uploading the copied file on
    // a cache miss. Returns a null Ref (negative-cached) for an unregistered or
    // unreadable asset.
    Ref<GpuTexture> loadTextureAsset(AssetServer& assets, Renderer& renderer, Uuid id)
    {
        auto cached = assets.textureRefByUuid.find(id.value);
        if (cached != assets.textureRefByUuid.end())
        {
            return cached->second;  // valid Ref, or a null Ref left by a prior failure
        }
        auto path = assets.texturePathByUuid.find(id.value);
        if (path == assets.texturePathByUuid.end())
        {
            return nullptr;
        }
        std::expected<DecodedImage, std::string> decoded = decodeImage(assets.root + "/" + path->second);
        if (decoded)
        {
            std::expected<Ref<GpuTexture>, std::string> texture = uploadTexture(renderer, decoded->rgba.data(), decoded->width, decoded->height, true);
            if (texture)
            {
                assets.textureRefByUuid[id.value] = *texture;
                return *texture;
            }
            logWarn(std::format("texture {}: {}", id.value, texture.error()));
        }
        else
        {
            logWarn(std::format("texture {}: {}", id.value, decoded.error()));
        }
        assets.textureRefByUuid[id.value] = nullptr;  // negative-cache the failure
        return nullptr;
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
        std::expected<Ref<GpuMesh>, std::string> meshRef = uploadMesh(renderer, model->mesh);
        if (!meshRef)
        {
            return std::unexpected(meshRef.error());
        }
        assets.pathByUuid[meshId.value] = relativePath;
        assets.meshRefByUuid[meshId.value] = *meshRef;

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
        if (std::expected<void, std::string> persisted = writeAssetRegistry(assets); !persisted)
        {
            logWarn(persisted.error());
        }
        return result;
    }

    // Resolves an id to a GPU mesh, loading + uploading the baked .smesh on a cache
    // miss. Returns a null Ref for an unregistered or unreadable asset.
    Ref<GpuMesh> loadMeshAsset(AssetServer& assets, Renderer& renderer, Uuid id)
    {
        auto cached = assets.meshRefByUuid.find(id.value);
        if (cached != assets.meshRefByUuid.end())
        {
            return cached->second;  // valid Ref, or a null Ref left by a prior failure
        }
        auto path = assets.pathByUuid.find(id.value);
        if (path == assets.pathByUuid.end())
        {
            return nullptr;
        }
        std::expected<Mesh, std::string> mesh = loadMesh(assets.root + "/" + path->second);
        if (mesh)
        {
            std::expected<Ref<GpuMesh>, std::string> meshRef = uploadMesh(renderer, *mesh);
            if (meshRef)
            {
                assets.meshRefByUuid[id.value] = *meshRef;
                return *meshRef;
            }
            logWarn(std::format("asset {}: {}", id.value, meshRef.error()));
        }
        else
        {
            logWarn(std::format("asset {}: {}", id.value, mesh.error()));
        }
        // Negative-cache so a broken registered asset is not retried + re-logged each frame.
        assets.meshRefByUuid[id.value] = nullptr;
        return nullptr;
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
    void renderScene(Renderer& renderer, Scene& scene, AssetServer& assets, const Ref<Pipeline>& meshPipeline)
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

        // Gather punctual (point + spot) lights, positioned by their Transform.
        std::vector<GpuLight> lights;
        forEach<TransformComponent, PointLightComponent>(scene,
            [&](Entity, TransformComponent& transform, PointLightComponent& light)
            {
                GpuLight gpu;
                gpu.positionRange = glm::vec4(transform.translation, light.range);
                gpu.colorIntensity = glm::vec4(light.color, light.intensity);
                gpu.directionType = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);  // type 0 = point
                gpu.spotCos = glm::vec4(0.0f);
                lights.push_back(gpu);
            });
        forEach<TransformComponent, SpotLightComponent>(scene,
            [&](Entity, TransformComponent& transform, SpotLightComponent& light)
            {
                const glm::vec3 dir = glm::normalize(light.direction);
                GpuLight gpu;
                gpu.positionRange = glm::vec4(transform.translation, light.range);
                gpu.colorIntensity = glm::vec4(light.color, light.intensity);
                gpu.directionType = glm::vec4(dir, 1.0f);  // type 1 = spot
                gpu.spotCos = glm::vec4(glm::cos(glm::radians(light.innerAngle)),
                                        glm::cos(glm::radians(light.outerAngle)), 0.0f, 0.0f);
                lights.push_back(gpu);
            });
        setSceneLighting(renderer, lightDir, lightColor, lightIntensity, lightAmbient, lights);
        setClusterCamera(renderer, view, proj, nearPlane, farPlane);  // arms the cull dispatch

        // Bucket entities by (mesh, albedo texture) so each bucket draws as one
        // instanced call. Linear lookup — bucket count is the number of distinct
        // mesh/material pairs, which stays small.
        struct Bucket
        {
            Ref<GpuMesh> mesh;
            Ref<GpuTexture> texture;
            std::vector<InstanceData> items;
        };
        std::vector<Bucket> buckets;
        forEach<TransformComponent, MeshComponent>(scene,
            [&](Entity entity, TransformComponent& transform, MeshComponent& mesh)
            {
                Ref<GpuMesh> meshRef = loadMeshAsset(assets, renderer, mesh.mesh);
                if (!meshRef)
                {
                    return;
                }
                glm::vec4 baseColor{ 1.0f };
                Ref<GpuTexture> textureRef;
                if (hasComponent<MaterialComponent>(scene, entity))
                {
                    const MaterialComponent& material = getComponent<MaterialComponent>(scene, entity);
                    baseColor = material.baseColor;
                    if (material.albedoTexture.value != 0)
                    {
                        textureRef = loadTextureAsset(assets, renderer, material.albedoTexture);
                    }
                }
                const glm::mat4 model = transformMatrix(transform);
                InstanceData data;
                data.model = model;
                data.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(model))));
                data.baseColor = baseColor;

                Bucket* bucket = nullptr;
                for (Bucket& candidate : buckets)
                {
                    if (candidate.mesh.get() == meshRef.get() && candidate.texture.get() == textureRef.get())
                    {
                        bucket = &candidate;
                        break;
                    }
                }
                if (bucket == nullptr)
                {
                    buckets.push_back(Bucket{ meshRef, textureRef, {} });
                    bucket = &buckets.back();
                }
                bucket->items.push_back(data);
            });

        // Flatten buckets into one contiguous instance array + per-batch offsets.
        std::vector<InstanceData> instances;
        std::vector<InstanceBatch> batches;
        batches.reserve(buckets.size());
        for (Bucket& bucket : buckets)
        {
            InstanceBatch batch;
            batch.mesh = bucket.mesh;
            batch.texture = bucket.texture;
            batch.baseInstance = static_cast<u32>(instances.size());
            batch.instanceCount = static_cast<u32>(bucket.items.size());
            instances.insert(instances.end(), bucket.items.begin(), bucket.items.end());
            batches.push_back(std::move(batch));
        }
        drawInstanced(renderer, meshPipeline, viewProjection, instances, batches);
    }
}
