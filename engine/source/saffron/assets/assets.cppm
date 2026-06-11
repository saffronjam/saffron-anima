module;

// Bridges Scene + Geometry + Rendering, so (like those) it uses classic includes.
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
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
    struct SystemMeshVisual
    {
        bool attempted = false;
        Ref<GpuMesh> mesh;
        std::vector<SubmeshMaterial> submeshMaterials;
    };

    struct RenderSceneOptions
    {
        bool showEditorCameraModels = false;
    };

    // Owns the project's asset catalog (id -> {name, type, path}) plus uuid-keyed GPU
    // caches so entities sharing an id upload once. A cached null Ref is the
    // negative-cache marker — a failed asset is not retried each frame.
    struct AssetServer
    {
        std::string root;
        AssetCatalog catalog;                                       // source of truth: id -> {name,type,path}
        std::unordered_map<u64, Ref<GpuMesh>> meshRefByUuid;        // GPU cache
        std::unordered_map<u64, Ref<GpuTexture>> textureRefByUuid;  // GPU cache
        SystemMeshVisual editorCameraModel;
    };

    // The native material asset (.smat): a reference-only property bag over the übershader. It
    // bakes nothing — texture references are catalog Uuids, and colorspace / normal convention
    // are recorded on the referenced texture's AssetEntry. Resolved to a SubmeshMaterial +
    // MaterialParams at draw time (a later phase). Grows with the PBR slot set.
    struct MaterialAsset
    {
        std::string shader = "mesh";   // übershader family selector
        std::string blend = "opaque";  // opaque | masked | translucent (PSO axis)
        bool unlit = false;
        bool doubleSided = false;
        glm::vec4 baseColor{ 1.0f };
        f32 metallic = 0.0f;
        f32 roughness = 1.0f;
        glm::vec3 emissive{ 0.0f };
        f32 emissiveStrength = 1.0f;
        f32 normalStrength = 1.0f;
        f32 alphaCutoff = 0.5f;
        f32 heightScale = 0.05f;
        glm::vec2 uvTiling{ 1.0f };
        glm::vec2 uvOffset{ 0.0f };
        Uuid albedoTexture;
        Uuid ormTexture;       // packed AO/roughness/metallic (or a standalone metallic-roughness)
        Uuid normalTexture;
        Uuid emissiveTexture;
        Uuid heightTexture;
        std::string normalConvention = "gl";  // gl | dx — baked to gl at import; recorded for provenance
        u32 features = 0;      // resolved feature bitset (populated from the PBR-slots phase)
        // Optional node graph (the editable source of truth for a graph-authored material). When a
        // foldable graph is present, the resolved factors/textures above are derived from it; a
        // non-foldable graph (procedural nodes) is the codegen path. Null/empty = no graph.
        nlohmann::json graph;
        // Material instance: an optional parent material + a sparse map of overridden fields. When
        // `parent` is set this material resolves to the parent's params with `overrides` applied on
        // top (edit-once-propagate: editing the parent reflows every instance). 0 = a master material.
        Uuid parent;
        nlohmann::json overrides;  // { fieldName: value } — only the fields this instance overrides
    };

    // The built-in default material: white albedo, fully rough, non-metallic. Returned by the
    // resolve path when a referenced material is missing. Its id is in the reserved (< 1024) range.
    inline constexpr Uuid DefaultMaterialId{ 1 };
    inline auto defaultMaterialAsset() -> MaterialAsset
    {
        return MaterialAsset{};
    }

    // What importModel produces: the imported mesh + its primary material, and — for a
    // rigged glTF — the node forest + skin descriptor spawnSkinnedModel instantiates
    // as bone entities.
    struct ImportResult
    {
        Uuid mesh;
        glm::vec4 baseColor{ 1.0f };
        Uuid albedoTexture;  // 0 == none
        // The imported material table (textures already registered). slot 0 mirrors
        // baseColor/albedoTexture above. >1 entry spawns a MaterialSetComponent.
        std::vector<MaterialSlot> materials;
        bool hasSkin = false;
        std::vector<ImportedNode> nodes;
        ImportedSkin skinDesc;
        std::vector<Uuid> animations;  // registered AssetType::Animation clip ids (skinned imports)
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
        if (type == AssetType::Animation)
        {
            return "animation";
        }
        if (type == AssetType::Material)
        {
            return "material";
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
        if (name == "animation")
        {
            return AssetType::Animation;
        }
        if (name == "material")
        {
            return AssetType::Material;
        }
        if (name == "mesh")
        {
            return AssetType::Mesh;
        }
        // An unknown type-string is a forward-compat entry from a newer build; keep it
        // around as Other rather than mis-treating it as a renderable mesh.
        return AssetType::Other;
    }

    auto catalogToJson(const AssetCatalog& catalog) -> nlohmann::json
    {
        nlohmann::json assets = nlohmann::json::array();
        for (const AssetEntry& entry : catalog.entries)
        {
            nlohmann::json record{ { "id", uuidToJson(entry.id.value) },
                                   { "name", entry.name },
                                   { "type", assetTypeName(entry.type) },
                                   { "path", entry.path },
                                   { "folder", entry.folder },
                                   { "hdr", entry.hdr },
                                   { "linear", entry.linear } };
            // Carry clip length on animation rows so the timeline + list-clips can report
            // duration without opening the .sanim; non-animation rows stay byte-identical.
            if (entry.type == AssetType::Animation)
            {
                record["duration"] = entry.duration;
            }
            assets.push_back(std::move(record));
        }
        return assets;
    }

    auto catalogFoldersToJson(const AssetCatalog& catalog) -> nlohmann::json
    {
        nlohmann::json folders = nlohmann::json::array();
        for (const std::string& folder : catalog.folders)
        {
            folders.push_back(folder);
        }
        return folders;
    }

    void catalogFoldersFromJson(AssetCatalog& catalog, const nlohmann::json& folders)
    {
        catalog.folders.clear();
        if (!folders.is_array())
        {
            return;
        }
        for (const nlohmann::json& folder : folders)
        {
            if (folder.is_string())
            {
                catalog.folders.push_back(folder.get<std::string>());
            }
        }
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
            parsed.folder = jsonStringOr(entry, "folder", std::string{});
            parsed.hdr = jsonBoolOr(entry, "hdr", false);
            parsed.linear = jsonBoolOr(entry, "linear", false);
            parsed.duration = jsonF32Or(entry, "duration", 0.0f);
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
        std::filesystem::create_directories(std::filesystem::path(assets.root) / "materials", ec);
    }

    void ensureAssetDirectories(const AssetServer& assets)
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(assets.root) / "models", ec);
        std::filesystem::create_directories(std::filesystem::path(assets.root) / "textures", ec);
        std::filesystem::create_directories(std::filesystem::path(assets.root) / "materials", ec);
    }

    void clearAssetCaches(AssetServer& assets)
    {
        assets.meshRefByUuid.clear();
        assets.textureRefByUuid.clear();
    }

    // Constant version for the unified project document.
    inline constexpr int ProjectVersion = 1;

    // The renderer settings the editor's render panel drives, as a project-file block.
    auto renderSettingsToJson(Renderer& renderer) -> nlohmann::json
    {
        return nlohmann::json{ { "aa", aaMode(renderer) },
                               { "exposureEv", exposureEv(renderer) },
                               { "clustered", clusteredEnabled(renderer) },
                               { "depthPrepass", depthPrepassEnabled(renderer) },
                               { "shadows", shadowsEnabled(renderer) },
                               { "ibl", iblEnabled(renderer) },
                               { "ssao", ssaoEnabled(renderer) },
                               { "contactShadows", contactShadowsEnabled(renderer) },
                               { "ssgi", ssgiEnabled(renderer) },
                               { "ddgi", ddgiEnabled(renderer) },
                               { "rtShadows", rtShadowsEnabled(renderer) },
                               { "restir", restirEnabled(renderer) } };
    }

    // Applies a saved renderSettings block; missing fields keep the current value, and the
    // RT toggles only apply where the device supports ray tracing.
    void applyRenderSettings(Renderer& renderer, const nlohmann::json& settings)
    {
        if (!settings.is_object())
        {
            return;
        }
        if (settings.contains("aa") && settings["aa"].is_string())
        {
            setAaMode(renderer, settings["aa"].get<std::string>());
        }
        if (settings.contains("exposureEv") && settings["exposureEv"].is_number())
        {
            setExposure(renderer, settings["exposureEv"].get<f32>());
        }
        auto applyBool = [&settings](const char* key, auto&& setter)
        {
            if (settings.contains(key) && settings[key].is_boolean())
            {
                setter(settings[key].get<bool>());
            }
        };
        applyBool("clustered", [&renderer](bool v) { setClustered(renderer, v); });
        applyBool("depthPrepass", [&renderer](bool v) { setDepthPrepass(renderer, v); });
        applyBool("shadows", [&renderer](bool v) { setShadows(renderer, v); });
        applyBool("ibl", [&renderer](bool v) { setIbl(renderer, v); });
        applyBool("ssao", [&renderer](bool v) { setSsao(renderer, v); });
        applyBool("contactShadows", [&renderer](bool v) { setContactShadows(renderer, v); });
        applyBool("ssgi", [&renderer](bool v) { setSsgi(renderer, v); });
        applyBool("ddgi", [&renderer](bool v) { setDdgi(renderer, v); });
        if (rtSupported(renderer))
        {
            applyBool("rtShadows", [&renderer](bool v) { setRtShadows(renderer, v); });
            applyBool("restir", [&renderer](bool v) { setRestir(renderer, v); });
        }
    }

    // Saves the whole project (asset catalog + scene entities + render settings + the editor
    // camera, when the caller passes one) to one JSON file.
    auto saveProject(AssetServer& assets, Renderer& renderer, ComponentRegistry& reg, Scene& scene,
                     const ProjectInfo& project, const std::string& path, const nlohmann::json& editorCamera = {})
        -> Result<void>
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
        doc["assetFolders"] = catalogFoldersToJson(assets.catalog);
        doc["scene"] = sceneToJson(reg, scene);
        doc["renderSettings"] = renderSettingsToJson(renderer);
        if (editorCamera.is_object())
        {
            doc["editorCamera"] = editorCamera;
        }

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

    // Every project carries a src/ for Lua scripts; ScriptComponent slots resolve
    // their paths against it. Idempotent: the folder is ensured on create AND on
    // load (pre-existing projects gain it on open), the example only when absent.
    inline constexpr std::string_view StarterScript =
        R"(-- example.lua: attach to an entity's Script component, then press Play.
