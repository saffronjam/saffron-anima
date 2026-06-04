module;

// Bridges Scene + Geometry + Rendering, so (like those) it uses classic includes.
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cctype>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

export module Saffron.Assets;

import Saffron.Core;
import Saffron.Json;
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

    struct ProjectInfo
    {
        bool loaded = false;
        std::string root;
        std::string path;
        std::string name;
        std::string displayName;
    };

    auto appDataRoot() -> std::string
    {
        if (const char* override = std::getenv("SAFFRON_APPDATA_DIR"))
        {
            if (override[0] != '\0')
            {
                return override;
            }
        }
        return "appdata";
    }

    auto projectUserdataRoot() -> std::string
    {
        return (std::filesystem::path(appDataRoot()) / "userdata").string();
    }

    auto validProjectName(const std::string& name) -> bool
    {
        if (name.empty() || name.size() > 63)
        {
            return false;
        }
        auto isLowerDigit = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
        if (!isLowerDigit(name.front()) || !isLowerDigit(name.back()))
        {
            return false;
        }
        for (char c : name)
        {
            if (!isLowerDigit(c) && c != '-')
            {
                return false;
            }
        }
        return true;
    }

    auto defaultDisplayName(std::string name) -> std::string
    {
        if (name.empty())
        {
            return "Untitled Project";
        }
        bool capitalize = true;
        for (char& c : name)
        {
            if (c == '-')
            {
                c = ' ';
                capitalize = true;
            }
            else if (capitalize && c >= 'a' && c <= 'z')
            {
                c = static_cast<char>(c - 'a' + 'A');
                capitalize = false;
            }
            else
            {
                capitalize = false;
            }
        }
        return name;
    }

    auto projectJsonPath(const std::string& selection) -> std::filesystem::path
    {
        std::filesystem::path path{ selection };
        if (validProjectName(selection))
        {
            return std::filesystem::path(projectUserdataRoot()) / selection / "project.json";
        }
        if (path.filename() == "project.json")
        {
            return path;
        }
        return path / "project.json";
    }

    auto projectInfoFromPath(const std::filesystem::path& path, const nlohmann::json& doc) -> ProjectInfo
    {
        const std::filesystem::path root =
            path.parent_path().empty() ? std::filesystem::path{ "." } : path.parent_path();
        const std::string fallbackName = root.filename().string().empty() ? "project" : root.filename().string();
        ProjectInfo project;
        project.loaded = true;
        project.root = root.string();
        project.path = path.string();
        project.name = jsonStringOr(doc, "name", fallbackName);
        if (!validProjectName(project.name))
        {
            project.name = validProjectName(fallbackName) ? fallbackName : "project";
        }
        project.displayName = jsonStringOr(doc, "displayName", defaultDisplayName(project.name));
        return project;
    }

    auto projectInfoJson(const ProjectInfo& project) -> nlohmann::json
    {
        return nlohmann::json{ { "loaded", project.loaded },
                               { "root", project.root },
                               { "path", project.path },
                               { "name", project.name },
                               { "displayName", project.displayName } };
    }

    auto assetTypeName(AssetType type) -> const char*
    {
        if (type == AssetType::Texture)
        {
            return "texture";
        }
        if (type == AssetType::Other)
        {
            return "other";
        }
        return "mesh";
    }

    auto assetTypeFromName(const std::string& name) -> AssetType
    {
        if (name == "texture")
        {
            return AssetType::Texture;
        }
        if (name == "other")
        {
            return AssetType::Other;
        }
        return AssetType::Mesh;
    }

    auto catalogToJson(const AssetCatalog& catalog) -> nlohmann::json
    {
        nlohmann::json assets = nlohmann::json::array();
        for (const AssetEntry& entry : catalog.entries)
        {
            assets.push_back(nlohmann::json{ { "id", uuidToJson(entry.id.value) },
                                             { "name", entry.name },
                                             { "type", assetTypeName(entry.type) },
                                             { "path", entry.path },
                                             { "hdr", entry.hdr } });
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
            parsed.id = Uuid{ jsonU64Or(entry, "id", 0) };
            parsed.name = jsonStringOr(entry, "name", std::string{});
            parsed.type = assetTypeFromName(jsonStringOr(entry, "type", std::string{ "mesh" }));
            parsed.path = jsonStringOr(entry, "path", std::string{});
            parsed.hdr = jsonBoolOr(entry, "hdr", false);
            if (parsed.id.value != 0)
            {
                putAsset(catalog, std::move(parsed));
            }
        }
    }

    // Creates the asset root (+ subdirs) and migrates any legacy asset_registry.json
    // into the catalog with synthesized names. The catalog is otherwise loaded from a
    // project file via loadProject.
    auto newAssetServer(std::string root) -> AssetServer
    {
        AssetServer assets;
        assets.root = std::move(root);
        std::error_code ec;
        std::filesystem::create_directories(assets.root + "/meshes", ec);
        std::filesystem::create_directories(assets.root + "/models", ec);
        std::filesystem::create_directories(assets.root + "/textures", ec);

        std::ifstream in(assets.root + "/asset_registry.json");
        if (in)
        {
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto parsedDoc = parseJson(text);
            if (parsedDoc)
            {
                const nlohmann::json& doc = *parsedDoc;
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

    void setAssetRoot(AssetServer& assets, const std::string& root)
    {
        assets.root = root;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(assets.root) / "models", ec);
        std::filesystem::create_directories(std::filesystem::path(assets.root) / "textures", ec);
    }

    void ensureAssetDirectories(const AssetServer& assets)
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(assets.root) / "models", ec);
        std::filesystem::create_directories(std::filesystem::path(assets.root) / "textures", ec);
    }

    void clearAssetCaches(AssetServer& assets)
    {
        assets.meshRefByUuid.clear();
        assets.textureRefByUuid.clear();
    }

    // Constant version for the unified project document.
    inline constexpr int ProjectVersion = 1;

    // Saves the whole project (asset catalog + scene entities) to one JSON file.
    auto saveProject(AssetServer& assets, ComponentRegistry& reg, Scene& scene, const ProjectInfo& project,
                     const std::string& path) -> Result<void>
    {
        const std::string target = path.empty() ? project.path : path;
        if (target.empty())
        {
            return Err(std::string{ "no active project path" });
        }
        nlohmann::json doc;
        doc["version"] = ProjectVersion;
        doc["name"] = project.name;
        doc["displayName"] = project.displayName;
        doc["assets"] = catalogToJson(assets.catalog);
        doc["scene"] = sceneToJson(reg, scene);

        const std::filesystem::path parent = std::filesystem::path(target).parent_path();
        if (!parent.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
        std::ofstream out(target);
        if (!out)
        {
            return Err(std::format("cannot open '{}' for writing", target));
        }
        out << dumpJson(doc, 2);
        out.flush();
        if (!out)
        {
            return Err(std::format("write failed for '{}'", target));
        }
        return {};
    }

    // Loads a project file: replaces the catalog + scene. Clears the GPU caches (after a
    // device idle) so stale Refs are dropped and assets re-resolve from the new catalog.
    auto loadProject(AssetServer& assets, Renderer& renderer, ComponentRegistry& reg, Scene& scene,
                     ProjectInfo& project, const std::string& selection) -> Result<void>
    {
        const std::filesystem::path path = projectJsonPath(selection);
        std::ifstream in(path);
        if (!in)
        {
            return Err(std::format("cannot open '{}'", path.string()));
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto parsedDoc = parseJson(text);
        if (!parsedDoc || !parsedDoc->is_object())
        {
            return Err(std::format("'{}': JSON parse error", path.string()));
        }
        const nlohmann::json& doc = *parsedDoc;
        const int version = static_cast<int>(jsonU64Or(doc, "version", 0));
        if (version != ProjectVersion)
        {
            return Err(std::format("unsupported project version {}", version));
        }

        waitGpuIdle(renderer);
        clearAssetCaches(assets);
        project = projectInfoFromPath(path, doc);
        setAssetRoot(assets, (std::filesystem::path(project.root) / "assets").string());
        catalogFromJson(assets.catalog, doc.value("assets", nlohmann::json::array()));
        return sceneFromJson(reg, scene, doc.value("scene", nlohmann::json::object()));
    }

    auto createProject(AssetServer& assets, Renderer& renderer, ComponentRegistry& reg, Scene& scene,
                       ProjectInfo& project, const std::string& name, const std::string& displayName,
                       const std::string& rootOverride = "") -> Result<void>
    {
        if (!validProjectName(name))
        {
            return Err(std::format("invalid project name '{}'", name));
        }
        std::filesystem::path root = rootOverride.empty() ? std::filesystem::path(projectUserdataRoot()) / name
                                                          : std::filesystem::path(rootOverride);
        ProjectInfo next;
        next.loaded = true;
        next.root = root.string();
        next.path = (root / "project.json").string();
        next.name = name;
        next.displayName = displayName.empty() ? defaultDisplayName(name) : displayName;

        waitGpuIdle(renderer);
        scene = Scene{};
        assets.catalog.entries.clear();
        assets.catalog.byId.clear();
        clearAssetCaches(assets);
        setAssetRoot(assets, (root / "assets").string());
        project = std::move(next);
        return saveProject(assets, reg, scene, project, project.path);
    }

    auto createAutoEmptyProject(AssetServer& assets, Renderer& renderer, ComponentRegistry& reg, Scene& scene,
                                ProjectInfo& project) -> Result<void>
    {
        const std::string socket =
            std::getenv("SAFFRON_CONTROL_SOCK") != nullptr ? std::getenv("SAFFRON_CONTROL_SOCK") : "";
        std::string suffix =
            std::to_string(std::hash<std::string>{}(std::filesystem::current_path().string() + socket));
        const std::string name = "auto-empty-" + suffix.substr(0, 12);
        return createProject(assets, renderer, reg, scene, project, name, "Auto Empty Project");
    }

    // Writes encoded image bytes into assets/textures/<uuid>.<ext>, decodes + uploads
    // them, and adds a Texture entry to the catalog (named, deduped). Returns the id.
    auto registerTextureBytes(AssetServer& assets, Renderer& renderer, const std::vector<u8>& encoded,
                              const std::string& ext, const std::string& name) -> Result<Uuid>
    {
        auto decoded = decodeImageFromMemory(encoded);
        if (!decoded)
        {
            return Err(decoded.error());
        }
        auto texture = uploadTexture(renderer, decoded->rgba.data(), decoded->width, decoded->height, true);
        if (!texture)
        {
            return Err(texture.error());
        }
        const Uuid id = newUuid();
        std::string extension = ext;
        if (extension.empty())
        {
            extension = "png";
        }
        ensureAssetDirectories(assets);
        const std::string relativePath = "textures/" + std::to_string(id.value) + "." + extension;
        std::ofstream out(assets.root + "/" + relativePath, std::ios::binary);
        if (!out)
        {
            return Err(std::format("cannot write texture '{}'", relativePath));
        }
        out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        if (!out)
        {
            return Err(std::format("write failed for texture '{}'", relativePath));
        }
        putAsset(assets.catalog, AssetEntry{ id, uniqueName(assets.catalog, name), AssetType::Texture, relativePath });
        assets.textureRefByUuid[id.value] = *texture;
        return id;
    }

    // Writes encoded HDR bytes into assets/textures/<uuid>.hdr, decodes + uploads them as a
    // linear float texture, and adds a Texture entry with hdr=true. Returns the id.
    auto registerHdrTextureBytes(AssetServer& assets, Renderer& renderer, const std::vector<u8>& encoded,
                                 const std::string& name) -> Result<Uuid>
    {
        auto decoded = decodeImageFromMemoryHdr(encoded);
        if (!decoded)
        {
            return Err(decoded.error());
        }
        auto texture = uploadTextureFloat(renderer, decoded->rgba.data(), decoded->width, decoded->height);
        if (!texture)
        {
            return Err(texture.error());
        }
        const Uuid id = newUuid();
        ensureAssetDirectories(assets);
        const std::string relativePath = "textures/" + std::to_string(id.value) + ".hdr";
        std::ofstream out(assets.root + "/" + relativePath, std::ios::binary);
        if (!out)
        {
            return Err(std::format("cannot write texture '{}'", relativePath));
        }
        out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        if (!out)
        {
            return Err(std::format("write failed for texture '{}'", relativePath));
        }
        putAsset(assets.catalog,
                 AssetEntry{ id, uniqueName(assets.catalog, name), AssetType::Texture, relativePath, true });
        assets.textureRefByUuid[id.value] = *texture;
        return id;
    }

    // Imports an external image file into the asset dir + catalog (name = filename stem).
    auto importTexture(AssetServer& assets, Renderer& renderer, const std::string& path) -> Result<Uuid>
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
        {
            return Err(std::format("cannot open '{}'", path));
        }
        const std::streamsize size = in.tellg();
        in.seekg(0);
        std::vector<u8> encoded(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(encoded.data()), size);
        if (!in)
        {
            return Err(std::format("read failed for '{}'", path));
        }
        const std::filesystem::path fsPath{ path };
        std::string ext = fsPath.extension().string();  // ".png" -> drop the dot
        if (!ext.empty())
        {
            ext.erase(0, 1);
        }
        std::string extLower = ext;
        for (char& c : extLower)
        {
            if (c >= 'A' && c <= 'Z')
            {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        if (extLower == "hdr")
        {
            return registerHdrTextureBytes(assets, renderer, encoded, fsPath.stem().string());
        }
        return registerTextureBytes(assets, renderer, encoded, ext, fsPath.stem().string());
    }

    // Resolves a texture id to a GPU texture, decoding + uploading the copied file on
    // a cache miss. Returns a null Ref (negative-cached) for an unregistered or
    // unreadable asset.
    auto loadTextureAsset(AssetServer& assets, Renderer& renderer, Uuid id) -> Ref<GpuTexture>
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
        const std::string fullPath = assets.root + "/" + entry->path;
        if (entry->hdr)
        {
            auto decoded = decodeImageHdr(fullPath);
            if (decoded)
            {
                auto texture = uploadTextureFloat(renderer, decoded->rgba.data(), decoded->width, decoded->height);
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
        auto decoded = decodeImage(fullPath);
        if (decoded)
        {
            auto texture = uploadTexture(renderer, decoded->rgba.data(), decoded->width, decoded->height, true);
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
    auto importModel(AssetServer& assets, Renderer& renderer, const std::string& path) -> Result<ImportResult>
    {
        auto model = importModelWithMaterial(path);
        if (!model)
        {
            return Err(model.error());
        }
        const std::string baseName = std::filesystem::path(path).stem().string();
        const Uuid meshId = newUuid();
        ensureAssetDirectories(assets);
        const std::string relativePath = "models/" + std::to_string(meshId.value) + ".smesh";
        if (Result<void> baked = saveMesh(model->mesh, assets.root + "/" + relativePath); !baked)
        {
            return Err(baked.error());
        }
        auto meshRef = uploadMesh(renderer, model->mesh);
        if (!meshRef)
        {
            return Err(meshRef.error());
        }
        putAsset(assets.catalog,
                 AssetEntry{ meshId, uniqueName(assets.catalog, baseName), AssetType::Mesh, relativePath });
        assets.meshRefByUuid[meshId.value] = *meshRef;

        ImportResult result;
        result.mesh = meshId;
        result.baseColor = model->material.baseColor;
        if (model->material.hasAlbedo)
        {
            auto texture = registerTextureBytes(assets, renderer, model->material.albedoBytes,
                                                model->material.albedoExt, baseName + " albedo");
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
    auto loadMeshAsset(AssetServer& assets, Renderer& renderer, Uuid id) -> Ref<GpuMesh>
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
        std::string fullPath = assets.root + "/" + entry->path;
        if (!std::filesystem::exists(fullPath) && entry->path.starts_with("meshes/"))
        {
            fullPath = assets.root + "/models/" + entry->path.substr(std::string{ "meshes/" }.size());
        }
        auto mesh = loadMesh(fullPath);
        if (mesh)
        {
            auto meshRef = uploadMesh(renderer, *mesh);
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
    auto spawnMesh(Scene& scene, std::string name, Uuid mesh) -> Entity
    {
        Entity entity = createEntity(scene, std::move(name));
        addComponent<MeshComponent>(scene, entity).mesh = mesh;
        return entity;
    }

    // Creates an entity from an import: a mesh + a material (base color + albedo).
    auto spawnModel(Scene& scene, std::string name, const ImportResult& result) -> Entity
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
    void renderScene(Renderer& renderer, Scene& scene, AssetServer& assets, const CameraView& camera)
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
        forEach<DirectionalLightComponent>(scene,
                                           [&](Entity, DirectionalLightComponent& light)
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

        // Gather punctual (point + spot) lights, positioned by their Transform. Track the
        // first spot light's index + its perspective light-space transform so it can cast
        // a shadow (the one shadowed spot in v1).
        std::vector<GpuLight> lights;
        bool havePointShadow = false;
        glm::vec3 pointShadowPos{ 0.0f };
        f32 pointShadowFar = 1.0f;
        u32 pointShadowIndex = 0;
        forEach<TransformComponent, PointLightComponent>(
            scene,
            [&](Entity, TransformComponent& transform, PointLightComponent& light)
            {
                GpuLight gpu;
                gpu.positionRange = glm::vec4(transform.translation, light.range);
                gpu.colorIntensity = glm::vec4(light.color, light.intensity);
                gpu.directionType = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);  // type 0 = point
                gpu.spotCos = glm::vec4(0.0f);
                if (!havePointShadow)
                {
                    pointShadowPos = transform.translation;
                    pointShadowFar = glm::max(light.range, 0.1f);
                    pointShadowIndex = static_cast<u32>(lights.size());
                    havePointShadow = true;
                }
                lights.push_back(gpu);
            });
        bool haveSpotShadow = false;
        glm::mat4 spotShadowViewProj{ 1.0f };
        u32 spotShadowIndex = 0;
        forEach<TransformComponent, SpotLightComponent>(
            scene,
            [&](Entity, TransformComponent& transform, SpotLightComponent& light)
            {
                const glm::vec3 dir = glm::normalize(light.direction);
                GpuLight gpu;
                gpu.positionRange = glm::vec4(transform.translation, light.range);
                gpu.colorIntensity = glm::vec4(light.color, light.intensity);
                gpu.directionType = glm::vec4(dir, 1.0f);  // type 1 = spot
                gpu.spotCos = glm::vec4(glm::cos(glm::radians(light.innerAngle)),
                                        glm::cos(glm::radians(light.outerAngle)), 0.0f, 0.0f);
                if (!haveSpotShadow)
                {
                    // A perspective frustum down the spot cone: fov = 2 x outer angle (a
                    // small pad so the penumbra is inside the map), aspect 1, near/far from range.
                    const f32 fov = glm::radians(glm::min(2.0f * light.outerAngle + 2.0f, 179.0f));
                    const glm::vec3 up =
                        glm::abs(dir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
                    const glm::mat4 lightView = glm::lookAt(transform.translation, transform.translation + dir, up);
                    // GLM_FORCE_DEPTH_ZERO_TO_ONE => Vulkan [0,1] clip depth.
                    const glm::mat4 lightProj = glm::perspective(fov, 1.0f, 0.05f, glm::max(light.range, 0.1f));
                    spotShadowViewProj = lightProj * lightView;
                    spotShadowIndex = static_cast<u32>(lights.size());
                    haveSpotShadow = true;
                }
                lights.push_back(gpu);
            });
        setSpotShadow(renderer, spotShadowViewProj, spotShadowIndex, haveSpotShadow);
        setPointShadow(renderer, pointShadowPos, pointShadowFar, pointShadowIndex, havePointShadow);
        // The camera world position is the inverse-view translation; the BRDF needs it
        // as the view-vector origin for specular. The lighting upload happens after the
        // draw loop, once the scene AABB (hence the shadow frustum) is known.
        const glm::vec3 eyePosition = glm::vec3(glm::inverse(view)[3]);

        // Gather the scene's renderables as a flat draw list; the renderer batches them
        // by (mesh, texture) and the scene + depth passes consume the result. The world
        // AABB accumulated here fits the directional shadow frustum below.
        std::vector<DrawItem> items;
        glm::vec3 sceneMin{ std::numeric_limits<f32>::max() };
        glm::vec3 sceneMax{ std::numeric_limits<f32>::lowest() };
        // Per-draw world AABBs + albedo for the DDGI voxel proxy.
        std::vector<glm::vec4> boxMins;
        std::vector<glm::vec4> boxMaxs;
        std::vector<glm::vec4> boxAlbedos;
        forEach<TransformComponent, MeshComponent>(
            scene,
            [&](Entity entity, TransformComponent& transform, MeshComponent& mesh)
            {
                auto meshRef = loadMeshAsset(assets, renderer, mesh.mesh);
                if (!meshRef)
                {
                    return;
                }
                glm::vec4 baseColor{ 1.0f };
                Ref<GpuTexture> textureRef;
                bool unlit = false;
                f32 metallic = 0.0f;
                f32 roughness = 1.0f;
                glm::vec3 emissive{ 0.0f };
                f32 emissiveStrength = 1.0f;
                if (hasComponent<MaterialComponent>(scene, entity))
                {
                    const MaterialComponent& material = getComponent<MaterialComponent>(scene, entity);
                    baseColor = material.baseColor;
                    unlit = material.unlit;
                    metallic = material.metallic;
                    roughness = material.roughness;
                    emissive = material.emissive;
                    emissiveStrength = material.emissiveStrength;
                    if (material.albedoTexture.value != 0)
                    {
                        textureRef = loadTextureAsset(assets, renderer, material.albedoTexture);
                    }
                }
                const glm::mat4 model = transformMatrix(transform);
                // Accumulate the scene + this draw's world AABB from the 8 transformed corners.
                glm::vec3 boxMin{ std::numeric_limits<f32>::max() };
                glm::vec3 boxMax{ std::numeric_limits<f32>::lowest() };
                for (u32 corner = 0; corner < 8; corner = corner + 1)
                {
                    glm::vec3 p = meshRef->boundsMin;
                    if (corner & 1u)
                    {
                        p.x = meshRef->boundsMax.x;
                    }
                    if (corner & 2u)
                    {
                        p.y = meshRef->boundsMax.y;
                    }
                    if (corner & 4u)
                    {
                        p.z = meshRef->boundsMax.z;
                    }
                    const glm::vec3 world = glm::vec3(model * glm::vec4(p, 1.0f));
                    sceneMin = glm::min(sceneMin, world);
                    sceneMax = glm::max(sceneMax, world);
                    boxMin = glm::min(boxMin, world);
                    boxMax = glm::max(boxMax, world);
                }
                boxMins.push_back(glm::vec4(boxMin, 0.0f));
                boxMaxs.push_back(glm::vec4(boxMax, 0.0f));
                boxAlbedos.push_back(glm::vec4(glm::vec3(baseColor), 0.0f));
                DrawItem item;
                item.mesh = meshRef;
                item.texture = textureRef;
                item.model = model;
                item.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(model))));
                item.baseColor = baseColor;
                item.metallic = metallic;
                item.roughness = roughness;
                item.emissive = emissive;
                item.emissiveStrength = emissiveStrength;
                item.material.unlit = unlit;
                items.push_back(std::move(item));
            });

        // Fit an orthographic shadow frustum to the scene's world AABB, looking down the
        // directional light. A bounding sphere keeps the fit rotation-stable.
        const bool castShadow = !items.empty() && sceneMax.x >= sceneMin.x;
        glm::mat4 shadowViewProj{ 1.0f };
        if (castShadow)
        {
            const glm::vec3 center = (sceneMin + sceneMax) * 0.5f;
            const f32 radius = glm::length(sceneMax - sceneMin) * 0.5f + 0.5f;
            const glm::vec3 dir = glm::normalize(lightDir);
            const glm::vec3 up = glm::abs(dir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
            const glm::vec3 eye = center - dir * (radius + 1.0f);
            const glm::mat4 lightView = glm::lookAt(eye, center, up);
            // GLM_FORCE_DEPTH_ZERO_TO_ONE => glm::ortho already emits Vulkan [0,1] clip depth.
            const glm::mat4 lightProj = glm::ortho(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + 2.0f);
            shadowViewProj = lightProj * lightView;
        }
        setDirectionalShadow(renderer, shadowViewProj, castShadow);
        // RT: hand the frame's instance transforms + meshes to the renderer for the per-frame
        // TLAS build (used by ray-query shadows when enabled + supported).
        {
            std::vector<glm::mat4> rtModels;
            std::vector<Ref<GpuMesh>> rtMeshes;
            rtModels.reserve(items.size());
            rtMeshes.reserve(items.size());
            for (const DrawItem& it : items)
            {
                rtModels.push_back(it.model);
                rtMeshes.push_back(it.mesh);
            }
            setRtScene(renderer, std::move(rtModels), std::move(rtMeshes));
        }
        // DDGI: fit the probe volume to the scene AABB (padded a little so probes sit just
        // outside the geometry), upload the box proxy, and pass the sun for the trace.
        // Done before setSceneLighting, which reads the volume placement into the light UBO.
        if (!items.empty() && sceneMax.x >= sceneMin.x)
        {
            const glm::vec3 pad{ 1.0f };
            const glm::vec3 volMin = sceneMin - pad;
            const glm::vec3 volExt = (sceneMax + pad) - volMin;
            // The DDGI indirect bounce reads the scene's sky/ambient instead of a hardcoded blue.
            glm::vec3 ddgiSky{ 0.1f, 0.13f, 0.2f };
            if (scene.environment.useSkyForAmbient)
            {
                ddgiSky = scene.environment.ambientColor * scene.environment.ambientIntensity;
            }
            setDdgiScene(renderer, boxMins, boxMaxs, boxAlbedos, volMin, volExt, lightDir, lightColor, lightIntensity,
                         ddgiSky);
        }
        // Fallback ambient (used when IBL is off): the scene environment's ambient color when
        // useSkyForAmbient, else the directional light's legacy scalar ambient (grayscale).
        glm::vec3 ambient{ lightAmbient };
        if (scene.environment.useSkyForAmbient)
        {
            ambient = scene.environment.ambientColor * scene.environment.ambientIntensity;
        }
        setSceneLighting(renderer, lightDir, lightColor, lightIntensity, ambient, eyePosition, lights);
        // Drive the environment bake. Procedural derives the sky from the directional light (the
        // sun sits opposite the light's travel direction); Texture mode routes the same panorama
        // that drives the visible sky into the IBL too (loaded once, shared below). requestEnvBake
        // no-ops unless the source/panorama/sun changed (re-bake happens in beginFrameGraph).
        Ref<GpuTexture> skyPanorama;
        {
            const SceneEnvironment& env = scene.environment;
            SkygenParams skyBake;
            skyBake.sunDir = -lightDir;
            skyBake.sunColor = lightColor;
            skyBake.sunIntensity = lightIntensity;
            if (env.skyMode == SkyMode::Texture && env.skyTexture.value != 0)
            {
                skyPanorama = loadTextureAsset(assets, renderer, env.skyTexture);
                if (skyPanorama)
                {
                    requestEnvBake(renderer, EnvSource::Equirect, skyPanorama, skyBake);
                }
                else
                {
                    requestEnvBake(renderer, EnvSource::Procedural, nullptr, skyBake);
                }
            }
            else
            {
                requestEnvBake(renderer, EnvSource::Procedural, nullptr, skyBake);
            }
        }
        setClusterCamera(renderer, view, proj, camera.nearPlane, camera.farPlane);  // arms the cull dispatch
        // Screen-space passes (G-buffer/GTAO/contact/SSGI) use the scene's view/proj + the
        // directional light direction (for contact shadows).
        setSsaoCamera(renderer, view, proj, lightDir);

        submitDrawList(renderer, viewProjection, items);

        // Resolve the scene environment into the visible-sky settings. Procedural samples the
        // baked envCube (so the background matches the IBL lighting); Texture loads the
        // panorama into the bindless array and passes its slot; Color is a flat fill.
        {
            const SceneEnvironment& env = scene.environment;
            SkyRenderSettings sky;
            sky.mode = static_cast<u32>(env.skyMode);
            sky.clearColor = env.clearColor;
            sky.intensity = env.skyIntensity;
            sky.rotation = env.skyRotation;
            sky.visible = env.visible;
            if (env.skyMode == SkyMode::Texture && env.skyTexture.value != 0)
            {
                if (skyPanorama)
                {
                    sky.textureIndex = skyPanorama->bindlessIndex;
                }
                else
                {
                    sky.mode = static_cast<u32>(SkyMode::Color);  // missing panorama -> clear color
                }
            }
            submitSky(renderer, sky);
        }
    }

    // Picks the nearest entity whose world-space mesh AABB the camera ray hits. `ndc` is
    // the click point in clip space [-1,1] matching the rendered image (Y-flipped proj).
    // Returns a null Entity on a miss (the caller clears the selection).
    auto pickEntity(Scene& scene, AssetServer& assets, Renderer& renderer, const CameraView& camera, glm::vec2 ndc)
        -> Entity
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
        forEach<TransformComponent, MeshComponent>(
            scene,
            [&](Entity entity, TransformComponent& transform, MeshComponent& mesh)
            {
                auto meshRef = loadMeshAsset(assets, renderer, mesh.mesh);
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
                    if (corner & 1u)
                    {
                        p.x = hi.x;
                    }
                    if (corner & 2u)
                    {
                        p.y = hi.y;
                    }
                    if (corner & 4u)
                    {
                        p.z = hi.z;
                    }
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
