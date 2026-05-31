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
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

export module Saffron.Assets;

import Saffron.Core;
import Saffron.Geometry;
import Saffron.Rendering;
import Saffron.Scene;

export namespace se
{
    // Owns the project's asset catalog (id -> {name, type, path}) plus uuid-keyed GPU
    // caches so entities sharing an id upload once. A cached null Ref is the
    // negative-cache marker — a failed asset is not retried each frame.
    struct AssetServer
    {
        std::string root;
        AssetCatalog catalog;                                       // source of truth: id -> {name,type,path}
        std::unordered_map<u64, Ref<GpuMesh>> meshRefByUuid;        // GPU cache
        std::unordered_map<u64, Ref<GpuTexture>> textureRefByUuid;  // GPU cache
    };

    // What importModel produces: the imported mesh + its primary material.
    struct ImportResult
    {
        Uuid mesh;
        glm::vec4 baseColor{ 1.0f };
        Uuid albedoTexture;  // 0 == none
    };

    const char* assetTypeName(AssetType type)
    {
        if (type == AssetType::Texture) { return "texture"; }
        if (type == AssetType::Other) { return "other"; }
        return "mesh";
    }

    AssetType assetTypeFromName(const std::string& name)
    {
        if (name == "texture") { return AssetType::Texture; }
        if (name == "other") { return AssetType::Other; }
        return AssetType::Mesh;
    }

    nlohmann::json catalogToJson(const AssetCatalog& catalog)
    {
        nlohmann::json assets = nlohmann::json::array();
        for (const AssetEntry& entry : catalog.entries)
        {
            assets.push_back(nlohmann::json{ { "id", entry.id.value }, { "name", entry.name },
                                             { "type", assetTypeName(entry.type) }, { "path", entry.path } });
        }
        return assets;
    }

    void catalogFromJson(AssetCatalog& catalog, const nlohmann::json& assets)
    {
        catalog.entries.clear();
        catalog.byId.clear();
        if (!assets.is_array())
        {
            return;
        }
        for (const nlohmann::json& entry : assets)
        {
            if (!entry.is_object())
            {
                continue;
            }
            AssetEntry parsed;
            parsed.id = Uuid{ entry.value("id", u64{ 0 }) };
            parsed.name = entry.value("name", std::string{});
            parsed.type = assetTypeFromName(entry.value("type", std::string{ "mesh" }));
            parsed.path = entry.value("path", std::string{});
            if (parsed.id.value != 0)
            {
                putAsset(catalog, std::move(parsed));
            }
        }
    }