-- Orbits the entity in the x/y plane around where it was authored.
local Example = {}

Example.properties = {
  speed = 1.0,  -- radians/second, editable in the Inspector
  radius = 2.0,
}

function Example.on_create(self)
  -- Center one radius left of the authored spot, so the orbit starts exactly
  -- on the entity's position instead of teleporting to the circle.
  local p = self.entity:get_position()
  self.center = { x = p.x - self.radius, y = p.y, z = p.z }
  self.angle = 0
end

function Example.on_update(self, dt)
  self.angle = self.angle + self.speed * dt
  local x = self.center.x + math.cos(self.angle) * self.radius
  local y = self.center.y + math.sin(self.angle) * self.radius
  self.entity:set_position(x, y, self.center.z)
end

return Example
)";

    void ensureScriptSrc(const std::filesystem::path& root)
    {
        std::error_code ec;
        std::filesystem::create_directories(root / "src", ec);
        if (ec)
        {
            logWarn(std::format("project src/ not created: {}", ec.message()));
            return;
        }
        const std::filesystem::path example = root / "src" / "example.lua";
        if (!std::filesystem::exists(example))
        {
            std::ofstream out(example);
            out << StarterScript;
        }
    }

    // The file stem as a Lua identifier for the boilerplate's class table:
    // non-identifier characters become underscores, the first letter upper-cases,
    // and a leading digit gets a prefix ("turret-2" -> Turret_2).
    auto scriptClassName(const std::string& stem) -> std::string
    {
        std::string name;
        for (const char c : stem)
        {
            if ((std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_')
            {
                name.push_back(c);
            }
            else
            {
                name.push_back('_');
            }
        }
        if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front())) != 0)
        {
            name.insert(0, "Script");
        }
        name.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(name.front())));
        return name;
    }

    /// Create <root>/src/<name>.lua with the class-table boilerplate the runtime
    /// expects (a `.lua` suffix is appended when missing; subfolders are allowed,
    /// `..` is not). Errs when the file already exists. Returns the src/-relative
    /// path a ScriptSlot stores.
    auto createProjectScript(const std::string& root, std::string name) -> Result<std::string>
    {
        if (name.empty() || name.find("..") != std::string::npos || name.front() == '/')
        {
            return Err(std::format("invalid script name '{}'", name));
        }
        if (!name.ends_with(".lua"))
        {
            name += ".lua";
        }
        const std::filesystem::path file = std::filesystem::path(root) / "src" / name;
        if (std::filesystem::exists(file))
        {
            return Err(std::format("'{}' already exists", name));
        }
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        if (ec)
        {
            return Err(std::format("cannot create '{}': {}", file.parent_path().string(), ec.message()));
        }
        const std::string className = scriptClassName(file.stem().string());
        std::ofstream out(file);
        if (!out)
        {
            return Err(std::format("cannot write '{}'", file.string()));
        }
        out << std::format(R"(local {0} = {{}}

{0}.properties = {{
  -- speed = 1.0, -- declared fields show up in the Inspector
}}

function {0}.on_create(self)
end

function {0}.on_update(self, dt)
end

return {0}
)",
                           className);
        return name;
    }

    // Loads a project file: replaces the catalog + scene. Clears the GPU caches (after a
    // device idle) so stale Refs are dropped and assets re-resolve from the new catalog.
    // The saved editor-camera block (if any) lands in `editorCamera` for the caller —
    // assets cannot apply it, SceneEdit owns the camera.
    auto loadProject(AssetServer& assets, Renderer& renderer, ComponentRegistry& reg, Scene& scene,
                     ProjectInfo& project, const std::string& selection, nlohmann::json* editorCamera = nullptr)
        -> Result<void>
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
        ensureScriptSrc(project.root);
        catalogFromJson(assets.catalog, doc.value("assets", nlohmann::json::array()));
        catalogFoldersFromJson(assets.catalog, doc.value("assetFolders", nlohmann::json::array()));
        // Older projects have no renderSettings block; the current settings stay.
        if (doc.contains("renderSettings"))
        {
            applyRenderSettings(renderer, doc["renderSettings"]);
        }
        if (editorCamera != nullptr)
        {
            *editorCamera = doc.value("editorCamera", nlohmann::json{});
        }
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
        assets.catalog.folders.clear();
        assets.catalog.byId.clear();
        clearAssetCaches(assets);
        setAssetRoot(assets, (root / "assets").string());
        ensureScriptSrc(root);
        project = std::move(next);
        return saveProject(assets, renderer, reg, scene, project, project.path);
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
                              const std::string& ext, const std::string& name, bool srgb = true) -> Result<Uuid>
    {
        auto decoded = decodeImageFromMemory(encoded);
        if (!decoded)
        {
            return Err(decoded.error());
        }
        auto texture = uploadTexture(renderer, decoded->rgba.data(), decoded->width, decoded->height, srgb);
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
        AssetEntry entry{ id, uniqueName(assets.catalog, name), AssetType::Texture, relativePath, std::string{} };
        entry.linear = !srgb;
        putAsset(assets.catalog, std::move(entry));
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
        putAsset(assets.catalog, AssetEntry{ id, uniqueName(assets.catalog, name), AssetType::Texture, relativePath,
                                             std::string{}, true });
        assets.textureRefByUuid[id.value] = *texture;
        return id;
    }

    // --- Native material asset (.smat) serialization + catalog I/O ---

    auto materialAssetToJson(const MaterialAsset& m) -> nlohmann::json
    {
        const auto u = [](Uuid id) { return std::to_string(id.value); };
        return nlohmann::json{
            { "version", 1 },
            { "shader", m.shader },
            { "blend", m.blend },
            { "unlit", m.unlit },
            { "doubleSided", m.doubleSided },
            { "normalConvention", m.normalConvention },
            { "factors",
              { { "baseColor", { m.baseColor.x, m.baseColor.y, m.baseColor.z, m.baseColor.w } },
                { "metallic", m.metallic },
                { "roughness", m.roughness },
                { "emissive", { m.emissive.x, m.emissive.y, m.emissive.z } },
                { "emissiveStrength", m.emissiveStrength },
                { "normalStrength", m.normalStrength },
                { "alphaCutoff", m.alphaCutoff },
                { "heightScale", m.heightScale },
                { "uvTiling", { m.uvTiling.x, m.uvTiling.y } },
                { "uvOffset", { m.uvOffset.x, m.uvOffset.y } } } },
            { "textures",
              { { "albedo", u(m.albedoTexture) },
                { "ormOrMr", u(m.ormTexture) },
                { "normal", u(m.normalTexture) },
                { "emissive", u(m.emissiveTexture) },
                { "height", u(m.heightTexture) } } },
            { "graph", m.graph.is_null() ? nlohmann::json::object() : m.graph },
            { "parent", std::to_string(m.parent.value) },
            { "overrides", m.overrides.is_null() ? nlohmann::json::object() : m.overrides }
        };
    }

    auto materialAssetFromJson(const nlohmann::json& j) -> MaterialAsset
    {
        MaterialAsset m;
        const auto uuid = [](const nlohmann::json& v) -> Uuid
        {
            if (v.is_string())
            {
                return Uuid{ std::strtoull(v.get<std::string>().c_str(), nullptr, 10) };
            }
            if (v.is_number_unsigned())
            {
                return Uuid{ v.get<u64>() };
            }
            return Uuid{ 0 };
        };
        m.shader = j.value("shader", std::string{ "mesh" });
        m.blend = j.value("blend", std::string{ "opaque" });
        m.unlit = j.value("unlit", false);
        m.doubleSided = j.value("doubleSided", false);
        m.normalConvention = j.value("normalConvention", std::string{ "gl" });
        if (auto fit = j.find("factors"); fit != j.end() && fit->is_object())
        {
            const nlohmann::json& f = *fit;
            if (auto v = f.find("baseColor"); v != f.end() && v->is_array() && v->size() == 4)
            {
                m.baseColor = glm::vec4{ (*v)[0].get<f32>(), (*v)[1].get<f32>(), (*v)[2].get<f32>(), (*v)[3].get<f32>() };
            }
            m.metallic = f.value("metallic", 0.0f);
            m.roughness = f.value("roughness", 1.0f);
            if (auto v = f.find("emissive"); v != f.end() && v->is_array() && v->size() == 3)
            {
                m.emissive = glm::vec3{ (*v)[0].get<f32>(), (*v)[1].get<f32>(), (*v)[2].get<f32>() };
            }
            m.emissiveStrength = f.value("emissiveStrength", 1.0f);
            m.normalStrength = f.value("normalStrength", 1.0f);
            m.alphaCutoff = f.value("alphaCutoff", 0.5f);
            m.heightScale = f.value("heightScale", 0.05f);
            if (auto v = f.find("uvTiling"); v != f.end() && v->is_array() && v->size() == 2)
            {
                m.uvTiling = glm::vec2{ (*v)[0].get<f32>(), (*v)[1].get<f32>() };
            }
            if (auto v = f.find("uvOffset"); v != f.end() && v->is_array() && v->size() == 2)
            {
                m.uvOffset = glm::vec2{ (*v)[0].get<f32>(), (*v)[1].get<f32>() };
            }
        }
        if (auto tit = j.find("textures"); tit != j.end() && tit->is_object())
        {
            const nlohmann::json& t = *tit;
            if (auto v = t.find("albedo"); v != t.end())
            {
                m.albedoTexture = uuid(*v);
            }
            if (auto v = t.find("ormOrMr"); v != t.end())
            {
                m.ormTexture = uuid(*v);
            }
            if (auto v = t.find("normal"); v != t.end())
            {
                m.normalTexture = uuid(*v);
            }
            if (auto v = t.find("emissive"); v != t.end())
            {
                m.emissiveTexture = uuid(*v);
            }
            if (auto v = t.find("height"); v != t.end())
            {
                m.heightTexture = uuid(*v);
            }
        }
        if (auto v = j.find("graph"); v != j.end() && v->is_object() && !v->empty())
        {
            m.graph = *v;
        }
        m.parent = uuid(j.value("parent", nlohmann::json{}));
        if (auto v = j.find("overrides"); v != j.end() && v->is_object() && !v->empty())
        {
            m.overrides = *v;
        }
        return m;
    }

    // Folds a node graph into the flat material params, when every materialOutput channel is driven
    // by a constant or a texture node (no procedural/math nodes). Returns false if any channel needs
    // codegen — the caller then keeps the stored params as a fallback. The graph json is
    // { nodes: [{id,type,props}], edges: [{from:[node,pin], to:[node,pin]}] }.
    auto lowerGraphToParams(const nlohmann::json& graph, MaterialAsset& m) -> bool
    {
        if (!graph.is_object())
        {
            return false;
        }
        const auto nodesIt = graph.find("nodes");
        const auto edgesIt = graph.find("edges");
        if (nodesIt == graph.end() || edgesIt == graph.end() || !nodesIt->is_array() || !edgesIt->is_array())
        {
            return false;
        }
        std::unordered_map<std::string, const nlohmann::json*> byId;
        std::string outputId;
        for (const nlohmann::json& n : *nodesIt)
        {
            byId[n.value("id", std::string{})] = &n;
            if (n.value("type", std::string{}) == "materialOutput")
            {
                outputId = n.value("id", std::string{});
            }
        }
        if (outputId.empty())
        {
            return false;
        }
        const auto uuidOf = [](const nlohmann::json& v) -> Uuid
        {
            if (v.is_string())
            {
                return Uuid{ std::strtoull(v.get<std::string>().c_str(), nullptr, 10) };
            }
            if (v.is_number_unsigned())
            {
                return Uuid{ v.get<u64>() };
            }
            return Uuid{ 0 };
        };
        const auto scalar = [](const nlohmann::json& v) -> f32
        { return v.is_array() && !v.empty() ? v[0].get<f32>() : (v.is_number() ? v.get<f32>() : 0.0f); };
        bool foldable = true;
        for (const nlohmann::json& e : *edgesIt)
        {
            const auto to = e.find("to");
            const auto from = e.find("from");
            if (to == e.end() || from == e.end() || !to->is_array() || to->size() < 2 || !from->is_array() ||
                from->empty() || (*to)[0].get<std::string>() != outputId)
            {
                continue;
            }
            const std::string channel = (*to)[1].get<std::string>();
            const auto srcIt = byId.find((*from)[0].get<std::string>());
            if (srcIt == byId.end())
            {
                foldable = false;
                continue;
            }
            const nlohmann::json& src = *srcIt->second;
            const std::string type = src.value("type", std::string{});
            const nlohmann::json props = src.value("props", nlohmann::json::object());
            if (type == "constant")
            {
                const nlohmann::json value = props.value("value", nlohmann::json::array());
                if (channel == "baseColor" && value.is_array() && value.size() >= 4)
                {
                    m.baseColor = glm::vec4{ value[0].get<f32>(), value[1].get<f32>(), value[2].get<f32>(),
                                             value[3].get<f32>() };
                }
                else if (channel == "emissive" && value.is_array() && value.size() >= 3)
                {
                    m.emissive = glm::vec3{ value[0].get<f32>(), value[1].get<f32>(), value[2].get<f32>() };
                }
                else if (channel == "metallic")
                {
                    m.metallic = scalar(value);
                }
                else if (channel == "roughness")
                {
                    m.roughness = scalar(value);
                }
                else if (channel == "emissiveStrength")
                {
                    m.emissiveStrength = scalar(value);
                }
                else
                {
                    foldable = false;
                }
            }
            else if (type == "texture")
            {
                const Uuid asset = uuidOf(props.value("asset", nlohmann::json{}));
                if (channel == "baseColor")
                {
                    m.albedoTexture = asset;
                }
                else if (channel == "normal")
                {
                    m.normalTexture = asset;
                }
                else if (channel == "emissive")
                {
                    m.emissiveTexture = asset;
                }
                else if (channel == "roughness" || channel == "metallic")
                {
                    m.ormTexture = asset;
                }
                else if (channel == "height")
                {
                    m.heightTexture = asset;
                }
                else
                {
                    foldable = false;
                }
            }
            else
            {
                foldable = false;  // procedural/math node -> needs the codegen path
            }
        }
        return foldable;
    }

    // Emits the body of evalSurface for a node graph: one Slang statement per node (in array
    // order — inputs must precede consumers), then the materialOutput channel assignments. The
    // generated body runs in a shader with `Sampler2D textures[1024]`, a `Mat { float4 baseColor;
    // uint4 tex; }` push constant, and a `float2 uv`. This is the codegen the foldable lowering
    // can't handle (procedural/math nodes).
    auto emitGraphSurface(const nlohmann::json& graph) -> std::string
    {
        std::string body = "    s.albedo = mat.baseColor.rgb;\n    s.metallic = 0.0;\n    s.roughness = 1.0;\n"
                           "    s.normal = float3(0.0, 0.0, 1.0);\n    s.emissive = float3(0.0);\n";
        if (!graph.is_object())
        {
            return body;
        }
        std::unordered_map<std::string, std::string> inputFrom;  // "node:pin" -> source node id
        std::string outputId;
        for (const nlohmann::json& e : graph.value("edges", nlohmann::json::array()))
        {
            const nlohmann::json to = e.value("to", nlohmann::json::array());
            const nlohmann::json from = e.value("from", nlohmann::json::array());
            if (to.size() >= 2 && !from.empty())
            {
                inputFrom[to[0].get<std::string>() + ":" + to[1].get<std::string>()] = from[0].get<std::string>();
            }
        }
        for (const nlohmann::json& n : graph.value("nodes", nlohmann::json::array()))
        {
            const std::string id = n.value("id", std::string{});
            const std::string type = n.value("type", std::string{});
            const nlohmann::json props = n.value("props", nlohmann::json::object());
            if (type == "materialOutput")
            {
                outputId = id;
            }
            else if (type == "constant")
            {
                const nlohmann::json v = props.value("value", nlohmann::json::array());
                const auto at = [&](std::size_t i, f32 d) { return i < v.size() ? v[i].get<f32>() : d; };
                body += std::format("    float4 n_{} = float4({}, {}, {}, {});\n", id, at(0, 0.0f), at(1, 0.0f),
                                    at(2, 0.0f), at(3, 1.0f));
            }
            else if (type == "textureSlot")
            {
                body += std::format("    float4 n_{} = textures[NonUniformResourceIndex(mat.tex.x)].Sample(uv);\n", id);
            }
            else
            {
                // Math / utility nodes. Inputs wired by pin name (a/b/t); all values are float4.
                const auto in = [&](const char* pin) -> std::string
                {
                    const auto it = inputFrom.find(id + ":" + pin);
                    return it != inputFrom.end() ? it->second : std::string{};
                };
                const std::string a = in("a");
                const std::string b = in("b");
                const std::string t = in("t");
                if (type == "multiply")
                {
                    body += std::format("    float4 n_{} = n_{} * n_{};\n", id, a, b);
                }
                else if (type == "add")
                {
                    body += std::format("    float4 n_{} = n_{} + n_{};\n", id, a, b);
                }
                else if (type == "subtract")
                {
                    body += std::format("    float4 n_{} = n_{} - n_{};\n", id, a, b);
                }
                else if (type == "divide")
                {
                    body += std::format("    float4 n_{} = n_{} / max(n_{}, float4(1e-5));\n", id, a, b);
                }
                else if (type == "lerp")
                {
                    body += std::format("    float4 n_{} = lerp(n_{}, n_{}, n_{});\n", id, a, b, t);
                }
                else if (type == "saturate" || type == "clamp")
                {
                    body += std::format("    float4 n_{} = saturate(n_{});\n", id, a);
                }
                else if (type == "oneMinus")
                {
                    body += std::format("    float4 n_{} = 1.0 - n_{};\n", id, a);
                }
                else if (type == "dot")
                {
                    body += std::format("    float4 n_{} = float4(dot(n_{}.rgb, n_{}.rgb));\n", id, a, b);
                }
                else if (type == "uv")
                {
                    body += std::format("    float4 n_{} = float4(uv, 0.0, 1.0);\n", id);
                }
                else if (type == "sin")
                {
                    body += std::format("    float4 n_{} = sin(n_{});\n", id, a);
                }
                else if (type == "cos")
                {
                    body += std::format("    float4 n_{} = cos(n_{});\n", id, a);
                }
                else if (type == "frac")
                {
                    body += std::format("    float4 n_{} = frac(n_{});\n", id, a);
                }
                else if (type == "step")
                {
                    body += std::format("    float4 n_{} = step(n_{}, n_{});\n", id, a, b);
                }
                else if (type == "smoothstep")
                {
                    body += std::format("    float4 n_{} = smoothstep(n_{}, n_{}, n_{});\n", id, a, b, t);
                }
                else
                {
                    body += std::format("    float4 n_{} = float4(0.0);  // unknown node '{}'\n", id, type);
                }
            }
        }
        const auto srcFor = [&](const char* pin) -> std::string
        {
            const auto it = inputFrom.find(outputId + ":" + pin);
            return it != inputFrom.end() ? it->second : std::string{};
        };
        if (auto s = srcFor("baseColor"); !s.empty())
        {
            body += std::format("    s.albedo = n_{}.rgb;\n", s);
        }
        if (auto s = srcFor("metallic"); !s.empty())
        {
            body += std::format("    s.metallic = n_{}.r;\n", s);
        }
        if (auto s = srcFor("roughness"); !s.empty())
        {
            body += std::format("    s.roughness = n_{}.r;\n", s);
        }
        if (auto s = srcFor("emissive"); !s.empty())
        {
            body += std::format("    s.emissive = n_{}.rgb;\n", s);
        }
        return body;
    }

    // Locates slangc (env SAFFRON_SLANGC, the prebuilt cache, then PATH).
    auto findSlangc() -> std::string
    {
        if (const char* env = std::getenv("SAFFRON_SLANGC"); env != nullptr && env[0] != '\0')
        {
            return env;
        }
        if (const char* home = std::getenv("HOME"); home != nullptr)
        {
            const std::string cached = std::string{ home } + "/.cache/saffron-slang/slang/bin/slangc";
            if (std::filesystem::exists(cached))
            {
                return cached;
            }
        }
        return "slangc";  // fall back to PATH
    }

    // Codegen: emits a self-contained shader for a material's node graph and compiles it with slangc
    // to materials/<uuid>.spv. Returns the .spv path. (The per-material PSO render path that loads it
    // is the next step; this proves the graph -> compilable Slang pipeline.)
    auto compileMaterialGraph(AssetServer& assets, const nlohmann::json& graph, Uuid id) -> Result<std::string>
    {
        const std::string slangc = findSlangc();
        const std::string surfaceBody = emitGraphSurface(graph);
        const std::string shader =
            "[[vk::binding(0, 0)]] Sampler2D textures[1024];\n"
            "struct SurfaceData { float3 albedo; float metallic; float roughness; float3 normal; float3 emissive; };\n"
            "struct Mat { float4 baseColor; uint4 tex; };\n"
            "[[vk::push_constant]] Mat mat;\n"
            "SurfaceData evalSurface(float2 uv)\n{\n    SurfaceData s;\n" +
            surfaceBody +
            "    return s;\n}\n"
            "[shader(\"fragment\")] float4 fragmentMain(float2 uv : TEXCOORD0) : SV_Target\n{\n"
            "    SurfaceData s = evalSurface(uv);\n    return float4(s.albedo + s.emissive, 1.0);\n}\n";
        ensureAssetDirectories(assets);
        const std::string slangPath = assets.root + "/materials/" + std::to_string(id.value) + ".slang";
        const std::string spvPath = assets.root + "/materials/" + std::to_string(id.value) + ".spv";
        {
            std::ofstream out(slangPath);
            if (!out)
            {
                return Err(std::format("cannot write generated shader '{}'", slangPath));
            }
            out << shader;
        }
        const std::string cmd = "\"" + slangc + "\" \"" + slangPath +
                                "\" -profile glsl_450 -target spirv -emit-spirv-directly -fvk-use-entrypoint-name "
                                "-matrix-layout-column-major -o \"" +
                                spvPath + "\" > /dev/null 2>&1";
        const int rc = std::system(cmd.c_str());
        if (rc != 0 || !std::filesystem::exists(spvPath))
        {
            return Err(std::format("slangc failed (rc={}) compiling material graph {}", rc, id.value));
        }
        return spvPath;
    }

    // Codegen for the preview pane: emits a full preview shader (the studio-lit sphere) whose
    // evalSurface is the material's graph, compiles it with slangc, and returns the .spv path. The
    // shader's PreviewPush + vertex layout match newPreviewPipeline, so renderMaterialPreview can drive
    // it with the same push + sphere. (A v1 that duplicates the preview lighting; later it should splice
    // the shared preview.slang source.)
    auto compileMaterialPreviewShader(AssetServer& assets, const nlohmann::json& graph, Uuid id) -> Result<std::string>
    {
        const std::string slangc = findSlangc();
        const std::string surfaceBody = emitGraphSurface(graph);
        const std::string shader =
            "[[vk::binding(0, 0)]] Sampler2D textures[1024];\n"
            "struct PreviewPush { float4x4 viewProj; float4 baseColor; uint4 tex; float4 pbr; };\n"
            "[[vk::push_constant]] PreviewPush push;\n"
            "struct SurfaceData { float3 albedo; float metallic; float roughness; float3 normal; float3 emissive; };\n"
            "struct Mat { float4 baseColor; uint4 tex; };\n"
            "SurfaceData evalSurface(float2 uv)\n{\n    Mat mat;\n    mat.baseColor = push.baseColor;\n"
            "    mat.tex = push.tex;\n    SurfaceData s;\n" +
            surfaceBody +
            "    return s;\n}\n"
            "struct VIn { [[vk::location(0)]] float3 position; [[vk::location(1)]] float3 normal; "
            "[[vk::location(2)]] float2 uv0; };\n"
            "struct VOut { float4 position : SV_Position; float3 normal : NORMAL; float2 uv : TEXCOORD0; };\n"
            "[shader(\"vertex\")] VOut vertexMain(VIn input)\n{\n    VOut o;\n"
            "    o.position = mul(push.viewProj, float4(input.position, 1.0));\n    o.normal = input.normal;\n"
            "    o.uv = input.uv0;\n    return o;\n}\n"
            "[shader(\"fragment\")] float4 fragmentMain(VOut input) : SV_Target\n{\n"
            "    SurfaceData s = evalSurface(input.uv);\n    float3 N = normalize(input.normal);\n"
            "    float3 L = normalize(float3(0.5, 0.6, 0.6));\n    float ndotl = max(dot(N, L), 0.0);\n"
            "    float3 c = s.albedo * (ndotl + 0.25) + s.emissive;\n    return float4(c / (c + 1.0), 1.0);\n}\n";
        ensureAssetDirectories(assets);
        const std::string slangPath = assets.root + "/materials/" + std::to_string(id.value) + "_preview.slang";
        const std::string spvPath = assets.root + "/materials/" + std::to_string(id.value) + "_preview.spv";
        {
            std::ofstream out(slangPath);
            if (!out)
            {
                return Err(std::format("cannot write generated preview shader '{}'", slangPath));
            }
            out << shader;
        }
        const std::string cmd = "\"" + slangc + "\" \"" + slangPath +
                                "\" -profile glsl_450 -target spirv -emit-spirv-directly -fvk-use-entrypoint-name "
                                "-matrix-layout-column-major -o \"" +
                                spvPath + "\" > /dev/null 2>&1";
        const int rc = std::system(cmd.c_str());
        if (rc != 0 || !std::filesystem::exists(spvPath))
        {
            return Err(std::format("slangc failed (rc={}) compiling preview shader {}", rc, id.value));
        }
        return spvPath;
    }

    // Writes a MaterialAsset to assets/materials/<uuid>.smat and registers a Material catalog
    // entry (named, deduped). Returns the new id.
    auto saveMaterialAsset(AssetServer& assets, const MaterialAsset& mat, const std::string& name,
                           const std::string& folder = std::string{}) -> Result<Uuid>
    {
        const Uuid id = newUuid();
        ensureAssetDirectories(assets);
        const std::string relativePath = "materials/" + std::to_string(id.value) + ".smat";
        std::ofstream out(assets.root + "/" + relativePath);
        if (!out)
        {
            return Err(std::format("cannot write material '{}'", relativePath));
        }
        out << materialAssetToJson(mat).dump(2);
        if (!out)
        {
            return Err(std::format("write failed for material '{}'", relativePath));
        }
        putAsset(assets.catalog,
                 AssetEntry{ id, uniqueName(assets.catalog, name), AssetType::Material, relativePath, folder });
        return id;
    }

    // Applies a sparse override map { field: value } onto a base material (the instance path).
    void applyOverrides(MaterialAsset& m, const nlohmann::json& overrides)
    {
        if (!overrides.is_object())
        {
            return;
        }
        const auto uuidOf = [](const nlohmann::json& v) -> Uuid
        {
            if (v.is_string())
            {
                return Uuid{ std::strtoull(v.get<std::string>().c_str(), nullptr, 10) };
            }
            if (v.is_number_unsigned())
            {
                return Uuid{ v.get<u64>() };
            }
            return Uuid{ 0 };
        };
        for (const auto& [field, value] : overrides.items())
        {
            if (field == "baseColor" && value.is_array() && value.size() >= 4)
            {
                m.baseColor = glm::vec4{ value[0].get<f32>(), value[1].get<f32>(), value[2].get<f32>(),
                                         value[3].get<f32>() };
            }
            else if (field == "emissive" && value.is_array() && value.size() >= 3)
            {
                m.emissive = glm::vec3{ value[0].get<f32>(), value[1].get<f32>(), value[2].get<f32>() };
            }
            else if (field == "metallic" && value.is_number())
            {
                m.metallic = value.get<f32>();
            }
            else if (field == "roughness" && value.is_number())
            {
                m.roughness = value.get<f32>();
            }
            else if (field == "emissiveStrength" && value.is_number())
            {
                m.emissiveStrength = value.get<f32>();
            }
            else if (field == "normalStrength" && value.is_number())
            {
                m.normalStrength = value.get<f32>();
            }
            else if (field == "albedoTexture")
            {
                m.albedoTexture = uuidOf(value);
            }
            else if (field == "ormTexture")
            {
                m.ormTexture = uuidOf(value);
            }
            else if (field == "normalTexture")
            {
                m.normalTexture = uuidOf(value);
            }
            else if (field == "emissiveTexture")
            {
                m.emissiveTexture = uuidOf(value);
            }
            else if (field == "heightTexture")
            {
                m.heightTexture = uuidOf(value);
            }
        }
    }

    // Reads the stored .smat as-is — no parent resolution, no graph fold. The edit path (the editor
    // mutates this and writes it back via updateMaterialAsset).
    auto loadMaterialAssetRaw(AssetServer& assets, Uuid id) -> Result<MaterialAsset>
    {
        if (id.value == DefaultMaterialId.value)
        {
            return defaultMaterialAsset();
        }
        const AssetEntry* entry = findAsset(assets.catalog, id);
        if (entry == nullptr || entry->type != AssetType::Material)
        {
            return Err(std::format("material asset {} not found", id.value));
        }
        std::ifstream in(assets.root + "/" + entry->path);
        if (!in)
        {
            return Err(std::format("cannot read material '{}'", entry->path));
        }
        nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_discarded())
        {
            return Err(std::format("invalid material json '{}'", entry->path));
        }
        return materialAssetFromJson(j);
    }

    // Reads a .smat resolved for rendering: an instance is its parent's resolved params with this
    // material's overrides applied (edit-once-propagate); a master applies its foldable graph. Pure
    // data — textures are not resolved to GPU handles here.
    auto loadMaterialAsset(AssetServer& assets, Uuid id, u32 depth = 0) -> Result<MaterialAsset>
    {
        auto raw = loadMaterialAssetRaw(assets, id);
        if (!raw)
        {
            return raw;
        }
        MaterialAsset m = *raw;
        if (m.parent.value != 0 && depth < 8)
        {
            if (auto parentResolved = loadMaterialAsset(assets, m.parent, depth + 1))
            {
                MaterialAsset base = *parentResolved;
                applyOverrides(base, m.overrides);
                base.parent = m.parent;        // keep the lineage so the editor knows it's an instance
                base.overrides = m.overrides;  // keep the raw overrides for editing
                return base;
            }
            // Missing/cyclic parent: fall back to this material's own stored params.
        }
        if (m.graph.is_object() && !m.graph.empty())
        {
            MaterialAsset folded = m;
            if (lowerGraphToParams(m.graph, folded))
            {
                m = folded;
            }
        }
        return m;
    }

    // Overwrites an existing .smat in place (same id + path) — the edit path, vs saveMaterialAsset
    // which mints a new asset.
    auto updateMaterialAsset(AssetServer& assets, Uuid id, const MaterialAsset& mat) -> Result<void>
    {
        const AssetEntry* entry = findAsset(assets.catalog, id);
        if (entry == nullptr || entry->type != AssetType::Material)
        {
            return Err(std::format("material asset {} not found", id.value));
        }
        std::ofstream out(assets.root + "/" + entry->path);
        if (!out)
        {
            return Err(std::format("cannot write material '{}'", entry->path));
        }
        out << materialAssetToJson(mat).dump(2);
        if (!out)
        {
            return Err(std::format("write failed for material '{}'", entry->path));
        }
        return {};
    }

    // Detects a PBR map role from a texture filename's suffix tokens (case-insensitive). Returns one
    // of albedo|normal|orm|roughness|metallic|ao|height|emissive|gloss|opacity, or empty if
    // unrecognized. Most-specific tokens win (e.g. arm/orm before albedo).
    auto detectMaterialRole(const std::string& filename) -> std::string
    {
        std::string s;
        s.reserve(filename.size());
        for (char c : filename)
        {
            s.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c);
        }
        const auto has = [&](const char* token) { return s.find(token) != std::string::npos; };
        if (has("arm") || has("orm") || has("_mra"))
        {
            return "orm";
        }
        if (has("albedo") || has("basecolor") || has("base_color") || has("diffuse") || has("_diff") || has("_col") ||
            has("color"))
        {
            return "albedo";
        }
        if (has("normal") || has("_nor") || has("nrm"))
        {
            return "normal";
        }
        if (has("rough"))
        {
            return "roughness";
        }
        if (has("metal"))
        {
            return "metallic";
        }
        if (has("emissive") || has("emission") || has("_emit"))
        {
            return "emissive";
        }
        if (has("height") || has("displace") || has("_disp") || has("bump"))
        {
            return "height";
        }
        if (has("occlusion") || has("_ao") || has("ambientocclusion"))
        {
            return "ao";
        }
        if (has("gloss"))
        {
            return "gloss";
        }
        if (has("opacity") || has("alpha") || has("_mask"))
        {
            return "opacity";
        }
        return "";
    }

    struct MaterialImportResult
    {
        Uuid material;
        std::string roles;  // space-joined detected roles, for the editor's confirmation proposal
    };

    // Drag-a-folder material import: scans `dir` for textures, detects each map's role by filename
    // suffix, imports it with the right colorspace, assembles a .smat, and saves it. Normal maps are
    // assumed OpenGL convention and roughness assumed (not glossiness) — the bake helpers (phase 07)
    // plug in here when DX/gloss provenance is added. A packed ARM/ORM also feeds the occlusion slot.
    auto importMaterialFolder(AssetServer& assets, Renderer& renderer, const std::string& dir, const std::string& name)
        -> Result<MaterialImportResult>
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec))
        {
            return Err(std::format("not a directory: {}", dir));
        }
        const auto registerFile = [&](const std::filesystem::path& p, bool srgb) -> Uuid
        {
            std::ifstream in(p, std::ios::binary);
            const std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (bytes.empty())
            {
                return Uuid{ 0 };
            }
            std::string ext = p.extension().string();
            if (!ext.empty() && ext[0] == '.')
            {
                ext = ext.substr(1);
            }
            auto id = registerTextureBytes(assets, renderer, bytes, ext, p.stem().string(), srgb);
            return id ? *id : Uuid{ 0 };
        };
        MaterialAsset mat;
        std::string roles;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const std::filesystem::path& p = entry.path();
            std::string ext = p.extension().string();
            for (char& c : ext)
            {
                c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
            }
            if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".tga")
            {
                continue;
            }
            const std::string role = detectMaterialRole(p.filename().string());
            if (role == "albedo")
            {
                mat.albedoTexture = registerFile(p, true);
                roles += "albedo ";
            }
            else if (role == "normal")
            {
                mat.normalTexture = registerFile(p, false);
                roles += "normal ";
            }
            else if (role == "orm" || role == "roughness" || role == "metallic")
            {
                mat.ormTexture = registerFile(p, false);
                roles += role + " ";
            }
            else if (role == "ao")
            {
                if (mat.ormTexture.value == 0)
                {
                    mat.ormTexture = registerFile(p, false);
                }
                roles += "ao ";
            }
            else if (role == "height")
            {
                mat.heightTexture = registerFile(p, false);
                roles += "height ";
            }
            else if (role == "emissive")
            {
                mat.emissiveTexture = registerFile(p, true);
                roles += "emissive ";
            }
        }
        const std::string matName = name.empty() ? std::filesystem::path(dir).filename().string() : name;
        auto id = saveMaterialAsset(assets, mat, matName.empty() ? std::string{ "Material" } : matName);
        if (!id)
        {
            return Err(id.error());
        }
        return MaterialImportResult{ *id, roles };
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
            auto texture =
                uploadTexture(renderer, decoded->rgba.data(), decoded->width, decoded->height, !entry->linear);
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
        Result<void> baked = model->hasSkin
                                 ? saveMeshSkinned(model->mesh, model->skin, assets.root + "/" + relativePath)
                                 : saveMesh(model->mesh, assets.root + "/" + relativePath);
        if (!baked)
        {
            return Err(baked.error());
        }
        auto meshRef =
            model->hasSkin ? uploadMesh(renderer, model->mesh, model->skin) : uploadMesh(renderer, model->mesh);
        if (!meshRef)
        {
            return Err(meshRef.error());
        }
        putAsset(assets.catalog, AssetEntry{ meshId, uniqueName(assets.catalog, baseName), AssetType::Mesh,
                                             relativePath, std::string{} });
        assets.meshRefByUuid[meshId.value] = *meshRef;

        ImportResult result;
        result.mesh = meshId;
        if (model->hasSkin)
        {
            result.hasSkin = true;
            result.nodes = std::move(model->nodes);
            result.skinDesc = std::move(model->skinDesc);
            // Bake each clip to a sidecar .sanim (uuid-named, beside the .smesh) and
            // register an AssetType::Animation entry the player resolves by id.
            for (const AnimClip& clip : model->animations)
            {
                const Uuid clipId = newUuid();
                const std::string clipPath = "models/" + std::to_string(clipId.value) + ".sanim";
                if (Result<void> bakedClip = saveAnimation(clip, assets.root + "/" + clipPath); !bakedClip)
                {
                    logWarn(std::format("model '{}': clip '{}' bake failed: {}", path, clip.name, bakedClip.error()));
                    continue;
                }
                AssetEntry entry{ clipId, uniqueName(assets.catalog, clip.name.empty() ? baseName : clip.name),
                                  AssetType::Animation, clipPath, std::string{} };
                entry.duration = clip.duration;
                putAsset(assets.catalog, std::move(entry));
                result.animations.push_back(clipId);
            }
        }
        // Register each material's albedo and lower the factors into a MaterialSlot.
        result.materials.reserve(model->materials.size());
        for (std::size_t i = 0; i < model->materials.size(); i = i + 1)
        {
            const ImportedMaterial& src = model->materials[i];
            MaterialSlot slot;
            slot.baseColor = src.baseColor;
            slot.metallic = src.metallic;
            slot.roughness = src.roughness;
            slot.emissive = src.emissive;
            slot.emissiveStrength = src.emissiveStrength;
            if (src.hasAlbedo)
            {
                const std::string label = std::format("{} albedo {}", baseName, i);
                auto texture = registerTextureBytes(assets, renderer, src.albedoBytes, src.albedoExt, label);
                if (texture)
                {
                    slot.albedoTexture = *texture;
                }
                else
                {
                    logWarn(std::format("model '{}': albedo texture failed: {}", path, texture.error()));
                }
            }
            if (src.hasMetallicRoughness)
            {
                // Metallic-roughness maps are linear data, not sRGB color.
                const std::string label = std::format("{} metallic-roughness {}", baseName, i);
                auto texture = registerTextureBytes(assets, renderer, src.metallicRoughnessBytes,
                                                    src.metallicRoughnessExt, label, /*srgb=*/false);
                if (texture)
                {
                    slot.metallicRoughnessTexture = *texture;
                }
                else
                {
                    logWarn(std::format("model '{}': metallic-roughness texture failed: {}", path, texture.error()));
                }
            }
            if (src.hasNormal)
            {
                // Normal maps are linear data, not sRGB color.
                const std::string label = std::format("{} normal {}", baseName, i);
                auto texture =
                    registerTextureBytes(assets, renderer, src.normalBytes, src.normalExt, label, /*srgb=*/false);
                if (texture)
                {
                    slot.normalTexture = *texture;
                }
                else
                {
                    logWarn(std::format("model '{}': normal texture failed: {}", path, texture.error()));
                }
            }
            if (src.hasOcclusion)
            {
                const std::string label = std::format("{} occlusion {}", baseName, i);
                auto texture =
                    registerTextureBytes(assets, renderer, src.occlusionBytes, src.occlusionExt, label, /*srgb=*/false);
                if (texture)
                {
                    slot.occlusionTexture = *texture;
                }
                else
                {
                    logWarn(std::format("model '{}': occlusion texture failed: {}", path, texture.error()));
                }
            }
            if (src.hasEmissiveTex)
            {
                const std::string label = std::format("{} emissive {}", baseName, i);
                auto texture = registerTextureBytes(assets, renderer, src.emissiveTexBytes, src.emissiveTexExt, label);
                if (texture)
                {
                    slot.emissiveTexture = *texture;
                }
                else
                {
                    logWarn(std::format("model '{}': emissive texture failed: {}", path, texture.error()));
                }
            }
            result.materials.push_back(slot);
        }
        if (!result.materials.empty())
        {
            result.baseColor = result.materials.front().baseColor;
            result.albedoTexture = result.materials.front().albedoTexture;
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
            // A v2 .smesh carries a VertexSkin stream; uploading it arms the GPU
            // skinning path for this mesh (v1 files return an empty stream).
            std::vector<VertexSkin> skin;
            if (auto loadedSkin = loadMeshSkin(fullPath))
            {
                skin = std::move(*loadedSkin);
            }
            auto meshRef = uploadMesh(renderer, *mesh, skin);
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

    // Attaches an import's material(s) to an entity: a single MaterialComponent when the
    // model has zero or one material, or a MaterialSetComponent (the slot table) when it
    // has more than one. Submesh.materialSlot indexes the set at render time.
    void applyImportedMaterials(Scene& scene, Entity entity, const ImportResult& result)
    {
        if (result.materials.size() > 1)
        {
            addComponent<MaterialSetComponent>(scene, entity).slots = result.materials;
            return;
        }
        MaterialComponent& material = addComponent<MaterialComponent>(scene, entity);
        if (!result.materials.empty())
        {
            const MaterialSlot& slot = result.materials.front();
            material.baseColor = slot.baseColor;
            material.albedoTexture = slot.albedoTexture;
            material.metallicRoughnessTexture = slot.metallicRoughnessTexture;
            material.metallic = slot.metallic;
            material.roughness = slot.roughness;
            material.emissive = slot.emissive;
            material.emissiveStrength = slot.emissiveStrength;
            material.unlit = slot.unlit;
            material.normalTexture = slot.normalTexture;
            material.occlusionTexture = slot.occlusionTexture;
            material.emissiveTexture = slot.emissiveTexture;
            material.heightTexture = slot.heightTexture;
            material.normalStrength = slot.normalStrength;
            material.uvTiling = slot.uvTiling;
            material.uvOffset = slot.uvOffset;
            material.heightScale = slot.heightScale;
            material.alphaClip = slot.alphaClip;
            material.alphaCutoff = slot.alphaCutoff;
        }
    }

    // Creates an entity from an import: a mesh + a material (base color + albedo).
    // Instantiates a rigged import: one entity per glTF node (local TRS, parented by
    // uuid), BoneComponent tags on the joints, and a SkinnedMeshComponent on the mesh
    // node listing the joints in glTF joint order. Returns the skinned mesh entity.
    auto spawnSkinnedModel(Scene& scene, std::string name, const ImportResult& result) -> Entity
    {
        std::vector<Entity> nodeEntities;
        std::vector<Uuid> nodeUuids;
        nodeEntities.reserve(result.nodes.size());
        nodeUuids.reserve(result.nodes.size());
        for (const ImportedNode& node : result.nodes)
        {
            Entity entity = createEntity(scene, node.name);
            TransformComponent& transform = getComponent<TransformComponent>(scene, entity);
            transform.translation = node.translation;
            // The source rotation is a quaternion; extract through the engine's
            // Rz*Ry*Rx Euler convention (the stable path setParent also uses).
            glm::vec3 euler;
            glm::extractEulerAngleZYX(glm::mat4_cast(node.rotation), euler.z, euler.y, euler.x);
            transform.rotation = euler;
            transform.scale = node.scale;
            nodeEntities.push_back(entity);
            nodeUuids.push_back(getComponent<IdComponent>(scene, entity).id);
        }
        for (std::size_t i = 0; i < result.nodes.size(); i = i + 1)
        {
            const i32 parent = result.nodes[i].parent;
            if (parent >= 0 && static_cast<std::size_t>(parent) < nodeUuids.size())
            {
                getComponent<RelationshipComponent>(scene, nodeEntities[i]).parent =
                    nodeUuids[static_cast<std::size_t>(parent)];
            }
        }

        std::vector<Uuid> bones;
        bones.reserve(result.skinDesc.joints.size());
        for (const i32 joint : result.skinDesc.joints)
        {
            if (joint < 0 || static_cast<std::size_t>(joint) >= nodeEntities.size())
            {
                bones.push_back(Uuid{ 0 });
                continue;
            }
            Entity bone = nodeEntities[static_cast<std::size_t>(joint)];
            if (!hasComponent<BoneComponent>(scene, bone))
            {
                addComponent<BoneComponent>(scene, bone);
            }
            bones.push_back(nodeUuids[static_cast<std::size_t>(joint)]);
        }

        const i32 meshNode = result.skinDesc.meshNode;
        Entity meshEntity = meshNode >= 0 && static_cast<std::size_t>(meshNode) < nodeEntities.size()
                                ? nodeEntities[static_cast<std::size_t>(meshNode)]
                                : createEntity(scene, name);
        getComponent<NameComponent>(scene, meshEntity).name = std::move(name);
        SkinnedMeshComponent& skin = addComponent<SkinnedMeshComponent>(scene, meshEntity);
        skin.mesh = result.mesh;
        const i32 root = result.skinDesc.skeletonRoot;
        skin.rootBone = root >= 0 && static_cast<std::size_t>(root) < nodeUuids.size()
                            ? nodeUuids[static_cast<std::size_t>(root)]
                            : (bones.empty() ? Uuid{ 0 } : bones.front());
        skin.bones = std::move(bones);
        skin.inverseBind = result.skinDesc.inverseBind;
        applyImportedMaterials(scene, meshEntity, result);

        // A rig that ships clips is immediately playable: attach a player defaulting to
        // the first clip, stopped and looping (Edit preview stays off until requested).
        if (!result.animations.empty())
        {
            AnimationPlayerComponent& player = addComponent<AnimationPlayerComponent>(scene, meshEntity);
            player.clip = result.animations.front();
            player.playing = false;
            player.wrap = AnimationPlayerComponent::Wrap::Loop;
        }

        relinkHierarchy(scene);  // resolve the parent uuids + the joint handles
        return meshEntity;
    }

    auto spawnModel(Scene& scene, std::string name, const ImportResult& result) -> Entity
    {
        if (result.hasSkin)
        {
            return spawnSkinnedModel(scene, std::move(name), result);
        }
        Entity entity = createEntity(scene, std::move(name));
        addComponent<MeshComponent>(scene, entity).mesh = result.mesh;
        applyImportedMaterials(scene, entity, result);
        return entity;
    }

    // The per-submesh materials for one renderable, plus the entity-level unlit flag
    // (selects the PSO) and a proxy albedo for the DDGI voxel box. Reads a
    // MaterialSetComponent (indexed by each submesh's materialSlot) when present, else a
    // single MaterialComponent applied to every submesh, else engine defaults.
    struct ResolvedMaterials
    {
        std::vector<SubmeshMaterial> submeshes;
        bool unlit = false;
        glm::vec3 proxyAlbedo{ 1.0f };
    };

    // Resolves a loaded MaterialAsset (.smat) to a render-ready SubmeshMaterial, loading its
    // texture handles to bindless GPU textures. A packed ORM also feeds the occlusion slot (AO in
    // R), so one ARM/ORM map drives roughness (G), metalness (B), and AO (R).
    auto resolveMaterialAsset(AssetServer& assets, Renderer& renderer, const MaterialAsset& mat) -> SubmeshMaterial
    {
        SubmeshMaterial sm;
        sm.baseColor = mat.baseColor;
        sm.metallic = mat.metallic;
        sm.roughness = mat.roughness;
        sm.emissive = mat.emissive;
        sm.emissiveStrength = mat.emissiveStrength;
        sm.normalStrength = mat.normalStrength;
        sm.uvTiling = mat.uvTiling;
        sm.uvOffset = mat.uvOffset;
        sm.heightScale = mat.heightScale;
        sm.alphaClip = (mat.blend == "masked");
        sm.alphaCutoff = mat.alphaCutoff;
        if (mat.albedoTexture.value != 0)
        {
            sm.albedoTexture = loadTextureAsset(assets, renderer, mat.albedoTexture);
        }
        if (mat.ormTexture.value != 0)
        {
            sm.metallicRoughnessTexture = loadTextureAsset(assets, renderer, mat.ormTexture);
            sm.occlusionTexture = loadTextureAsset(assets, renderer, mat.ormTexture);
        }
        if (mat.normalTexture.value != 0)
        {
            sm.normalTexture = loadTextureAsset(assets, renderer, mat.normalTexture);
        }
        if (mat.emissiveTexture.value != 0)
        {
            sm.emissiveTexture = loadTextureAsset(assets, renderer, mat.emissiveTexture);
        }
        if (mat.heightTexture.value != 0)
        {
            sm.heightTexture = loadTextureAsset(assets, renderer, mat.heightTexture);
        }
        return sm;
    }

    auto resolveEntityMaterials(Scene& scene, AssetServer& assets, Renderer& renderer, Entity entity,
                                const Ref<GpuMesh>& meshRef) -> ResolvedMaterials
    {
        ResolvedMaterials out;
        const auto lower = [&](const MaterialSlot& slot) -> SubmeshMaterial
        {
            SubmeshMaterial sm;
            sm.baseColor = slot.baseColor;
            sm.metallic = slot.metallic;
            sm.roughness = slot.roughness;
            sm.emissive = slot.emissive;
            sm.emissiveStrength = slot.emissiveStrength;
            if (slot.albedoTexture.value != 0)
            {
                sm.albedoTexture = loadTextureAsset(assets, renderer, slot.albedoTexture);
            }
            if (slot.metallicRoughnessTexture.value != 0)
            {
                sm.metallicRoughnessTexture = loadTextureAsset(assets, renderer, slot.metallicRoughnessTexture);
            }
            sm.normalStrength = slot.normalStrength;
            sm.uvTiling = slot.uvTiling;
            sm.uvOffset = slot.uvOffset;
            if (slot.normalTexture.value != 0)
            {
                sm.normalTexture = loadTextureAsset(assets, renderer, slot.normalTexture);
            }
            if (slot.occlusionTexture.value != 0)
            {
                sm.occlusionTexture = loadTextureAsset(assets, renderer, slot.occlusionTexture);
            }
            if (slot.emissiveTexture.value != 0)
            {
                sm.emissiveTexture = loadTextureAsset(assets, renderer, slot.emissiveTexture);
            }
            sm.heightScale = slot.heightScale;
            sm.alphaClip = slot.alphaClip;
            sm.alphaCutoff = slot.alphaCutoff;
            if (slot.heightTexture.value != 0)
            {
                sm.heightTexture = loadTextureAsset(assets, renderer, slot.heightTexture);
            }
            return sm;
        };
        if (hasComponent<MaterialAssetComponent>(scene, entity))
        {
            const Uuid matId = getComponent<MaterialAssetComponent>(scene, entity).material;
            if (matId.value != 0)
            {
                auto loaded = loadMaterialAsset(assets, matId);
                if (!loaded)
                {
                    logWarn(std::format("entity material asset {} missing; using default", matId.value));
                }
                const MaterialAsset mat = loaded ? *loaded : defaultMaterialAsset();
                out.unlit = mat.unlit;
                out.proxyAlbedo = glm::vec3(mat.baseColor);
                const SubmeshMaterial sm = resolveMaterialAsset(assets, renderer, mat);
                out.submeshes.assign(std::max<std::size_t>(meshRef->submeshes.size(), std::size_t{ 1 }), sm);
                return out;
            }
        }
        if (hasComponent<MaterialSetComponent>(scene, entity))
        {
            const std::vector<MaterialSlot>& slots = getComponent<MaterialSetComponent>(scene, entity).slots;
            if (!slots.empty())
            {
                out.unlit = slots.front().unlit;
                out.proxyAlbedo = glm::vec3(slots.front().baseColor);
                out.submeshes.reserve(meshRef->submeshes.size());
                for (const Submesh& submesh : meshRef->submeshes)
                {
                    const std::size_t slot = std::min<std::size_t>(submesh.materialSlot, slots.size() - 1);
                    out.submeshes.push_back(lower(slots[slot]));
                }
                return out;
            }
        }
        if (hasComponent<MaterialComponent>(scene, entity))
        {
            const MaterialComponent& material = getComponent<MaterialComponent>(scene, entity);
            out.unlit = material.unlit;
            out.proxyAlbedo = glm::vec3(material.baseColor);
            MaterialSlot slot;
            slot.baseColor = material.baseColor;
            slot.albedoTexture = material.albedoTexture;
            slot.metallicRoughnessTexture = material.metallicRoughnessTexture;
            slot.metallic = material.metallic;
            slot.roughness = material.roughness;
            slot.emissive = material.emissive;
            slot.emissiveStrength = material.emissiveStrength;
            slot.normalTexture = material.normalTexture;
            slot.occlusionTexture = material.occlusionTexture;
            slot.emissiveTexture = material.emissiveTexture;
            slot.normalStrength = material.normalStrength;
            slot.uvTiling = material.uvTiling;
            slot.uvOffset = material.uvOffset;
            slot.heightTexture = material.heightTexture;
            slot.heightScale = material.heightScale;
            slot.alphaClip = material.alphaClip;
            slot.alphaCutoff = material.alphaCutoff;
            out.submeshes.push_back(lower(slot));
        }
        return out;
    }

    auto loadEditorCameraModel(AssetServer& assets, Renderer& renderer) -> SystemMeshVisual*
    {
        SystemMeshVisual& visual = assets.editorCameraModel;
        if (visual.attempted)
        {
            return visual.mesh ? &visual : nullptr;
        }
        visual.attempted = true;
        auto model = importModelWithMaterial(assetPath("models/editor-camera.glb"));
        if (!model)
        {
            logWarn(std::format("editor camera model: {}", model.error()));
            return nullptr;
        }
        auto meshRef =
            model->hasSkin ? uploadMesh(renderer, model->mesh, model->skin) : uploadMesh(renderer, model->mesh);
        if (!meshRef)
        {
            logWarn(std::format("editor camera model: {}", meshRef.error()));
            return nullptr;
        }
        visual.mesh = *meshRef;
        SubmeshMaterial material;
        material.baseColor = glm::vec4{ 0.02f, 0.018f, 0.016f, 1.0f };
        material.roughness = 0.78f;
        material.emissive = glm::vec3{ 0.012f };
        visual.submeshMaterials.assign(model->mesh.submeshes.size(), material);
        if (visual.submeshMaterials.empty())
        {
            visual.submeshMaterials.push_back(material);
        }
        return &visual;
    }

    void appendEditorCameraModels(Scene& scene, AssetServer& assets, Renderer& renderer, std::vector<DrawItem>& items)
    {
        SystemMeshVisual* visual = loadEditorCameraModel(assets, renderer);
        if (visual == nullptr || !visual->mesh)
        {
            return;
        }
        forEach<TransformComponent, CameraComponent>(
            scene,
            [&](Entity entity, TransformComponent&, CameraComponent& camera)
            {
                if (!camera.showModel)
                {
                    return;
                }
                constexpr f32 ModelScale = 7.5f;
                constexpr f32 LensLocalX = 0.0801217f;
                const glm::mat4 model =
                    worldMatrix(scene, entity) * glm::translate(glm::mat4(1.0f), glm::vec3{ 0.0f, -0.1f, 0.0f }) *
                    glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3{ 0.0f, 1.0f, 0.0f }) *
                    glm::scale(glm::mat4(1.0f), glm::vec3{ ModelScale }) *
                    glm::translate(glm::mat4(1.0f), glm::vec3{ -LensLocalX, 0.0f, 0.0f });
                DrawItem item;
                item.mesh = visual->mesh;
                item.model = model;
                item.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(model))));
                item.submeshMaterials = visual->submeshMaterials;
                items.push_back(std::move(item));
            });
    }

    // Draws every entity with a Transform + Mesh through the given camera (the editor
    // viewport camera), resolving each mesh on demand. A no-op without a viewport.
    void renderScene(Renderer& renderer, Scene& scene, AssetServer& assets, const CameraView& camera,
                     const RenderSceneOptions& options = {})
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

        // Flatten the hierarchy once per frame before any consumer reads: every loop
        // below (lights, meshes, probes) and the between-frame pick/gizmo paths read
        // the WorldTransformComponent cache this writes.
        updateWorldTransforms(scene);

        glm::vec3 lightDir{ -0.5f, -1.0f, -0.3f };
        glm::vec3 lightColor{ 1.0f };
        f32 lightIntensity = 1.0f;
        f32 lightAmbient = 0.15f;
        bool haveLight = false;
        forEach<DirectionalLightComponent>(scene,
                                           [&](Entity entity, DirectionalLightComponent& light)
                                           {
                                               if (haveLight)
                                               {
                                                   return;
                                               }
                                               // A parented light re-aims with its parent; a
                                               // transformless one keeps its raw direction.
                                               lightDir = hasComponent<TransformComponent>(scene, entity)
                                                              ? worldRotation(scene, entity) * light.direction
                                                              : light.direction;
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
            [&](Entity entity, TransformComponent&, PointLightComponent& light)
            {
                const glm::vec3 pos = worldTranslation(scene, entity);
                GpuLight gpu;
                gpu.positionRange = glm::vec4(pos, light.range);
                gpu.colorIntensity = glm::vec4(light.color, light.intensity);
                gpu.directionType = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);  // type 0 = point
                gpu.spotCos = glm::vec4(0.0f);
                if (!havePointShadow)
                {
                    pointShadowPos = pos;
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
            [&](Entity entity, TransformComponent&, SpotLightComponent& light)
            {
                // The component direction re-aims with the entity's world rotation, so a
                // parented spot follows its parent's orientation.
                const glm::vec3 pos = worldTranslation(scene, entity);
                const glm::vec3 dir = glm::normalize(worldRotation(scene, entity) * light.direction);
                GpuLight gpu;
                gpu.positionRange = glm::vec4(pos, light.range);
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
                    const glm::mat4 lightView = glm::lookAt(pos, pos + dir, up);
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
            [&](Entity entity, TransformComponent&, MeshComponent& mesh)
            {
                auto meshRef = loadMeshAsset(assets, renderer, mesh.mesh);
                if (!meshRef)
                {
                    return;
                }
                ResolvedMaterials materials = resolveEntityMaterials(scene, assets, renderer, entity, meshRef);
                const glm::mat4 model = worldMatrix(scene, entity);
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
                boxAlbedos.push_back(glm::vec4(materials.proxyAlbedo, 0.0f));
                DrawItem item;
                item.mesh = meshRef;
                item.model = model;
                item.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(model))));
                item.submeshMaterials = std::move(materials.submeshes);
                item.material.unlit = materials.unlit;
                item.entity =
                    hasComponent<IdComponent>(scene, entity) ? getComponent<IdComponent>(scene, entity).id.value : 0;
                items.push_back(std::move(item));
            });

        // Skinned renderables: joints place the vertices entirely (the node transform is
        // ignored per glTF, so model stays identity), blending the frame joint palette
        // built here. Gated by the skinning toggle; off leaves the frame byte-identical
        // to a build without the skinned path.
        std::vector<glm::mat4> frameJoints;
        if (skinningEnabled(renderer))
        {
            forEach<TransformComponent, SkinnedMeshComponent>(
                scene,
                [&](Entity entity, TransformComponent&, SkinnedMeshComponent& skin)
                {
                    auto meshRef = loadMeshAsset(assets, renderer, skin.mesh);
                    if (!meshRef || !meshRef->skinBuffer)
                    {
                        return;  // missing asset, or one baked without a skin stream
                    }
                    std::vector<glm::mat4> palette;
                    jointMatrices(scene, skin, palette);
                    if (palette.empty())
                    {
                        return;
                    }
                    ResolvedMaterials materials = resolveEntityMaterials(scene, assets, renderer, entity, meshRef);
                    // Conservative bounds: union the bind-space AABB corners through every
                    // joint matrix, feeding the scene AABB (shadow/DDGI fit) like any draw.
                    for (const glm::mat4& joint : palette)
                    {
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
                            const glm::vec3 world = glm::vec3(joint * glm::vec4(p, 1.0f));
                            sceneMin = glm::min(sceneMin, world);
                            sceneMax = glm::max(sceneMax, world);
                        }
                    }
                    DrawItem item;
                    item.mesh = meshRef;
                    item.skinned = true;
                    item.jointOffset = static_cast<u32>(frameJoints.size());
                    item.jointCount = static_cast<u32>(palette.size());
                    item.submeshMaterials = std::move(materials.submeshes);
                    item.material.unlit = materials.unlit;
                    // model stays identity: the joint matrices (worldBone * inverseBind) already
                    // place the vertices in world space, so entity movement rides inside the
                    // palette. Deformation motion (prev palette vs current) thus covers both bone
                    // animation AND the whole rig moving — no separate object-motion term needed.
                    item.entity = hasComponent<IdComponent>(scene, entity)
                                      ? getComponent<IdComponent>(scene, entity).id.value
                                      : 0;
                    frameJoints.insert(frameJoints.end(), palette.begin(), palette.end());
                    items.push_back(std::move(item));
                });
        }

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
        // RT: hand the frame's STATIC instance transforms + meshes to the renderer for the
        // per-frame TLAS build (used by ray-query shadows when enabled + supported). Skinned
        // instances ride the draw list instead: submitDrawList records each one's deformed
        // offset for a per-frame refit BLAS the TLAS references with an identity transform (the
        // deformed vertices are already in world space). The static BLAS is object-space, so it
        // carries item.model; a skinned mesh's static BLAS would freeze the bind pose, hence the
        // split here.
        {
            std::vector<glm::mat4> rtModels;
            std::vector<Ref<GpuMesh>> rtMeshes;
            rtModels.reserve(items.size());
            rtMeshes.reserve(items.size());
            for (const DrawItem& it : items)
            {
                if (it.skinned)
                {
                    continue;  // skinned RT instances come from the draw list's deformed-offset entries
                }
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
        // Reflection probes: snapshot each ReflectionProbeComponent (positioned by its Transform)
        // into the renderer before the lighting upload, so setSceneLighting writes the right probe
        // count and submitReflectionProbes can arm a capture for any dirty/moved slot.
        std::vector<ReflectionProbeUpload> probeUploads;
        forEach<TransformComponent, ReflectionProbeComponent>(
            scene,
            [&](Entity entity, TransformComponent&, ReflectionProbeComponent& probe)
            {
                if (probeUploads.size() >= MaxReflectionProbes)
                {
                    return;
                }
                ReflectionProbeUpload up;
                up.entity =
                    hasComponent<IdComponent>(scene, entity) ? getComponent<IdComponent>(scene, entity).id.value : 0;
                up.origin = worldTranslation(scene, entity);
                up.influenceRadius = probe.influenceRadius;
                up.intensity = probe.intensity;
                up.boxProjection = probe.boxProjection;
                up.boxExtent = probe.boxExtent;
                up.dirty = probe.dirty;
                probe.dirty = false;  // consumed; the renderer tracks capture state from here
                probeUploads.push_back(up);
            });
        submitReflectionProbes(renderer, probeUploads);

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
            // Mirror the scene atmosphere onto the renderer-side params so the bake carries it.
            const AtmosphereSettings& at = env.atmosphere;
            skyBake.atmosphere.enabled = at.enabled;
            skyBake.atmosphere.planetRadius = at.planetRadius;
            skyBake.atmosphere.atmosphereHeight = at.atmosphereHeight;
            skyBake.atmosphere.rayleighScattering = at.rayleighScattering;
            skyBake.atmosphere.rayleighScaleHeight = at.rayleighScaleHeight;
            skyBake.atmosphere.mieScattering = at.mieScattering;
            skyBake.atmosphere.mieScaleHeight = at.mieScaleHeight;
            skyBake.atmosphere.mieAnisotropy = at.mieAnisotropy;
            skyBake.atmosphere.ozoneAbsorption = at.ozoneAbsorption;
            skyBake.atmosphere.sunDiskAngularRadius = at.sunDiskAngularRadius;
            skyBake.atmosphere.sunDiskIntensity = at.sunDiskIntensity;
            // Resolution order: a user equirect panorama wins, then the atmosphere, then the
            // gradient. Only a valid loaded panorama claims Equirect.
            const bool wantEquirect = env.skyMode == SkyMode::Texture && env.skyTexture.value != 0;
            if (wantEquirect)
            {
                skyPanorama = loadTextureAsset(assets, renderer, env.skyTexture);
            }
            if (skyPanorama)
            {
                requestEnvBake(renderer, EnvSource::Equirect, skyPanorama, skyBake);
            }
            else if (at.enabled)
            {
                requestEnvBake(renderer, EnvSource::Atmosphere, nullptr, skyBake);
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

        if (options.showEditorCameraModels)
        {
            appendEditorCameraModels(scene, assets, renderer, items);
        }
        submitDrawList(renderer, viewProjection, items, frameJoints);

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
        forEach<TransformComponent, MeshComponent>(scene,
                                                   [&](Entity entity, TransformComponent&, MeshComponent& mesh)
                                                   {
                                                       auto meshRef = loadMeshAsset(assets, renderer, mesh.mesh);
                                                       if (!meshRef)
                                                       {
                                                           return;
                                                       }
                                                       // World AABB from the 8 transformed local-AABB corners; the
                                                       // world matrix comes from the last frame's flatten (lockstep
                                                       // with the draw loop).
                                                       const glm::mat4 model = worldMatrix(scene, entity);
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
                                                           const glm::vec3 world =
                                                               glm::vec3(model * glm::vec4(p, 1.0f));
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