    // Creates the asset root (+ subdirs) and migrates any legacy asset_registry.json
    // into the catalog with synthesized names. The catalog is otherwise loaded from a
    // project file via loadProject.
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
                auto migrate = [&](const char* key, AssetType type)
                {
                    if (!doc.contains(key) || !doc[key].is_object())
                    {
                        return;
                    }
                    for (auto it = doc[key].begin(); it != doc[key].end(); ++it)
                    {
                        if (!it.value().is_string())
                        {
                            continue;
                        }
                        const std::string path = it.value().get<std::string>();
                        AssetEntry entry;
                        entry.id = Uuid{ std::strtoull(it.key().c_str(), nullptr, 10) };
                        entry.name = uniqueName(assets.catalog, std::filesystem::path(path).stem().string());
                        entry.type = type;
                        entry.path = path;
                        putAsset(assets.catalog, std::move(entry));
                    }
                };
                migrate("meshes", AssetType::Mesh);
                migrate("textures", AssetType::Texture);
            }
        }
        return assets;
    }

    // Constant version for the unified project document.
    inline constexpr int ProjectVersion = 1;

    // Saves the whole project (asset catalog + scene entities) to one JSON file.
    std::expected<void, std::string> saveProject(AssetServer& assets, ComponentRegistry& reg, Scene& scene,
                                                 const std::string& path)
    {
        nlohmann::json doc;
        doc["version"] = ProjectVersion;
        doc["assets"] = catalogToJson(assets.catalog);
        doc["scene"] = sceneToJson(reg, scene);

        std::ofstream out(path);
        if (!out)
        {
            return std::unexpected(std::format("cannot open '{}' for writing", path));
        }
        out << doc.dump(2);
        out.flush();
        if (!out)
        {
            return std::unexpected(std::format("write failed for '{}'", path));
        }
        return {};
    }

    // Loads a project file: replaces the catalog + scene. Clears the GPU caches (after a
    // device idle) so stale Refs are dropped and assets re-resolve from the new catalog.
    std::expected<void, std::string> loadProject(AssetServer& assets, Renderer& renderer, ComponentRegistry& reg,
                                                 Scene& scene, const std::string& path)
    {
        std::ifstream in(path);
        if (!in)
        {
            return std::unexpected(std::format("cannot open '{}'", path));
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
        if (doc.is_discarded() || !doc.is_object())
        {
            return std::unexpected(std::format("'{}': JSON parse error", path));
        }
        const int version = doc.value("version", 0);
        if (version != ProjectVersion)
        {
            return std::unexpected(std::format("unsupported project version {}", version));
        }

        waitGpuIdle(renderer);
        assets.meshRefByUuid.clear();
        assets.textureRefByUuid.clear();
        catalogFromJson(assets.catalog, doc.value("assets", nlohmann::json::array()));
        return sceneFromJson(reg, scene, doc.value("scene", nlohmann::json::object()));
    }

    // Writes encoded image bytes into assets/textures/<uuid>.<ext>, decodes + uploads
    // them, and adds a Texture entry to the catalog (named, deduped). Returns the id.
    std::expected<Uuid, std::string> registerTextureBytes(AssetServer& assets, Renderer& renderer,
                                                          const std::vector<u8>& encoded, const std::string& ext,
                                                          const std::string& name)
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
        putAsset(assets.catalog, AssetEntry{ id, uniqueName(assets.catalog, name), AssetType::Texture, relativePath });
        assets.textureRefByUuid[id.value] = *texture;
        return id;
    }

    // Imports an external image file into the asset dir + catalog (name = filename stem).
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
        const std::filesystem::path fsPath{ path };
        std::string ext = fsPath.extension().string();  // ".png" -> drop the dot
        if (!ext.empty())
        {
            ext.erase(0, 1);
        }
        return registerTextureBytes(assets, renderer, encoded, ext, fsPath.stem().string());
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
        const AssetEntry* entry = findAsset(assets.catalog, id);
        if (entry == nullptr || entry->type != AssetType::Texture)
        {
            return nullptr;
        }
        std::expected<DecodedImage, std::string> decoded = decodeImage(assets.root + "/" + entry->path);
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
    // primary material's albedo texture (if any), and adds catalog entries (named by
    // the source filename stem). Does not spawn an entity or save the project.
    std::expected<ImportResult, std::string> importModel(AssetServer& assets, Renderer& renderer, const std::string& path)
    {
        std::expected<ImportedModel, std::string> model = importModelWithMaterial(path);
        if (!model)
        {
            return std::unexpected(model.error());
        }
        const std::string baseName = std::filesystem::path(path).stem().string();
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
        putAsset(assets.catalog, AssetEntry{ meshId, uniqueName(assets.catalog, baseName), AssetType::Mesh, relativePath });
        assets.meshRefByUuid[meshId.value] = *meshRef;

        ImportResult result;
        result.mesh = meshId;
        result.baseColor = model->material.baseColor;
        if (model->material.hasAlbedo)
        {
            std::expected<Uuid, std::string> texture = registerTextureBytes(
                assets, renderer, model->material.albedoBytes, model->material.albedoExt, baseName + " albedo");
            if (texture)
            {
                result.albedoTexture = *texture;
            }
            else
            {
                logWarn(std::format("model '{}': albedo texture failed: {}", path, texture.error()));
            }
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
        const AssetEntry* entry = findAsset(assets.catalog, id);
        if (entry == nullptr || entry->type != AssetType::Mesh)
        {
            return nullptr;
        }
        std::expected<Mesh, std::string> mesh = loadMesh(assets.root + "/" + entry->path);
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

    // Draws every entity with a Transform + Mesh through the given camera (the editor
    // viewport camera), resolving each mesh on demand. A no-op without a viewport.
    void renderScene(Renderer& renderer, Scene& scene, AssetServer& assets,
                     const Ref<Pipeline>& meshPipeline, const CameraView& camera)
    {
        if (!camera.valid)
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
        const glm::mat4 view = camera.view;
        glm::mat4 proj = cameraProjection(camera, aspect);
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
        setClusterCamera(renderer, view, proj, camera.nearPlane, camera.farPlane);  // arms the cull dispatch

        // Gather the scene's renderables as a flat draw list; the renderer batches them
        // by (mesh, texture) and the scene + depth passes consume the result.
        std::vector<DrawItem> items;
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
                DrawItem item;
                item.mesh = meshRef;
                item.texture = textureRef;
                item.model = model;
                item.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(model))));
                item.baseColor = baseColor;
                items.push_back(std::move(item));
            });

        submitDrawList(renderer, meshPipeline, viewProjection, items);
    }

    // Picks the nearest entity whose world-space mesh AABB the camera ray hits. `ndc` is
    // the click point in clip space [-1,1] matching the rendered image (Y-flipped proj).
    // Returns a null Entity on a miss (the caller clears the selection).
    Entity pickEntity(Scene& scene, AssetServer& assets, Renderer& renderer,
                      const CameraView& camera, glm::vec2 ndc)
    {
        if (!camera.valid)
        {
            return Entity{ entt::null };
        }
        const u32 width = viewportWidth(renderer);
        const u32 height = viewportHeight(renderer);
        if (width == 0 || height == 0)
        {
            return Entity{ entt::null };
        }
        const f32 aspect = static_cast<f32>(width) / static_cast<f32>(height);
        glm::mat4 proj = cameraProjection(camera, aspect);
        proj[1][1] *= -1.0f;  // match the renderer's clip space
        const glm::mat4 invViewProj = glm::inverse(proj * camera.view);
        const glm::vec4 nearH = invViewProj * glm::vec4(ndc.x, ndc.y, 0.0f, 1.0f);  // GLM 0..1 depth: near = 0
        const glm::vec4 farH = invViewProj * glm::vec4(ndc.x, ndc.y, 1.0f, 1.0f);
        const glm::vec3 origin = glm::vec3(nearH) / nearH.w;
        const glm::vec3 dir = glm::normalize(glm::vec3(farH) / farH.w - origin);
        const glm::vec3 invDir = 1.0f / dir;  // inf for axis-aligned components is fine for the slab test

        Entity hit{ entt::null };
        f32 nearest = std::numeric_limits<f32>::max();
        forEach<TransformComponent, MeshComponent>(scene,
            [&](Entity entity, TransformComponent& transform, MeshComponent& mesh)
            {
                Ref<GpuMesh> meshRef = loadMeshAsset(assets, renderer, mesh.mesh);
                if (!meshRef)
                {
                    return;
                }
                // World AABB from the 8 transformed local-AABB corners.
                const glm::mat4 model = transformMatrix(transform);
                const glm::vec3 lo = meshRef->boundsMin;
                const glm::vec3 hi = meshRef->boundsMax;
                glm::vec3 worldMin{ std::numeric_limits<f32>::max() };
                glm::vec3 worldMax{ std::numeric_limits<f32>::lowest() };
                for (u32 corner = 0; corner < 8; corner = corner + 1)
                {
                    glm::vec3 p = lo;
                    if (corner & 1u) { p.x = hi.x; }
                    if (corner & 2u) { p.y = hi.y; }
                    if (corner & 4u) { p.z = hi.z; }
                    const glm::vec3 world = glm::vec3(model * glm::vec4(p, 1.0f));
                    worldMin = glm::min(worldMin, world);
                    worldMax = glm::max(worldMax, world);
                }

                const glm::vec3 t0 = (worldMin - origin) * invDir;
                const glm::vec3 t1 = (worldMax - origin) * invDir;
                const glm::vec3 tlo = glm::min(t0, t1);
                const glm::vec3 thi = glm::max(t0, t1);
                const f32 tEnter = glm::max(glm::max(tlo.x, tlo.y), tlo.z);
                const f32 tExit = glm::min(glm::min(thi.x, thi.y), thi.z);
                if (tExit < 0.0f || tEnter > tExit)
                {
                    return;
                }
                f32 t = tEnter;
                if (t < 0.0f)
                {
                    t = tExit;  // ray origin inside the box
                }
                if (t < nearest)
                {
                    nearest = t;
                    hit = entity;
                }
            });
        return hit;
    }
}
