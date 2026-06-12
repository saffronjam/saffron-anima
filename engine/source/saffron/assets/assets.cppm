module;

// Bridges Scene + Geometry + Rendering, so (like those) it uses classic includes.
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <deque>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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

    struct ThumbnailWorker;  // async thumbnail generation; defined at the foot of this module
    struct ModelAsset;       // an opened .smodel (metadata + chunk reader); defined after ContainerMetadata

    // Owns the project's asset catalog (id -> {name, type, path}) plus uuid-keyed GPU
    // caches so entities sharing an id upload once. A cached null Ref is the
    // negative-cache marker — a failed asset is not retried each frame.
    struct AssetServer
    {
        std::string root;
        AssetCatalog catalog;                                       // source of truth: id -> {name,type,path}
        std::unordered_map<u64, Ref<GpuMesh>> meshRefByUuid;        // GPU cache (keyed by mesh / sub-id)
        std::unordered_map<u64, Ref<GpuTexture>> textureRefByUuid;  // GPU cache (keyed by texture / sub-id)
        std::unordered_map<u64, Ref<ModelAsset>> modelRefByUuid;    // opened .smodel containers, by model id
        SystemMeshVisual editorCameraModel;
        // Off-thread thumbnail generation (null until startThumbnailWorker). The worker uploads +
        // renders + caches thumbnails so a cold cache-miss never blocks the frame loop; finished GPU
        // resources are handed back here and inserted into the caches above by the main thread.
        Ref<ThumbnailWorker> thumbnailWorker;
    };

    // Drops the worker's queued/failed jobs + un-drained handbacks (defined with the worker, below).
    // Called from clearAssetCaches on a project switch so stale work doesn't leak into the new project.
    void clearThumbnailQueue(AssetServer& assets);

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
        Uuid ormTexture;  // packed AO/roughness/metallic (or a standalone metallic-roughness)
        Uuid normalTexture;
        Uuid emissiveTexture;
        Uuid heightTexture;
        std::string normalConvention = "gl";  // gl | dx — baked to gl at import; recorded for provenance
        u32 features = 0;                     // resolved feature bitset (populated from the PBR-slots phase)
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

    // The spawn input spawnModel/spawnSkinnedModel consume: the mesh + its material table and — for a
    // rigged glTF — the node forest + skin descriptor instantiated as bone entities. Reconstructed by
    // instantiateModel from a .smodel container's metadata; never an import output.
    struct ModelSpawnInput
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

    /// Every decision an import makes, in one serializable place (the UE Interchange "decide" half).
    /// Stored verbatim in a container's MetadataChunk so a reimport replays the same options rather
    /// than today's defaults. For v1 scale/axis/genTangents are recorded intent; embedTextures is
    /// always true. `colorspaceFor` is the single source of truth for per-role texture colorspace.
    struct ImportOptions
    {
        enum class Axis : u8
        {
            YUp,
            ZUp
        };
        f32 scale = 1.0f;
        Axis axis = Axis::YUp;
        bool genTangents = true;
        bool embedTextures = true;

        auto colorspaceFor(MaterialMapRole role) const -> Colorspace
        {
            if (role == MaterialMapRole::Albedo || role == MaterialMapRole::Emissive)
            {
                return Colorspace::Srgb;
            }
            return Colorspace::Linear;  // normal / metallic-roughness / occlusion / height are data maps
        }

        auto toJson() const -> nlohmann::json
        {
            const char* axisName = "y-up";
            if (axis == Axis::ZUp)
            {
                axisName = "z-up";
            }
            return nlohmann::json{ { "scale", scale },
                                   { "axis", axisName },
                                   { "genTangents", genTangents },
                                   { "embedTextures", embedTextures } };
        }

        static auto fromJson(const nlohmann::json& j) -> ImportOptions
        {
            ImportOptions options;
            options.scale = jsonF32Or(j, "scale", 1.0f);
            if (jsonStringOr(j, "axis", std::string{ "y-up" }) == "z-up")
            {
                options.axis = Axis::ZUp;
            }
            options.genTangents = jsonBoolOr(j, "genTangents", true);
            options.embedTextures = jsonBoolOr(j, "embedTextures", true);
            return options;
        }
    };

    /// The bump-on-incompatible-translator version stamped into a container's import recipe; a reimport
    /// whose stored value differs is re-baked rather than skipped (phase 13).
    inline constexpr u32 ImporterVersion = 1;

    /// What bakeModel produces: the new container's id, its on-disk path, and the catalog rows it
    /// contributes (one AssetType::Model parent + one row per embedded sub-asset). No GPU, no spawn.
    struct BakeResult
    {
        Uuid modelId;
        std::string path;  // project-relative path to the .smodel
        std::vector<AssetEntry> rows;
    };

    /// What a scan changed relative to the live catalog: rows added (newly discovered on disk) and ids
    /// removed (their backing file is gone). The filesystem is the source of truth, so an unsaved import
    /// can never become a dead orphan — its `.smodel` is rediscovered on the next scan.
    struct ScanDelta
    {
        std::vector<AssetEntry> added;
        std::vector<Uuid> removed;
    };

    auto scanAssets(AssetServer& assets) -> Result<ScanDelta>;
    auto loadCatalog(AssetServer& assets) -> Result<ScanDelta>;
    void writeCatalogCache(const AssetServer& assets);

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
        if (type == AssetType::Model)
        {
            return "model";
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
        if (name == "model")
        {
            return AssetType::Model;
        }
        if (name == "mesh")
        {
            return AssetType::Mesh;
        }
        // An unknown type-string is a forward-compat entry from a newer build; keep it
        // around as Other rather than mis-treating it as a renderable mesh.
        return AssetType::Other;
    }

    auto colorspaceName(Colorspace space) -> const char*
    {
        if (space == Colorspace::Srgb)
        {
            return "srgb";
        }
        if (space == Colorspace::Linear)
        {
            return "linear";
        }
        if (space == Colorspace::Hdr)
        {
            return "hdr";
        }
        return "auto";
    }

    auto colorspaceFromName(const std::string& name) -> Colorspace
    {
        if (name == "srgb")
        {
            return Colorspace::Srgb;
        }
        if (name == "linear")
        {
            return Colorspace::Linear;
        }
        if (name == "hdr")
        {
            return Colorspace::Hdr;
        }
        return Colorspace::Auto;
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
            // Container linkage + colorspace: omitted when default so standalone rows stay
            // byte-identical to before this field set existed.
            if (entry.container.value != 0)
            {
                record["container"] = uuidToJson(entry.container.value);
            }
            if (entry.chunk >= 0)
            {
                record["chunk"] = entry.chunk;
            }
            if (entry.colorspace != Colorspace::Auto)
            {
                record["colorspace"] = colorspaceName(entry.colorspace);
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
            parsed.container = Uuid{ jsonU64Or(entry, "container", 0) };
            if (auto it = entry.find("chunk"); it != entry.end() && it->is_number_integer())
            {
                parsed.chunk = it->get<i32>();
            }
            parsed.colorspace = colorspaceFromName(jsonStringOr(entry, "colorspace", std::string{ "auto" }));
            if (parsed.id.value != 0)
            {
                putAsset(catalog, std::move(parsed));
            }
        }
    }

    /// Headless check: a catalog with a model parent + embedded sub-assets + a standalone asset
    /// round-trips through json with container linkage, chunk index, and colorspace intact.
    void runCatalogLinkageSelfTest()
    {
        AssetCatalog catalog;
        putAsset(
            catalog,
            AssetEntry{ .id = Uuid{ 100 }, .name = "town", .type = AssetType::Model, .path = "models/100.smodel" });
        putAsset(catalog, AssetEntry{ .id = Uuid{ 101 },
                                      .name = "town_mesh",
                                      .type = AssetType::Mesh,
                                      .path = "models/100.smodel",
                                      .container = Uuid{ 100 },
                                      .chunk = 1 });
        putAsset(catalog, AssetEntry{ .id = Uuid{ 102 },
                                      .name = "town_albedo",
                                      .type = AssetType::Texture,
                                      .path = "models/100.smodel",
                                      .container = Uuid{ 100 },
                                      .chunk = 2,
                                      .colorspace = Colorspace::Srgb });
        putAsset(catalog,
                 AssetEntry{
                     .id = Uuid{ 200 }, .name = "loose", .type = AssetType::Material, .path = "materials/200.smat" });

        AssetCatalog restored;
        catalogFromJson(restored, catalogToJson(catalog));

        const AssetEntry* model = findAsset(restored, Uuid{ 100 });
        const AssetEntry* mesh = findAsset(restored, Uuid{ 101 });
        const AssetEntry* tex = findAsset(restored, Uuid{ 102 });
        const AssetEntry* loose = findAsset(restored, Uuid{ 200 });
        const bool ok = restored.entries.size() == 4 && model != nullptr && mesh != nullptr && tex != nullptr &&
                        loose != nullptr && model->type == AssetType::Model && model->container.value == 0 &&
                        model->chunk == -1 && mesh->container.value == 100 && mesh->chunk == 1 &&
                        tex->container.value == 100 && tex->chunk == 2 && tex->colorspace == Colorspace::Srgb &&
                        loose->container.value == 0 && loose->chunk == -1 && loose->colorspace == Colorspace::Auto;
        if (ok)
        {
            logInfo("catalog model + sub-asset linkage round-trip OK");
        }
        else
        {
            logError("catalog model + sub-asset linkage round-trip MISMATCH");
        }
    }

    /// The parsed MetadataChunk of a `.smodel` — the header-first record a catalog scan and a
    /// deterministic reimport need without touching payloads. On disk, uuids are decimal strings;
    /// nodes/skin/materials/remap stay as json (parsed on demand by instantiate/reimport).
    struct ContainerMetadata
    {
        struct Import
        {
            std::string sourcePath;  // project-relative source file
            std::string sourceHash;  // content hash of the source bytes (NOT mtime)
            u32 importerVersion = 1;
            nlohmann::json options;  // ImportOptions::toJson(), stored verbatim
        };
        struct SubAsset
        {
            Uuid subId;
            AssetType type = AssetType::Mesh;
            std::string name;
            u32 chunk = 0;           // TOC chunk index inside the container
            std::string colorspace;  // texture: srgb|linear|hdr|auto (empty otherwise)
            f32 duration = 0.0f;     // animation: clip length in seconds
        };
        u32 schema = MetadataSchemaVersion;
        Uuid modelId;
        std::string name;
        std::string sourceFormat;  // "gltf" | "obj"
        Import import;
        std::vector<SubAsset> subAssets;
        nlohmann::json materials = nlohmann::json::array();  // [{subId, baseColor, metallic, roughness}]
        nlohmann::json nodes = nlohmann::json::array();      // glTF nodes-block shape; index-referenced
        nlohmann::json skin;                                 // null when unskinned
        nlohmann::json remap = nlohmann::json::object();     // {subId: {external: relPath}} (empty at bake)
    };

    /// Build the META-chunk bytes from a populated ContainerMetadata. Object keys serialize in a
    /// stable (sorted) order, so the bytes are deterministic for source hashing and the contract test.
    auto encodeContainerMetadata(const ContainerMetadata& meta) -> std::vector<std::byte>
    {
        nlohmann::json doc;
        doc["schema"] = meta.schema;
        doc["model"] = { { "id", uuidToJson(meta.modelId.value) },
                         { "name", meta.name },
                         { "sourceFormat", meta.sourceFormat } };
        doc["import"] = { { "sourcePath", meta.import.sourcePath },
                          { "sourceHash", meta.import.sourceHash },
                          { "importerVersion", meta.import.importerVersion },
                          { "options",
                            meta.import.options.is_null() ? nlohmann::json::object() : meta.import.options } };
        nlohmann::json subs = nlohmann::json::array();
        for (const ContainerMetadata::SubAsset& sub : meta.subAssets)
        {
            nlohmann::json record{ { "subId", uuidToJson(sub.subId.value) },
                                   { "type", assetTypeName(sub.type) },
                                   { "name", sub.name },
                                   { "chunk", sub.chunk } };
            if (!sub.colorspace.empty())
            {
                record["colorspace"] = sub.colorspace;
            }
            if (sub.type == AssetType::Animation)
            {
                record["duration"] = sub.duration;
            }
            subs.push_back(std::move(record));
        }
        doc["subAssets"] = std::move(subs);
        doc["materials"] = meta.materials.is_null() ? nlohmann::json::array() : meta.materials;
        doc["nodes"] = meta.nodes.is_null() ? nlohmann::json::array() : meta.nodes;
        if (!meta.skin.is_null())
        {
            doc["skin"] = meta.skin;
        }
        doc["remap"] = meta.remap.is_null() ? nlohmann::json::object() : meta.remap;

        const std::string text = dumpJson(doc);
        std::vector<std::byte> bytes(text.size());
        std::memcpy(bytes.data(), text.data(), text.size());
        return bytes;
    }

    /// Prefix read: parse only the 64-byte header + the META chunk; touches no payload bytes. Forward
    /// compatible (ignores unknown keys) so a v1 reader survives a later schema growing new fields.
    auto readContainerMetadata(const std::string& path) -> Result<ContainerMetadata>
    {
        auto header = readContainerHeader(path);
        if (!header)
        {
            return Err(header.error());
        }
        if (header->metaLength == 0)
        {
            return Err(std::format("'{}' has no metadata chunk", path));
        }
        if (header->metaOffset < sizeof(SModelHeader) || header->metaOffset + header->metaLength > header->totalLength)
        {
            return Err(std::format("'{}' metadata chunk is out of bounds", path));
        }

        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return Err(std::format("cannot open '{}'", path));
        }
        std::string text(static_cast<std::size_t>(header->metaLength), '\0');
        in.seekg(static_cast<std::streamoff>(header->metaOffset));
        in.read(text.data(), static_cast<std::streamsize>(header->metaLength));
        if (!in)
        {
            return Err(std::format("read failed for the metadata of '{}'", path));
        }
        auto doc = parseJson(text);
        if (!doc || !doc->is_object())
        {
            return Err(std::format("'{}' has an invalid metadata chunk", path));
        }

        ContainerMetadata meta;
        meta.schema = static_cast<u32>(jsonU64Or(*doc, "schema", MetadataSchemaVersion));
        if (auto it = doc->find("model"); it != doc->end() && it->is_object())
        {
            meta.modelId = Uuid{ jsonU64Or(*it, "id", 0) };
            meta.name = jsonStringOr(*it, "name", std::string{});
            meta.sourceFormat = jsonStringOr(*it, "sourceFormat", std::string{});
        }
        if (auto it = doc->find("import"); it != doc->end() && it->is_object())
        {
            meta.import.sourcePath = jsonStringOr(*it, "sourcePath", std::string{});
            meta.import.sourceHash = jsonStringOr(*it, "sourceHash", std::string{});
            meta.import.importerVersion = static_cast<u32>(jsonU64Or(*it, "importerVersion", 1));
            meta.import.options = it->value("options", nlohmann::json::object());
        }
        if (auto it = doc->find("subAssets"); it != doc->end() && it->is_array())
        {
            for (const nlohmann::json& record : *it)
            {
                if (!record.is_object())
                {
                    continue;
                }
                ContainerMetadata::SubAsset sub;
                sub.subId = Uuid{ jsonU64Or(record, "subId", 0) };
                sub.type = assetTypeFromName(jsonStringOr(record, "type", std::string{ "mesh" }));
                sub.name = jsonStringOr(record, "name", std::string{});
                sub.chunk = static_cast<u32>(jsonU64Or(record, "chunk", 0));
                sub.colorspace = jsonStringOr(record, "colorspace", std::string{});
                sub.duration = jsonF32Or(record, "duration", 0.0f);
                meta.subAssets.push_back(std::move(sub));
            }
        }
        meta.materials = doc->value("materials", nlohmann::json::array());
        meta.nodes = doc->value("nodes", nlohmann::json::array());
        meta.skin = doc->value("skin", nlohmann::json{});
        meta.remap = doc->value("remap", nlohmann::json::object());
        return meta;
    }

    /// Headless check: round-trip a hand-built ContainerMetadata through a `.smodel`, asserting
    /// field-exact recovery and that an oversized metadata length is rejected (not crashed).
    void runContainerMetadataSelfTest()
    {
        ContainerMetadata meta;
        meta.modelId = Uuid{ 4242 };
        meta.name = "town";
        meta.sourceFormat = "gltf";
        meta.import.sourcePath = "raw/town.glb";
        meta.import.sourceHash = "abc123";
        meta.import.importerVersion = 1;
        meta.import.options = nlohmann::json{ { "scale", 1.0 }, { "axis", "y-up" } };
        meta.subAssets.push_back({ .subId = Uuid{ 11 }, .type = AssetType::Mesh, .name = "town_mesh", .chunk = 1 });
        meta.subAssets.push_back({ .subId = Uuid{ 12 },
                                   .type = AssetType::Texture,
                                   .name = "town_albedo",
                                   .chunk = 2,
                                   .colorspace = "srgb" });
        meta.subAssets.push_back({ .subId = Uuid{ 13 }, .type = AssetType::Material, .name = "stone", .chunk = 3 });
        meta.subAssets.push_back(
            { .subId = Uuid{ 14 }, .type = AssetType::Animation, .name = "walk", .chunk = 4, .duration = 1.2f });

        nlohmann::json material;
        material["subId"] = "13";
        material["baseColor"] = { 1.0, 1.0, 1.0, 1.0 };
        material["metallic"] = 0.0;
        material["roughness"] = 1.0;
        meta.materials.push_back(material);

        nlohmann::json node;
        node["name"] = "root";
        node["parent"] = -1;
        node["mesh"] = 0;
        meta.nodes.push_back(node);

        meta.skin = nlohmann::json::object();
        meta.skin["joints"] = nlohmann::json::array({ 0 });
        meta.skin["skeletonRoot"] = 0;
        meta.skin["meshNode"] = 1;

        std::vector<std::byte> metaBytes = encodeContainerMetadata(meta);

        // A large payload chunk proves the prefix read never touches payloads.
        std::vector<std::byte> payload(8192, std::byte{ 0x5A });
        const std::array<ContainerChunk, 2> chunks{
            ContainerChunk{ .kind = ChunkKind::Meta, .subId = 0, .flags = 0, .bytes = metaBytes },
            ContainerChunk{ .kind = ChunkKind::Mesh, .subId = 11, .flags = 0, .bytes = payload },
        };
        const std::string path = "/tmp/saffron_meta.smodel";
        if (auto wrote = writeContainer(path, chunks); !wrote)
        {
            logError(std::format("container-metadata self-test: write failed: {}", wrote.error()));
            return;
        }
        auto read = readContainerMetadata(path);
        if (!read)
        {
            logError(std::format("container-metadata self-test: read failed: {}", read.error()));
            return;
        }
        const f32 durationDelta = read->subAssets.size() == 4 ? read->subAssets[3].duration - 1.2f : 1.0f;
        const bool ok = read->modelId.value == meta.modelId.value && read->name == meta.name &&
                        read->sourceFormat == meta.sourceFormat && read->import.sourcePath == meta.import.sourcePath &&
                        read->import.sourceHash == meta.import.sourceHash && read->subAssets.size() == 4 &&
                        read->subAssets[0].type == AssetType::Mesh && read->subAssets[1].type == AssetType::Texture &&
                        read->subAssets[1].colorspace == "srgb" && read->subAssets[3].type == AssetType::Animation &&
                        durationDelta > -1e-4f && durationDelta < 1e-4f && read->nodes.is_array() &&
                        read->nodes.size() == 1 && !read->skin.is_null() && read->materials.is_array() &&
                        read->materials.size() == 1;
        if (ok)
        {
            logInfo(".smodel metadata round-trip OK");
        }
        else
        {
            logError(".smodel metadata round-trip MISMATCH");
        }

        std::ifstream raw(path, std::ios::binary | std::ios::ate);
        if (raw)
        {
            const std::streamsize size = raw.tellg();
            raw.seekg(0);
            std::vector<char> bytes(static_cast<std::size_t>(size));
            raw.read(bytes.data(), size);
            const u64 hugeMeta = static_cast<u64>(bytes.size()) * 4;
            std::memcpy(bytes.data() + offsetof(SModelHeader, metaLength), &hugeMeta, sizeof(hugeMeta));
            const std::string badPath = "/tmp/saffron_meta_badlen.smodel";
            if (std::ofstream out(badPath, std::ios::binary); out)
            {
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            }
            if (!readContainerMetadata(badPath).has_value())
            {
                logInfo(".smodel rejects an oversized metadata length");
            }
            else
            {
                logError(".smodel oversized-metadata check FAILED");
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
        clearThumbnailQueue(assets);  // abandon stale worker jobs for the old project (GPU is idle here)
        assets.meshRefByUuid.clear();
        assets.textureRefByUuid.clear();
        assets.modelRefByUuid.clear();
    }

    // --- On-disk thumbnail cache (engine-owned, one PNG per entry) ---
    //
    // <projectRoot>/cache/thumbnails/<uuid>-<size>-<stamp>.png, where <stamp> folds a cache-format
    // version with the source file's size + mtime. A hit is a single exists() after a stat; a stale
    // entry simply never matches its (now-changed) stamp again. The dir lives outside assets/, so the
    // catalog scan and project save/load never see it.

    // Bump when generation behaviour changes (the phase 1/2 mapping, a future format tweak) so every
    // existing entry's stamp stops matching and the whole cache regenerates.
    inline constexpr u32 ThumbnailCacheVersion = 1;

    auto thumbnailCacheDir(const AssetServer& assets) -> std::filesystem::path
    {
        if (assets.root.empty())
        {
            return {};
        }
        return std::filesystem::path(assets.root).parent_path() / "cache" / "thumbnails";
    }

    // Folds version + size + mtime (FNV-1a) into a compact hex token — the <stamp> filename field.
    auto foldThumbnailStamp(u64 version, u64 fileSize, u64 mtimeTicks) -> std::string
    {
        u64 h = 1469598103934665603ull;
        const auto mix = [&](u64 v)
        {
            h ^= v;
            h *= 1099511628211ull;
        };
        mix(version);
        mix(fileSize);
        mix(mtimeTicks);
        return std::format("{:016x}", h);
    }

    // A stamp of the asset's source file (its catalog `path` under assets/): size + mtime folded with
    // the cache version. Empty when the file is missing/unstattable — the entry is then never cached.
    auto thumbnailSourceStamp(const AssetServer& assets, const AssetEntry& entry) -> std::string
    {
        if (assets.root.empty() || entry.path.empty())
        {
            return {};
        }
        std::error_code ec;
        const std::filesystem::path src = std::filesystem::path(assets.root) / entry.path;
        const auto size = std::filesystem::file_size(src, ec);
        if (ec)
        {
            return {};
        }
        const auto mtime = std::filesystem::last_write_time(src, ec);
        if (ec)
        {
            return {};
        }
        return foldThumbnailStamp(ThumbnailCacheVersion, static_cast<u64>(size),
                                  static_cast<u64>(mtime.time_since_epoch().count()));
    }

    auto thumbnailCachePath(const AssetServer& assets, Uuid id, u32 size, const std::string& stamp)
        -> std::filesystem::path
    {
        return thumbnailCacheDir(assets) / std::format("{}-{}-{}.png", id.value, size, stamp);
    }

    // A cached thumbnail's bytes + the dimensions read from its PNG header (so a hit reports truthful
    // width/height without a decode). nullopt if the file is absent or not a readable PNG.
    struct CachedThumbnail
    {
        std::vector<u8> bytes;
        u32 width = 0;
        u32 height = 0;
    };

    auto readThumbnailCache(const std::filesystem::path& path) -> std::optional<CachedThumbnail>
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in)
        {
            return std::nullopt;
        }
        const std::streamsize size = in.tellg();
        if (size < 24)  // 8-byte signature + IHDR length/type + the width/height fields
        {
            return std::nullopt;
        }
        in.seekg(0);
        CachedThumbnail hit;
        hit.bytes.resize(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(hit.bytes.data()), size);
        if (!in)
        {
            return std::nullopt;
        }
        static constexpr std::array<u8, 8> pngSig{ 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };
        if (!std::equal(pngSig.begin(), pngSig.end(), hit.bytes.begin()))
        {
            return std::nullopt;
        }
        const auto be32 = [&](std::size_t at) -> u32
        {
            return (static_cast<u32>(hit.bytes[at]) << 24) | (static_cast<u32>(hit.bytes[at + 1]) << 16) |
                   (static_cast<u32>(hit.bytes[at + 2]) << 8) | static_cast<u32>(hit.bytes[at + 3]);
        };
        hit.width = be32(16);   // IHDR width
        hit.height = be32(20);  // IHDR height
        return hit;
    }

    auto writeThumbnailCache(const std::filesystem::path& path, const std::vector<u8>& bytes) -> Result<void>
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            return Err(std::format("cannot open thumbnail cache '{}'", path.string()));
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out)
        {
            return Err(std::format("write failed for thumbnail cache '{}'", path.string()));
        }
        return {};
    }

    // A material thumbnail keys on its *resolved* state, not a file stamp: editing a parent material
    // reflows every instance without touching the child .smat, so the key must be a content hash of
    // the resolved params + texture uuids. Folded with the cache version like the source stamp.
    auto thumbnailMaterialStamp(const MaterialAsset& m) -> std::string
    {
        u64 h = 1469598103934665603ull;
        const auto mix = [&](u64 v)
        {
            h ^= v;
            h *= 1099511628211ull;
        };
        const auto mixF = [&](f32 f) { mix(std::bit_cast<u32>(f)); };
        mix(ThumbnailCacheVersion);
        mixF(m.baseColor.r);
        mixF(m.baseColor.g);
        mixF(m.baseColor.b);
        mixF(m.baseColor.a);
        mixF(m.metallic);
        mixF(m.roughness);
        mixF(m.emissive.r);
        mixF(m.emissive.g);
        mixF(m.emissive.b);
        mixF(m.emissiveStrength);
        mixF(m.normalStrength);
        mixF(m.alphaCutoff);
        mixF(m.heightScale);
        mixF(m.uvTiling.x);
        mixF(m.uvTiling.y);
        mixF(m.uvOffset.x);
        mixF(m.uvOffset.y);
        mix(m.albedoTexture.value);
        mix(m.ormTexture.value);
        mix(m.normalTexture.value);
        mix(m.emissiveTexture.value);
        mix(m.heightTexture.value);
        mix(m.unlit ? 1u : 0u);
        mix(m.doubleSided ? 1u : 0u);
        for (char c : m.shader)
        {
            mix(static_cast<u8>(c));
        }
        for (char c : m.blend)
        {
            mix(static_cast<u8>(c));
        }
        return std::format("{:016x}", h);
    }

    // The leading uuid of a cache filename (<uuid>-<size>-<stamp>.png); 0 if it doesn't parse.
    auto thumbnailCacheFileUuid(const std::string& filename) -> u64
    {
        const std::size_t dash = filename.find('-');
        if (dash == std::string::npos)
        {
            return 0;
        }
        return std::strtoull(filename.substr(0, dash).c_str(), nullptr, 10);
    }

    // Removes every cached thumbnail for one asset uuid (all sizes/stamps) — on delete + reimport.
    void removeThumbnailCacheForAsset(const AssetServer& assets, Uuid id)
    {
        const std::filesystem::path dir = thumbnailCacheDir(assets);
        std::error_code ec;
        if (dir.empty() || !std::filesystem::exists(dir, ec))
        {
            return;
        }
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (thumbnailCacheFileUuid(entry.path().filename().string()) == id.value)
            {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }

    // Deletes cache files whose uuid is no longer in the catalog (re-import mints new uuids, orphaning
    // the old PNGs). Run on project load to keep the dir bounded without a background task.
    void sweepThumbnailCacheOrphans(const AssetServer& assets)
    {
        const std::filesystem::path dir = thumbnailCacheDir(assets);
        std::error_code ec;
        if (dir.empty() || !std::filesystem::exists(dir, ec))
        {
            return;
        }
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, ec))
        {
            const u64 uuid = thumbnailCacheFileUuid(entry.path().filename().string());
            if (uuid == 0 || !assets.catalog.byId.contains(uuid))
            {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }

    struct ThumbnailCacheStats
    {
        u32 entries = 0;
        u64 bytes = 0;
    };

    auto thumbnailCacheStats(const AssetServer& assets) -> ThumbnailCacheStats
    {
        ThumbnailCacheStats stats;
        const std::filesystem::path dir = thumbnailCacheDir(assets);
        std::error_code ec;
        if (dir.empty() || !std::filesystem::exists(dir, ec))
        {
            return stats;
        }
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file(ec))
            {
                continue;
            }
            stats.entries += 1;
            stats.bytes += static_cast<u64>(entry.file_size(ec));
        }
        return stats;
    }

    // Empties the project's cache dir; returns what was removed (count + bytes).
    auto clearThumbnailCacheDir(const AssetServer& assets) -> ThumbnailCacheStats
    {
        const ThumbnailCacheStats removed = thumbnailCacheStats(assets);
        const std::filesystem::path dir = thumbnailCacheDir(assets);
        std::error_code ec;
        if (!dir.empty() && std::filesystem::exists(dir, ec))
        {
            for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, ec))
            {
                std::filesystem::remove(entry.path(), ec);
            }
        }
        return removed;
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
        // The filesystem is the source of truth: build the catalog from disk via the regenerable cache
        // (a cold scan when the cache misses), so a never-saved import (a .smodel on disk the
        // project.json never recorded) is rediscovered and a deleted file's row is dropped. The project
        // .json names loaded just above seed the scan's name preservation. Caches were just cleared, so
        // no GPU patch is needed here.
        if (auto scan = loadCatalog(assets); scan)
        {
            if (!scan->added.empty() || !scan->removed.empty())
            {
                logInfo(std::format("scan: reconciled catalog with disk (+{} -{})", scan->added.size(),
                                    scan->removed.size()));
            }
        }
        else
        {
            logWarn(std::format("scan: {}", scan.error()));
        }
        sweepThumbnailCacheOrphans(assets);  // drop cache files for uuids no longer in the catalog
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
        return nlohmann::json{ { "version", 1 },
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
                               { "overrides", m.overrides.is_null() ? nlohmann::json::object() : m.overrides } };
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
                m.baseColor =
                    glm::vec4{ (*v)[0].get<f32>(), (*v)[1].get<f32>(), (*v)[2].get<f32>(), (*v)[3].get<f32>() };
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
                    m.baseColor =
                        glm::vec4{ value[0].get<f32>(), value[1].get<f32>(), value[2].get<f32>(), value[3].get<f32>() };
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
    // Lowers a node graph to a Slang evalSurface body. mesh=false targets the self-contained preview/shell
    // shader (a `Mat mat` push + `textures[]` + `uv` param, 5-field SurfaceData); mesh=true targets the
    // übershader's evalSurface(MaterialInput m) — `m.mat` (MaterialParams), `albedoTextures[]`, a `uv`
    // local the splice template provides, and the 7-field SurfaceData (world normal + occlusion/opacity).
    auto emitGraphSurface(const nlohmann::json& graph, bool mesh = false) -> std::string
    {
        const std::string baseColor = mesh ? "m.mat.baseColor" : "mat.baseColor";
        std::string body = std::format("    s.albedo = {}.rgb;\n    s.metallic = 0.0;\n    s.roughness = 1.0;\n"
                                       "    s.emissive = float3(0.0);\n",
                                       baseColor);
        if (mesh)
        {
            body += std::format("    s.normal = normalize(m.worldNormal);\n    s.occlusion = 1.0;\n"
                                "    s.opacity = {}.a;\n",
                                baseColor);
        }
        else
        {
            body += "    s.normal = float3(0.0, 0.0, 1.0);\n";
        }
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
                const std::string slot = props.value("slot", std::string{ "albedo" });
                const std::string arr = mesh ? "albedoTextures" : "textures";
                std::string idx;
                if (mesh)
                {
                    if (slot == "metallicRoughness" || slot == "mr")
                    {
                        idx = "m.mat.tex0.y";
                    }
                    else if (slot == "normal")
                    {
                        idx = "m.mat.tex0.z";
                    }
                    else if (slot == "emissive")
                    {
                        idx = "m.mat.tex0.w";
                    }
                    else if (slot == "height")
                    {
                        idx = "m.mat.tex1.x";
                    }
                    else if (slot == "occlusion")
                    {
                        idx = "m.mat.tex1.y";
                    }
                    else
                    {
                        idx = "m.mat.tex0.x";
                    }
                }
                else
                {
                    if (slot == "metallicRoughness" || slot == "mr")
                    {
                        idx = "mat.tex.y";
                    }
                    else if (slot == "normal")
                    {
                        idx = "mat.tex.z";
                    }
                    else if (slot == "emissive")
                    {
                        idx = "mat.tex.w";
                    }
                    else
                    {
                        idx = "mat.tex.x";
                    }
                }
                body += std::format("    float4 n_{} = {}[NonUniformResourceIndex({})].Sample(uv);\n", id, arr, idx);
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

    // Scene-path codegen: splice the graph's evalSurface into the real übershader (mesh.slang) so a
    // codegen material renders on actual entities with full lighting. Reads the runtime mesh.slang,
    // replaces the body between the @graph markers with the mesh-context emit, and slangc-compiles a
    // per-material übershader variant. Returns the .spv path; set it as the material's Material.shader.
    auto compileMaterialMeshShader(AssetServer& assets, const nlohmann::json& graph, Uuid id) -> Result<std::string>
    {
        const std::string slangc = findSlangc();
        const std::string shadersDir = assetPath("shaders");  // holds lighting.slang-module for `import lighting`
        const std::string meshSrcPath = shadersDir + "/mesh.slang";
        std::ifstream in(meshSrcPath);
        if (!in)
        {
            return Err(
                std::format("cannot read übershader source '{}' (is the .slang copied to bin/shaders?)", meshSrcPath));
        }
        std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string beginMark = "// @graph-begin";
        const std::string endMark = "// @graph-end";
        const auto b = src.find(beginMark);
        const auto e = src.find(endMark);
        if (b == std::string::npos || e == std::string::npos || e < b)
        {
            return Err(std::string{ "übershader source is missing the @graph markers" });
        }
        const auto bodyStart = src.find('\n', b);  // keep the begin-marker line, drop the default body
        const std::string emitted = emitGraphSurface(graph, /*mesh=*/true);
        const std::string spliced = src.substr(0, bodyStart + 1) + emitted + "    " + src.substr(e);

        ensureAssetDirectories(assets);
        const std::string slangPath = assets.root + "/materials/" + std::to_string(id.value) + "_mesh.slang";
        const std::string spvPath = assets.root + "/materials/" + std::to_string(id.value) + "_mesh.spv";
        {
            std::ofstream out(slangPath);
            if (!out)
            {
                return Err(std::format("cannot write generated übershader '{}'", slangPath));
            }
            out << spliced;
        }
        const std::string cmd = "\"" + slangc + "\" \"" + slangPath +
                                "\" -profile glsl_450 -target spirv -emit-spirv-directly -fvk-use-entrypoint-name "
                                "-matrix-layout-column-major -I \"" +
                                shadersDir + "\" -o \"" + spvPath + "\" > /dev/null 2>&1";
        const int rc = std::system(cmd.c_str());
        if (rc != 0 || !std::filesystem::exists(spvPath))
        {
            return Err(std::format("slangc failed (rc={}) compiling übershader variant {}", rc, id.value));
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
                m.baseColor =
                    glm::vec4{ value[0].get<f32>(), value[1].get<f32>(), value[2].get<f32>(), value[3].get<f32>() };
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

    /// A whole file, or a byte slice of one. `length == 0` means the whole file; a non-zero length
    /// reads exactly [offset, offset+length) — a `.smodel` chunk. Lets the loaders below treat a
    /// standalone file and an embedded chunk identically.
    struct ByteSource
    {
        std::string path;
        u64 offset = 0;
        u64 length = 0;

        auto read() const -> Result<std::vector<std::byte>>
        {
            std::ifstream in(path, std::ios::binary | std::ios::ate);
            if (!in)
            {
                return Err(std::format("cannot open '{}'", path));
            }
            const auto fileSize = static_cast<u64>(in.tellg());
            u64 begin = offset;
            u64 count = length;
            if (count == 0)
            {
                begin = 0;
                count = fileSize;
            }
            if (begin + count > fileSize)
            {
                return Err(std::format("slice [{}, {}) exceeds '{}' ({} bytes)", begin, begin + count, path, fileSize));
            }
            std::vector<std::byte> bytes(static_cast<std::size_t>(count));
            in.seekg(static_cast<std::streamoff>(begin));
            in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count));
            if (!in)
            {
                return Err(std::format("read failed for '{}'", path));
            }
            return bytes;
        }
    };

    /// An opened `.smodel`: its prefix metadata plus a chunk reader that slices payloads lazily.
    /// Negative-cached in modelRefByUuid like a mesh; dropped on a project switch with the GPU idle.
    struct ModelAsset
    {
        ContainerMetadata meta;
        ContainerReader reader;
    };

    auto loadModelAsset(AssetServer& assets, Uuid modelId) -> Ref<ModelAsset>
    {
        auto cached = assets.modelRefByUuid.find(modelId.value);
        if (cached != assets.modelRefByUuid.end())
        {
            return cached->second;
        }
        const AssetEntry* entry = findAsset(assets.catalog, modelId);
        if (entry == nullptr || entry->type != AssetType::Model)
        {
            assets.modelRefByUuid[modelId.value] = nullptr;
            return nullptr;
        }
        const std::string fullPath = assets.root + "/" + entry->path;
        auto meta = readContainerMetadata(fullPath);
        if (!meta)
        {
            logWarn(std::format("model {}: {}", modelId.value, meta.error()));
            assets.modelRefByUuid[modelId.value] = nullptr;
            return nullptr;
        }
        auto reader = readContainer(fullPath);
        if (!reader)
        {
            logWarn(std::format("model {}: {}", modelId.value, reader.error()));
            assets.modelRefByUuid[modelId.value] = nullptr;
            return nullptr;
        }
        auto model = std::make_shared<ModelAsset>(ModelAsset{ .meta = std::move(*meta), .reader = std::move(*reader) });
        assets.modelRefByUuid[modelId.value] = model;
        return model;
    }

    // Resolution order for a sub-id: a remap (extracted/external file) wins, falling back to the
    // embedded chunk with a warning if the external file is gone; otherwise the embedded chunk. An
    // empty path means the sub-id has no such chunk.
    auto chunkSourceFor(const AssetServer& assets, const ModelAsset& model, ChunkKind kind, Uuid subId) -> ByteSource
    {
        const std::string key = std::to_string(subId.value);
        if (model.meta.remap.is_object() && model.meta.remap.contains(key))
        {
            const nlohmann::json& entry = model.meta.remap[key];
            if (entry.is_object() && entry.contains("external") && entry["external"].is_string())
            {
                const std::string external = entry["external"].get<std::string>();
                const std::string externalPath = assets.root + "/" + external;
                if (std::filesystem::exists(externalPath))
                {
                    return ByteSource{ .path = externalPath };
                }
                logWarn(std::format("model {}: remap target '{}' for sub-asset {} is missing; using the embedded chunk",
                                    model.meta.modelId.value, external, subId.value));
            }
        }
        const TocEntry* entry = model.reader.find(kind, subId.value);
        if (entry == nullptr)
        {
            return ByteSource{};
        }
        return ByteSource{ .path = model.reader.path, .offset = entry->offset, .length = entry->length };
    }

    // Loads + uploads a mesh from any byte source (file or chunk slice), caching the GPU Ref under
    // the sub-id. A failure is negative-cached so a broken sub-asset is not retried each frame.
    auto loadMeshFromSource(AssetServer& assets, Renderer& renderer, Uuid subId, const ByteSource& source)
        -> Ref<GpuMesh>
    {
        auto cached = assets.meshRefByUuid.find(subId.value);
        if (cached != assets.meshRefByUuid.end())
        {
            return cached->second;
        }
        auto bytes = source.read();
        if (!bytes)
        {
            logWarn(std::format("mesh {}: {}", subId.value, bytes.error()));
            assets.meshRefByUuid[subId.value] = nullptr;
            return nullptr;
        }
        auto mesh = loadMeshFromBytes(std::span<const std::byte>{ *bytes });
        if (!mesh)
        {
            logWarn(std::format("mesh {}: {}", subId.value, mesh.error()));
            assets.meshRefByUuid[subId.value] = nullptr;
            return nullptr;
        }
        std::vector<VertexSkin> skin;
        if (auto loadedSkin = loadMeshSkinFromBytes(std::span<const std::byte>{ *bytes }))
        {
            skin = std::move(*loadedSkin);
        }
        auto meshRef = uploadMesh(renderer, *mesh, skin);
        if (!meshRef)
        {
            logWarn(std::format("mesh {}: {}", subId.value, meshRef.error()));
            assets.meshRefByUuid[subId.value] = nullptr;
            return nullptr;
        }
        assets.meshRefByUuid[subId.value] = *meshRef;
        return *meshRef;
    }

    // Loads + uploads a texture from any byte source, picking the upload format from the colorspace
    // (Hdr → float; Linear → unorm; Srgb/Auto → sRGB). Caches the GPU Ref under the sub-id.
    auto loadTextureFromSource(AssetServer& assets, Renderer& renderer, Uuid subId, const ByteSource& source,
                               Colorspace space) -> Ref<GpuTexture>
    {
        auto cached = assets.textureRefByUuid.find(subId.value);
        if (cached != assets.textureRefByUuid.end())
        {
            return cached->second;
        }
        auto bytes = source.read();
        if (!bytes)
        {
            logWarn(std::format("texture {}: {}", subId.value, bytes.error()));
            assets.textureRefByUuid[subId.value] = nullptr;
            return nullptr;
        }
        std::vector<u8> encoded(bytes->size());
        if (!bytes->empty())
        {
            std::memcpy(encoded.data(), bytes->data(), bytes->size());
        }
        if (space == Colorspace::Hdr)
        {
            auto decoded = decodeImageFromMemoryHdr(encoded);
            if (decoded)
            {
                auto texture = uploadTextureFloat(renderer, decoded->rgba.data(), decoded->width, decoded->height);
                if (texture)
                {
                    assets.textureRefByUuid[subId.value] = *texture;
                    return *texture;
                }
                logWarn(std::format("texture {}: {}", subId.value, texture.error()));
            }
            else
            {
                logWarn(std::format("texture {}: {}", subId.value, decoded.error()));
            }
            assets.textureRefByUuid[subId.value] = nullptr;
            return nullptr;
        }
        auto decoded = decodeImageFromMemory(encoded);
        if (decoded)
        {
            const bool srgb = space != Colorspace::Linear;
            auto texture = uploadTexture(renderer, decoded->rgba.data(), decoded->width, decoded->height, srgb);
            if (texture)
            {
                assets.textureRefByUuid[subId.value] = *texture;
                return *texture;
            }
            logWarn(std::format("texture {}: {}", subId.value, texture.error()));
        }
        else
        {
            logWarn(std::format("texture {}: {}", subId.value, decoded.error()));
        }
        assets.textureRefByUuid[subId.value] = nullptr;
        return nullptr;
    }

    /// Resolve an embedded mesh sub-asset to a live GPU mesh, honoring the remap table; keyed by sub-id.
    auto resolveMesh(AssetServer& assets, Renderer& renderer, Uuid modelId, Uuid subId) -> Ref<GpuMesh>
    {
        auto cached = assets.meshRefByUuid.find(subId.value);
        if (cached != assets.meshRefByUuid.end())
        {
            return cached->second;
        }
        auto model = loadModelAsset(assets, modelId);
        if (!model)
        {
            assets.meshRefByUuid[subId.value] = nullptr;
            return nullptr;
        }
        const ByteSource source = chunkSourceFor(assets, *model, ChunkKind::Mesh, subId);
        if (source.path.empty())
        {
            logWarn(std::format("model {}: no mesh sub-asset {}", modelId.value, subId.value));
            assets.meshRefByUuid[subId.value] = nullptr;
            return nullptr;
        }
        return loadMeshFromSource(assets, renderer, subId, source);
    }

    /// Resolve an embedded texture sub-asset to a live GPU texture (colorspace from the chunk flags).
    auto resolveTexture(AssetServer& assets, Renderer& renderer, Uuid modelId, Uuid subId) -> Ref<GpuTexture>
    {
        auto cached = assets.textureRefByUuid.find(subId.value);
        if (cached != assets.textureRefByUuid.end())
        {
            return cached->second;
        }
        auto model = loadModelAsset(assets, modelId);
        if (!model)
        {
            assets.textureRefByUuid[subId.value] = nullptr;
            return nullptr;
        }
        Colorspace space = Colorspace::Srgb;
        if (const TocEntry* texEntry = model->reader.find(ChunkKind::Texture, subId.value); texEntry != nullptr)
        {
            space = static_cast<Colorspace>(texEntry->flags);
        }
        const ByteSource source = chunkSourceFor(assets, *model, ChunkKind::Texture, subId);
        if (source.path.empty())
        {
            logWarn(std::format("model {}: no texture sub-asset {}", modelId.value, subId.value));
            assets.textureRefByUuid[subId.value] = nullptr;
            return nullptr;
        }
        return loadTextureFromSource(assets, renderer, subId, source, space);
    }

    /// Resolve an embedded material sub-asset to a CPU MaterialAsset (parsed from the .smat chunk).
    auto resolveMaterial(AssetServer& assets, Uuid modelId, Uuid subId) -> Result<MaterialAsset>
    {
        auto model = loadModelAsset(assets, modelId);
        if (!model)
        {
            return Err(std::format("model {} is not loadable", modelId.value));
        }
        const ByteSource source = chunkSourceFor(assets, *model, ChunkKind::Material, subId);
        if (source.path.empty())
        {
            return Err(std::format("model {} has no material sub-asset {}", modelId.value, subId.value));
        }
        auto bytes = source.read();
        if (!bytes)
        {
            return Err(bytes.error());
        }
        std::string text(bytes->size(), '\0');
        if (!bytes->empty())
        {
            std::memcpy(text.data(), bytes->data(), bytes->size());
        }
        auto doc = parseJson(text);
        if (!doc)
        {
            return Err(doc.error());
        }
        return materialAssetFromJson(*doc);
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
            // Dangling reference: a material/scene names a texture not in the catalog. Warn once
            // (negative-cache), and let the draw path fall back to the default white texture.
            logWarn(std::format("texture {} not in catalog; using default", id.value));
            assets.textureRefByUuid[id.value] = nullptr;
            return nullptr;
        }
        // An embedded sub-asset resolves through its container's chunk (colorspace from the flags).
        if (entry->container.value != 0)
        {
            return resolveTexture(assets, renderer, entry->container, id);
        }
        // A standalone image file: an explicit colorspace (from a .smeta) wins; otherwise fall back to
        // the row's hdr/linear provenance (engine-written textures set those at registration).
        Colorspace space = Colorspace::Srgb;
        if (entry->colorspace != Colorspace::Auto)
        {
            space = entry->colorspace;
        }
        else if (entry->hdr)
        {
            space = Colorspace::Hdr;
        }
        else if (entry->linear)
        {
            space = Colorspace::Linear;
        }
        return loadTextureFromSource(assets, renderer, id, ByteSource{ .path = assets.root + "/" + entry->path },
                                     space);
    }

    /// Loads an animation clip by id into a CPU AnimClip. An embedded clip reads its SANM chunk through
    /// the owning container; a standalone clip reads its file. The animation runtime calls this on a
    /// cache miss (the Host injects it, since the container reader lives here).
    auto loadAnimationClipAsset(AssetServer& assets, Uuid id) -> Result<AnimClip>
    {
        const AssetEntry* entry = findAsset(assets.catalog, id);
        if (entry == nullptr || entry->type != AssetType::Animation)
        {
            return Err(std::format("animation clip {} not in catalog", id.value));
        }
        if (entry->container.value != 0)
        {
            auto model = loadModelAsset(assets, entry->container);
            if (!model)
            {
                return Err(std::format("clip {}: container {} is not loadable", id.value, entry->container.value));
            }
            const ByteSource source = chunkSourceFor(assets, *model, ChunkKind::Animation, id);
            if (source.path.empty())
            {
                return Err(std::format("container {} has no animation sub-asset {}", entry->container.value, id.value));
            }
            auto bytes = source.read();
            if (!bytes)
            {
                return Err(bytes.error());
            }
            return loadAnimationFromBytes(std::as_bytes(std::span{ *bytes }));
        }
        return loadAnimation(assets.root + "/" + entry->path);
    }


    // FNV-1a 64-bit of a file's bytes, as a decimal string. Content-addressed (not mtime) so a
    // reimport whose source is byte-identical is skipped (phase 13). Empty string on a read failure.
    auto hashFileFnv(const std::string& path) -> std::string
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return std::string{};
        }
        u64 hash = 1469598103934665603ull;
        std::array<char, 8192> buffer{};
        while (in)
        {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize got = in.gcount();
            for (std::streamsize i = 0; i < got; i = i + 1)
            {
                hash = hash ^ static_cast<u8>(buffer[static_cast<std::size_t>(i)]);
                hash = hash * 1099511628211ull;
            }
        }
        return std::to_string(hash);
    }

    // The import node forest as the MetadataChunk `nodes` block (glTF-shaped; quaternion as w,x,y,z).
    auto importedNodesToJson(const std::vector<ImportedNode>& nodes) -> nlohmann::json
    {
        nlohmann::json array = nlohmann::json::array();
        for (const ImportedNode& node : nodes)
        {
            nlohmann::json record;
            record["name"] = node.name;
            record["parent"] = node.parent;
            record["t"] = { node.translation.x, node.translation.y, node.translation.z };
            record["r"] = { node.rotation.w, node.rotation.x, node.rotation.y, node.rotation.z };
            record["s"] = { node.scale.x, node.scale.y, node.scale.z };
            array.push_back(std::move(record));
        }
        return array;
    }

    // The skin descriptor as the MetadataChunk `skin` block; inverse-bind matrices are 16 floats
    // each, column-major (the glm layout), so the reader can memcpy them straight back.
    auto importedSkinToJson(const ImportedSkin& skin) -> nlohmann::json
    {
        nlohmann::json record;
        record["joints"] = skin.joints;
        nlohmann::json inverseBind = nlohmann::json::array();
        for (const glm::mat4& matrix : skin.inverseBind)
        {
            nlohmann::json flat = nlohmann::json::array();
            for (int column = 0; column < 4; column = column + 1)
            {
                for (int row = 0; row < 4; row = row + 1)
                {
                    flat.push_back(matrix[column][row]);
                }
            }
            inverseBind.push_back(std::move(flat));
        }
        record["inverseBind"] = std::move(inverseBind);
        record["skeletonRoot"] = skin.skeletonRoot;
        record["meshNode"] = skin.meshNode;
        return record;
    }

    // Inverse of importedNodesToJson: the MetadataChunk `nodes` block back into the spawn input.
    auto importedNodesFromJson(const nlohmann::json& nodes) -> std::vector<ImportedNode>
    {
        std::vector<ImportedNode> out;
        if (!nodes.is_array())
        {
            return out;
        }
        for (const nlohmann::json& record : nodes)
        {
            if (!record.is_object())
            {
                continue;
            }
            ImportedNode node;
            node.name = record.value("name", std::string{});
            node.parent = record.value("parent", -1);
            if (auto it = record.find("t"); it != record.end() && it->is_array() && it->size() == 3)
            {
                node.translation = glm::vec3((*it)[0].get<f32>(), (*it)[1].get<f32>(), (*it)[2].get<f32>());
            }
            if (auto it = record.find("r"); it != record.end() && it->is_array() && it->size() == 4)
            {
                node.rotation =
                    glm::quat((*it)[0].get<f32>(), (*it)[1].get<f32>(), (*it)[2].get<f32>(), (*it)[3].get<f32>());
            }
            if (auto it = record.find("s"); it != record.end() && it->is_array() && it->size() == 3)
            {
                node.scale = glm::vec3((*it)[0].get<f32>(), (*it)[1].get<f32>(), (*it)[2].get<f32>());
            }
            out.push_back(std::move(node));
        }
        return out;
    }

    // Inverse of importedSkinToJson: the MetadataChunk `skin` block back into the spawn descriptor.
    auto importedSkinFromJson(const nlohmann::json& skin) -> ImportedSkin
    {
        ImportedSkin out;
        if (!skin.is_object())
        {
            return out;
        }
        if (auto it = skin.find("joints"); it != skin.end() && it->is_array())
        {
            for (const nlohmann::json& joint : *it)
            {
                out.joints.push_back(joint.get<i32>());
            }
        }
        out.skeletonRoot = skin.value("skeletonRoot", -1);
        out.meshNode = skin.value("meshNode", -1);
        if (auto it = skin.find("inverseBind"); it != skin.end() && it->is_array())
        {
            for (const nlohmann::json& flat : *it)
            {
                glm::mat4 matrix(1.0f);
                if (flat.is_array() && flat.size() == 16)
                {
                    for (int column = 0; column < 4; column = column + 1)
                    {
                        for (int row = 0; row < 4; row = row + 1)
                        {
                            matrix[column][row] = flat[static_cast<std::size_t>(column * 4 + row)].get<f32>();
                        }
                    }
                }
                out.inverseBind.push_back(matrix);
            }
        }
        return out;
    }

    // The catalog rows a container contributes: one AssetType::Model parent + one row per embedded
    // sub-asset (container linkage + chunk index + colorspace). Shared by bakeModel and the scan so
    // a freshly baked container and a rediscovered one yield identical rows.
    auto catalogRowsForModel(const ContainerMetadata& meta, const std::string& relativePath) -> std::vector<AssetEntry>
    {
        std::vector<AssetEntry> rows;
        rows.push_back(
            AssetEntry{ .id = meta.modelId, .name = meta.name, .type = AssetType::Model, .path = relativePath });
        for (const ContainerMetadata::SubAsset& sub : meta.subAssets)
        {
            AssetEntry row;
            row.id = sub.subId;
            row.name = sub.name;
            row.type = sub.type;
            row.colorspace = colorspaceFromName(sub.colorspace);
            row.duration = sub.duration;
            // An extracted (remapped) sub-asset is a standalone file: its row points at the external
            // path, not the container, so a scan agrees with the resolver and the ids never alias.
            const std::string key = std::to_string(sub.subId.value);
            bool remapped = false;
            if (meta.remap.is_object() && meta.remap.contains(key))
            {
                const nlohmann::json& entry = meta.remap.at(key);
                if (entry.is_object() && entry.contains("external") && entry.at("external").is_string())
                {
                    row.path = entry.at("external").get<std::string>();
                    row.container = Uuid{ 0 };
                    row.chunk = -1;
                    remapped = true;
                }
            }
            if (!remapped)
            {
                row.path = relativePath;
                row.container = meta.modelId;
                row.chunk = static_cast<i32>(sub.chunk);
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

    // Bakes an import graph into one self-contained assets/models/<uuid>.smodel: the mesh, each
    // material as a first-class .smat-JSON chunk, each texture as a raw chunk (colorspace in the
    // chunk flags), each clip as a .sanim chunk, and a MetadataChunk carrying the node/skin
    // hierarchy plus the deterministic reimport recipe. No GPU upload, no entity spawn. modelId is
    // reused on reimport (0 mints a fresh one). Sub-ids are stable (subIdFor), keyed by source name.
    auto bakeModel(AssetServer& assets, const ImportedModel& graph, const ImportOptions& options,
                   const std::string& sourcePath, Uuid modelId) -> Result<BakeResult>
    {
        if (modelId.value == 0)
        {
            modelId = newUuid();
        }
        const std::filesystem::path source(sourcePath);
        const std::string modelKey = source.stem().string();
        std::string sourceFormat = "obj";
        const std::string ext = source.extension().string();
        if (ext == ".gltf" || ext == ".glb")
        {
            sourceFormat = "gltf";
        }

        // Each chunk's bytes are owned here; META sits at index 0 (front-loaded by writeContainer),
        // its bytes filled in last once every sub-asset's TOC index is known.
        struct Pending
        {
            ChunkKind kind;
            u64 subId;
            u32 flags;
            std::vector<std::byte> bytes;
        };
        std::vector<Pending> pending;
        pending.push_back(Pending{ ChunkKind::Meta, 0, 0, {} });

        ContainerMetadata meta;
        meta.modelId = modelId;
        meta.name = modelKey;
        meta.sourceFormat = sourceFormat;
        meta.import.sourcePath = sourcePath;
        meta.import.sourceHash = hashFileFnv(sourcePath);
        meta.import.importerVersion = ImporterVersion;
        meta.import.options = options.toJson();

        const Uuid meshSubId = subIdFor(modelKey, "mesh", "0", 0);
        std::vector<std::byte> meshBytes;
        if (graph.hasSkin)
        {
            auto buffer = saveMeshSkinnedToBuffer(graph.mesh, graph.skin);
            if (!buffer)
            {
                return Err(buffer.error());
            }
            meshBytes = std::move(*buffer);
        }
        else
        {
            meshBytes = saveMeshToBuffer(graph.mesh);
        }
        const u32 meshChunk = static_cast<u32>(pending.size());
        pending.push_back(Pending{ ChunkKind::Mesh, meshSubId.value, 0, std::move(meshBytes) });
        meta.subAssets.push_back(
            { .subId = meshSubId, .type = AssetType::Mesh, .name = modelKey + "_mesh", .chunk = meshChunk });

        for (std::size_t m = 0; m < graph.materials.size(); m = m + 1)
        {
            const ImportedMaterial& src = graph.materials[m];
            std::string materialName = src.name;
            if (materialName.empty())
            {
                materialName = std::format("material_{}", m);
            }

            MaterialAsset materialAsset;
            materialAsset.baseColor = src.baseColor;
            materialAsset.metallic = src.metallic;
            materialAsset.roughness = src.roughness;
            materialAsset.emissive = src.emissive;
            materialAsset.emissiveStrength = src.emissiveStrength;

            auto emitTexture = [&](const std::vector<u8>& bytes, MaterialMapRole role, const char* roleName) -> Uuid
            {
                const Uuid texId = subIdFor(modelKey, "texture", std::format("{}_{}", m, roleName), 0);
                const Colorspace space = options.colorspaceFor(role);
                std::vector<std::byte> chunk(bytes.size());
                if (!bytes.empty())
                {
                    std::memcpy(chunk.data(), bytes.data(), bytes.size());
                }
                const u32 index = static_cast<u32>(pending.size());
                pending.push_back(
                    Pending{ ChunkKind::Texture, texId.value, static_cast<u32>(space), std::move(chunk) });
                meta.subAssets.push_back({ .subId = texId,
                                           .type = AssetType::Texture,
                                           .name = std::format("{}_{}", materialName, roleName),
                                           .chunk = index,
                                           .colorspace = colorspaceName(space) });
                return texId;
            };

            if (src.hasAlbedo)
            {
                materialAsset.albedoTexture = emitTexture(src.albedoBytes, MaterialMapRole::Albedo, "albedo");
            }
            if (src.hasMetallicRoughness)
            {
                materialAsset.ormTexture =
                    emitTexture(src.metallicRoughnessBytes, MaterialMapRole::MetallicRoughness, "orm");
            }
            if (src.hasNormal)
            {
                materialAsset.normalTexture = emitTexture(src.normalBytes, MaterialMapRole::Normal, "normal");
            }
            if (src.hasEmissiveTex)
            {
                materialAsset.emissiveTexture =
                    emitTexture(src.emissiveTexBytes, MaterialMapRole::Emissive, "emissive");
            }
            // Occlusion has no dedicated .smat slot in v1 (the format packs AO into orm); its bytes
            // are not embedded — a documented v1 gap, not a silent drop.

            const Uuid materialId = subIdFor(modelKey, "material", materialName, static_cast<u32>(m));
            const std::string materialJson = dumpJson(materialAssetToJson(materialAsset));
            std::vector<std::byte> materialChunk(materialJson.size());
            std::memcpy(materialChunk.data(), materialJson.data(), materialJson.size());
            const u32 materialChunkIndex = static_cast<u32>(pending.size());
            pending.push_back(Pending{ ChunkKind::Material, materialId.value, 0, std::move(materialChunk) });
            meta.subAssets.push_back({ .subId = materialId,
                                       .type = AssetType::Material,
                                       .name = materialName,
                                       .chunk = materialChunkIndex });

            nlohmann::json summary;
            summary["subId"] = uuidToJson(materialId.value);
            summary["baseColor"] = { src.baseColor.x, src.baseColor.y, src.baseColor.z, src.baseColor.w };
            summary["metallic"] = src.metallic;
            summary["roughness"] = src.roughness;
            meta.materials.push_back(std::move(summary));
        }

        for (std::size_t a = 0; a < graph.animations.size(); a = a + 1)
        {
            const AnimClip& clip = graph.animations[a];
            std::string clipName = clip.name;
            if (clipName.empty())
            {
                clipName = std::format("clip_{}", a);
            }
            const Uuid clipId = subIdFor(modelKey, "animation", clipName, static_cast<u32>(a));
            std::vector<std::byte> clipBytes = saveAnimationToBuffer(clip);
            const u32 clipChunk = static_cast<u32>(pending.size());
            pending.push_back(Pending{ ChunkKind::Animation, clipId.value, 0, std::move(clipBytes) });
            ContainerMetadata::SubAsset sub;
            sub.subId = clipId;
            sub.type = AssetType::Animation;
            sub.name = clipName;
            sub.chunk = clipChunk;
            sub.duration = clip.duration;
            meta.subAssets.push_back(std::move(sub));
        }

        meta.nodes = importedNodesToJson(graph.nodes);
        if (graph.hasSkin)
        {
            meta.skin = importedSkinToJson(graph.skinDesc);
        }

        pending[0].bytes = encodeContainerMetadata(meta);

        std::vector<ContainerChunk> chunks;
        chunks.reserve(pending.size());
        for (const Pending& chunk : pending)
        {
            chunks.push_back(
                ContainerChunk{ .kind = chunk.kind, .subId = chunk.subId, .flags = chunk.flags, .bytes = chunk.bytes });
        }
        const std::string relativePath = "models/" + std::to_string(modelId.value) + ".smodel";
        ensureAssetDirectories(assets);
        if (auto wrote = writeContainer(assets.root + "/" + relativePath, chunks); !wrote)
        {
            return Err(wrote.error());
        }

        BakeResult bake;
        bake.modelId = modelId;
        bake.path = relativePath;
        bake.rows = catalogRowsForModel(meta, relativePath);
        return bake;
    }

    // Imports a model source by translating it, baking it into one `.smodel`, and adding the catalog
    // rows it contributes. Produces an asset; does not upload to the GPU or spawn an entity. Pair with
    // instantiateModel to place it. (No renderer — baking is pure disk + catalog.)
    auto importModel(AssetServer& assets, const std::string& path, const ImportOptions& options)
        -> Result<BakeResult>
    {
        auto graph = translateModel(path);
        if (!graph)
        {
            return Err(graph.error());
        }
        auto bake = bakeModel(assets, *graph, options, path, Uuid{ 0 });
        if (!bake)
        {
            return Err(bake.error());
        }
        for (const AssetEntry& row : bake->rows)
        {
            putAsset(assets.catalog, row);
        }
        return bake;
    }

    /// Ensures a built-in model asset (the editor's add-entity cube preset) exists, baking it once under
    /// a deterministic id derived from its source name so repeated use reuses the same container rather
    /// than re-baking or colliding on the source-name sub-ids. Returns the model id to instantiate.
    auto ensureBuiltinModelAsset(AssetServer& assets, const std::string& sourcePath) -> Result<Uuid>
    {
        const std::string key = std::filesystem::path(sourcePath).stem().string();
        const Uuid modelId = subIdFor(key, "model", "0", 0);
        if (findAsset(assets.catalog, modelId) != nullptr)
        {
            return modelId;
        }
        auto graph = translateModel(sourcePath);
        if (!graph)
        {
            return Err(graph.error());
        }
        auto bake = bakeModel(assets, *graph, ImportOptions{}, sourcePath, modelId);
        if (!bake)
        {
            return Err(bake.error());
        }
        for (const AssetEntry& row : bake->rows)
        {
            putAsset(assets.catalog, row);
        }
        return modelId;
    }

    /// The `.smeta` sidecar for a foreign/headerless file (a raw `.png` dropped into assets/): the one
    /// place a file with no room in its own bytes can carry a stable id + colorspace. Engine-written
    /// files (`.smodel`, extracted `.smat`/`.smesh`) never get one — their identity is the bytes/name.
    struct SmetaData
    {
        Uuid id;
        AssetType type = AssetType::Texture;
        Colorspace colorspace = Colorspace::Auto;
        std::string folder;
        std::string name;
    };

    auto readSmeta(const std::string& path) -> Result<SmetaData>
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return Err(std::format("cannot open '{}'", path));
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto doc = parseJson(text);
        if (!doc || !doc->is_object())
        {
            return Err(std::format("'{}' is not a valid .smeta", path));
        }
        SmetaData meta;
        meta.id = Uuid{ jsonU64Or(*doc, "id", 0) };
        meta.type = assetTypeFromName(jsonStringOr(*doc, "type", std::string{ "texture" }));
        meta.colorspace = colorspaceFromName(jsonStringOr(*doc, "colorspace", std::string{ "auto" }));
        meta.folder = jsonStringOr(*doc, "folder", std::string{});
        meta.name = jsonStringOr(*doc, "name", std::string{});
        if (meta.id.value == 0)
        {
            return Err(std::format("'{}' has no id", path));
        }
        return meta;
    }

    auto writeSmeta(const std::string& path, const SmetaData& meta) -> Result<void>
    {
        nlohmann::json doc;
        doc["version"] = 1;
        doc["id"] = uuidToJson(meta.id.value);
        doc["type"] = assetTypeName(meta.type);
        doc["colorspace"] = colorspaceName(meta.colorspace);
        if (!meta.folder.empty())
        {
            doc["folder"] = meta.folder;
        }
        if (!meta.name.empty())
        {
            doc["name"] = meta.name;
        }
        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            return Err(std::format("cannot write '{}'", path));
        }
        out << dumpJson(doc, 2);
        if (!out)
        {
            return Err(std::format("write failed for '{}'", path));
        }
        return {};
    }

    // Walk assets/ (skipping .cache/), rebuild the catalog from disk, and diff it against the live one.
    // Containers contribute a Model row + sub-asset rows via the prefix read; engine-written standalone
    // files identify by their uuid filename stem; foreign/non-uuid files identify via a `.smeta` sidecar
    // (minted + written on first sight). Display names + folders are preserved from the prior catalog by
    // id. The caller owns GPU-cache invalidation (loadProject just cleared them; scan-assets idles +
    // clears before this).
    auto scanAssets(AssetServer& assets) -> Result<ScanDelta>
    {
        ScanDelta delta;
        if (assets.root.empty())
        {
            return delta;
        }
        const std::filesystem::path root(assets.root);
        std::error_code ec;
        if (!std::filesystem::exists(root, ec))
        {
            return delta;
        }

        const AssetCatalog previous = assets.catalog;
        AssetCatalog rebuilt;
        rebuilt.folders = previous.folders;

        auto preserveNameFolder = [&](AssetEntry& row)
        {
            if (const AssetEntry* prev = findAsset(previous, row.id); prev != nullptr)
            {
                row.name = prev->name;
                if (!prev->folder.empty())
                {
                    row.folder = prev->folder;
                }
            }
        };
        auto parseUuidStem = [](const std::filesystem::path& path, u64& out) -> bool
        {
            const std::string stem = path.stem().string();
            if (stem.empty())
            {
                return false;
            }
            char* endPointer = nullptr;
            out = std::strtoull(stem.c_str(), &endPointer, 10);
            return endPointer != nullptr && *endPointer == '\0';
        };

        for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
            {
                break;
            }
            if (it->is_directory(ec))
            {
                if (it->path().filename() == ".cache")
                {
                    it.disable_recursion_pending();
                }
                continue;
            }
            const std::filesystem::path path = it->path();
            std::string ext = path.extension().string();
            for (char& ch : ext)
            {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            const std::string rel = std::filesystem::relative(path, root, ec).generic_string();

            if (ext == ".smodel")
            {
                auto meta = readContainerMetadata(path.string());
                if (!meta)
                {
                    logWarn(std::format("scan: skipping '{}': {}", rel, meta.error()));
                    continue;
                }
                for (AssetEntry row : catalogRowsForModel(*meta, rel))
                {
                    preserveNameFolder(row);
                    putAsset(rebuilt, std::move(row));
                }
                continue;
            }

            AssetType type = AssetType::Other;
            bool recognized = false;
            bool hdr = false;
            if (ext == ".smesh")
            {
                type = AssetType::Mesh;
                recognized = true;
            }
            else if (ext == ".smat")
            {
                type = AssetType::Material;
                recognized = true;
            }
            else if (ext == ".sanim")
            {
                type = AssetType::Animation;
                recognized = true;
            }
            else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp")
            {
                type = AssetType::Texture;
                recognized = true;
            }
            else if (ext == ".hdr")
            {
                type = AssetType::Texture;
                recognized = true;
                hdr = true;
            }
            if (!recognized)
            {
                continue;
            }
            u64 id = 0;
            if (parseUuidStem(path, id))
            {
                // Engine-written standalone file (uuid name). A known one keeps its catalog row verbatim
                // (name/folder/duration/colorspace are not recoverable from the filename) — only its
                // path is refreshed. A genuinely new one infers type/hdr from the extension.
                if (const AssetEntry* prev = findAsset(previous, Uuid{ id }); prev != nullptr)
                {
                    AssetEntry row = *prev;
                    row.path = rel;
                    row.container = Uuid{ 0 };
                    row.chunk = -1;
                    putAsset(rebuilt, std::move(row));
                }
                else
                {
                    AssetEntry row;
                    row.id = Uuid{ id };
                    row.name = path.stem().string();
                    row.type = type;
                    row.path = rel;
                    row.hdr = hdr;
                    putAsset(rebuilt, std::move(row));
                }
                continue;
            }

            // A foreign / headerless file (a raw .png dropped in): identity + colorspace come from a
            // sibling .smeta, minted + written on first sight (a wrong-colorspace guess is warned).
            const std::string smetaPath = path.string() + ".smeta";
            SmetaData sidecar;
            bool haveSidecar = false;
            if (std::filesystem::exists(smetaPath))
            {
                if (auto loaded = readSmeta(smetaPath))
                {
                    sidecar = *loaded;
                    haveSidecar = true;
                }
                else
                {
                    logWarn(std::format("scan: ignoring bad .smeta '{}.smeta': {}", rel, loaded.error()));
                }
            }
            if (!haveSidecar)
            {
                sidecar.id = newUuid();
                sidecar.type = type;
                sidecar.colorspace = Colorspace::Srgb;
                if (hdr)
                {
                    sidecar.colorspace = Colorspace::Hdr;
                }
                sidecar.name = path.stem().string();
                if (auto wrote = writeSmeta(smetaPath, sidecar); !wrote)
                {
                    logWarn(std::format("scan: could not write '{}.smeta': {}", rel, wrote.error()));
                }
                logWarn(std::format("scan: minted .smeta for foreign file '{}' (colorspace {} — verify it for "
                                    "data maps like normals)",
                                    rel, colorspaceName(sidecar.colorspace)));
            }
            AssetEntry row;
            row.id = sidecar.id;
            row.name = path.stem().string();
            if (!sidecar.name.empty())
            {
                row.name = sidecar.name;
            }
            row.type = sidecar.type;
            row.path = rel;
            row.folder = sidecar.folder;
            row.colorspace = sidecar.colorspace;
            row.hdr = sidecar.colorspace == Colorspace::Hdr;
            row.linear = sidecar.colorspace == Colorspace::Linear;
            preserveNameFolder(row);
            putAsset(rebuilt, std::move(row));
        }

        for (const auto& [id, index] : rebuilt.byId)
        {
            if (!previous.byId.contains(id))
            {
                delta.added.push_back(rebuilt.entries[index]);
            }
        }
        for (const auto& [id, index] : previous.byId)
        {
            if (!rebuilt.byId.contains(id))
            {
                delta.removed.push_back(Uuid{ id });
            }
        }
        assets.catalog = std::move(rebuilt);
        return delta;
    }

    auto catalogCachePath(const AssetServer& assets) -> std::filesystem::path
    {
        return std::filesystem::path(assets.root) / ".cache" / "catalog.json";
    }

    // A cheap fingerprint of assets/: an FNV-1a fold over sorted (relpath, mtime, size) for every file
    // (including .smeta sidecars; excluding .cache/). Stat-only — no file contents read. Any add /
    // remove / touch / sidecar edit changes it, so it is a sound trigger for invalidating the cache.
    auto assetSignature(const std::string& root) -> u64
    {
        std::vector<std::string> entries;
        const std::filesystem::path base(root);
        std::error_code ec;
        if (!std::filesystem::exists(base, ec))
        {
            return 0;
        }
        for (auto it = std::filesystem::recursive_directory_iterator(base, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
            {
                break;
            }
            if (it->is_directory(ec))
            {
                if (it->path().filename() == ".cache")
                {
                    it.disable_recursion_pending();
                }
                continue;
            }
            const std::string rel = std::filesystem::relative(it->path(), base, ec).generic_string();
            const auto size = static_cast<u64>(it->file_size(ec));
            const auto mtime = static_cast<u64>(it->last_write_time(ec).time_since_epoch().count());
            entries.push_back(std::format("{}|{}|{}", rel, mtime, size));
        }
        std::ranges::sort(entries);
        u64 hash = 1469598103934665603ull;
        for (const std::string& entry : entries)
        {
            for (const char ch : entry)
            {
                hash = hash ^ static_cast<u8>(ch);
                hash = hash * 1099511628211ull;
            }
            hash = hash * 1099511628211ull;  // separator between entries
        }
        return hash;
    }

    // Persists the catalog + the current asset signature to assets/.cache/catalog.json. Regenerable and
    // gitignored: deleting it is always safe (the next load falls back to a cold scan).
    void writeCatalogCache(const AssetServer& assets)
    {
        const std::filesystem::path path = catalogCachePath(assets);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        nlohmann::json doc;
        doc["version"] = 1;
        doc["signature"] = std::to_string(assetSignature(assets.root));
        doc["assets"] = catalogToJson(assets.catalog);
        doc["assetFolders"] = catalogFoldersToJson(assets.catalog);
        std::ofstream out(path, std::ios::binary);
        if (out)
        {
            out << dumpJson(doc, 0);
        }
    }

    // Builds the catalog with the cache as a latency shortcut: if assets/.cache/catalog.json exists and
    // its stored signature still matches the on-disk signature, reuse its rows (skipping every .smodel
    // prefix read); on any mismatch, a missing or corrupt cache, fall back to a full scanAssets. The
    // cache is NEVER load-bearing — a cold scan always yields the identical catalog. Refreshes the cache.
    auto loadCatalog(AssetServer& assets) -> Result<ScanDelta>
    {
        const std::filesystem::path path = catalogCachePath(assets);
        std::error_code ec;
        if (std::filesystem::exists(path, ec))
        {
            std::ifstream in(path, std::ios::binary);
            if (in)
            {
                std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (auto doc = parseJson(text); doc && doc->is_object())
                {
                    const std::string cached = jsonStringOr(*doc, "signature", std::string{});
                    if (!cached.empty() && cached == std::to_string(assetSignature(assets.root)))
                    {
                        catalogFromJson(assets.catalog, doc->value("assets", nlohmann::json::array()));
                        catalogFoldersFromJson(assets.catalog, doc->value("assetFolders", nlohmann::json::array()));
                        return ScanDelta{};
                    }
                }
            }
        }
        auto delta = scanAssets(assets);
        if (!delta)
        {
            return Err(delta.error());
        }
        writeCatalogCache(assets);
        return delta;
    }

    // Default standalone destination (project-relative) for an extracted sub-asset, by type.
    auto defaultExtractDest(AssetType type, Uuid subId, std::string_view imageExt) -> std::string
    {
        const std::string id = std::to_string(subId.value);
        if (type == AssetType::Material)
        {
            return "materials/" + id + ".smat";
        }
        if (type == AssetType::Mesh)
        {
            return "models/" + id + ".smesh";
        }
        if (type == AssetType::Animation)
        {
            return "models/" + id + ".sanim";
        }
        std::string ext{ imageExt };
        if (ext.empty())
        {
            ext = "png";
        }
        return "textures/" + id + "." + ext;
    }

    // The image extension implied by a texture chunk's leading bytes (png/jpg/hdr), default png.
    auto imageExtFromBytes(std::span<const std::byte> bytes) -> std::string
    {
        auto at = [&](std::size_t i) { return static_cast<u8>(bytes[i]); };
        if (bytes.size() >= 8 && at(0) == 0x89 && at(1) == 0x50 && at(2) == 0x4E && at(3) == 0x47)
        {
            return "png";
        }
        if (bytes.size() >= 3 && at(0) == 0xFF && at(1) == 0xD8 && at(2) == 0xFF)
        {
            return "jpg";
        }
        if (bytes.size() >= 2 && at(0) == 0x23 && at(1) == 0x3F)  // "#?" — Radiance HDR
        {
            return "hdr";
        }
        return "png";
    }

    // Rewrites a container with a fresh META chunk, preserving every payload chunk verbatim. The
    // simplest correct way to grow/shrink the metadata (remap edits) without tracking payload offsets.
    auto rewriteContainerMeta(const std::string& fullPath, const ContainerReader& reader,
                              const ContainerMetadata& newMeta) -> Result<void>
    {
        std::vector<std::vector<std::byte>> owned;
        owned.push_back(encodeContainerMetadata(newMeta));
        for (const TocEntry& entry : reader.toc)
        {
            if (entry.fourcc == static_cast<u32>(ChunkKind::Meta))
            {
                continue;
            }
            auto bytes = reader.readChunk(entry);
            if (!bytes)
            {
                return Err(bytes.error());
            }
            owned.push_back(std::move(*bytes));
        }
        std::vector<ContainerChunk> chunks;
        chunks.push_back(ContainerChunk{ .kind = ChunkKind::Meta, .subId = 0, .flags = 0, .bytes = owned[0] });
        std::size_t ownedIndex = 1;
        for (const TocEntry& entry : reader.toc)
        {
            if (entry.fourcc == static_cast<u32>(ChunkKind::Meta))
            {
                continue;
            }
            chunks.push_back(ContainerChunk{ .kind = static_cast<ChunkKind>(entry.fourcc),
                                             .subId = entry.subId,
                                             .flags = entry.flags,
                                             .bytes = owned[ownedIndex] });
            ownedIndex = ownedIndex + 1;
        }
        return writeContainer(fullPath, chunks);
    }

    // Slices a sub-asset's chunk out of its container to a standalone file (keeping the same sub-id),
    // registers a standalone catalog row for it, and writes a remap entry so resolution prefers the
    // external file. The container's bytes are otherwise untouched; clearExtraction reverts. Returns the
    // standalone asset's id (== subId). dest is project-relative; "" picks the per-type default.
    auto extractSubAsset(AssetServer& assets, Uuid modelId, Uuid subId, const std::string& dest) -> Result<Uuid>
    {
        auto model = loadModelAsset(assets, modelId);
        if (!model)
        {
            return Err(std::format("model {} is not loadable", modelId.value));
        }
        const ContainerMetadata::SubAsset* sub = nullptr;
        for (const ContainerMetadata::SubAsset& candidate : model->meta.subAssets)
        {
            if (candidate.subId.value == subId.value)
            {
                sub = &candidate;
                break;
            }
        }
        if (sub == nullptr)
        {
            return Err(std::format("model {} has no sub-asset {}", modelId.value, subId.value));
        }
        const TocEntry* entry = nullptr;
        for (const TocEntry& candidate : model->reader.toc)
        {
            if (candidate.subId == subId.value && candidate.fourcc != static_cast<u32>(ChunkKind::Meta))
            {
                entry = &candidate;
                break;
            }
        }
        if (entry == nullptr)
        {
            return Err(std::format("model {} has no chunk for sub-asset {}", modelId.value, subId.value));
        }
        auto bytes = model->reader.readChunk(*entry);
        if (!bytes)
        {
            return Err(bytes.error());
        }

        std::string relativeDest = dest;
        if (relativeDest.empty())
        {
            std::string imageExt;
            if (sub->type == AssetType::Texture)
            {
                imageExt = imageExtFromBytes(*bytes);
            }
            relativeDest = defaultExtractDest(sub->type, subId, imageExt);
        }
        const std::string fullDest = assets.root + "/" + relativeDest;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(fullDest).parent_path(), ec);
        std::ofstream out(fullDest, std::ios::binary);
        if (!out)
        {
            return Err(std::format("cannot write '{}'", relativeDest));
        }
        out.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
        if (!out)
        {
            return Err(std::format("write failed for '{}'", relativeDest));
        }

        ContainerMetadata updated = model->meta;
        if (!updated.remap.is_object())
        {
            updated.remap = nlohmann::json::object();
        }
        updated.remap[std::to_string(subId.value)] = { { "external", relativeDest } };
        const std::string containerPath = assets.root + "/" + findAsset(assets.catalog, modelId)->path;
        if (auto wrote = rewriteContainerMeta(containerPath, model->reader, updated); !wrote)
        {
            return Err(wrote.error());
        }

        AssetEntry row;
        row.id = subId;
        row.name = sub->name;
        row.type = sub->type;
        row.path = relativeDest;
        row.colorspace = colorspaceFromName(sub->colorspace);
        row.duration = sub->duration;
        putAsset(assets.catalog, std::move(row));

        // Drop the stale reader (its TOC offsets shifted) and the sub-asset's GPU ref so the next
        // resolve reads the external file.
        assets.modelRefByUuid.erase(modelId.value);
        assets.meshRefByUuid.erase(subId.value);
        assets.textureRefByUuid.erase(subId.value);
        return subId;
    }

    // Drops a sub-asset's extraction: removes the remap entry, deletes the external file (so its uuid
    // name can never alias the embedded chunk on a later scan), reverts the catalog row to the embedded
    // chunk, and refreshes caches. The embedded chunk has been the dormant fallback all along.
    auto clearExtraction(AssetServer& assets, Uuid modelId, Uuid subId) -> Result<void>
    {
        auto model = loadModelAsset(assets, modelId);
        if (!model)
        {
            return Err(std::format("model {} is not loadable", modelId.value));
        }
        const std::string key = std::to_string(subId.value);
        std::string external;
        if (model->meta.remap.is_object() && model->meta.remap.contains(key))
        {
            const nlohmann::json& entry = model->meta.remap.at(key);
            if (entry.is_object() && entry.contains("external") && entry.at("external").is_string())
            {
                external = entry.at("external").get<std::string>();
            }
        }

        ContainerMetadata updated = model->meta;
        if (updated.remap.is_object())
        {
            updated.remap.erase(key);
        }
        const std::string containerPath = assets.root + "/" + findAsset(assets.catalog, modelId)->path;
        if (auto wrote = rewriteContainerMeta(containerPath, model->reader, updated); !wrote)
        {
            return Err(wrote.error());
        }
        if (!external.empty())
        {
            std::error_code ec;
            std::filesystem::remove(assets.root + "/" + external, ec);
        }

        // Revert the catalog row to the embedded sub-asset (container + chunk).
        for (const ContainerMetadata::SubAsset& sub : model->meta.subAssets)
        {
            if (sub.subId.value == subId.value)
            {
                AssetEntry row;
                row.id = subId;
                row.name = sub.name;
                row.type = sub.type;
                row.path = findAsset(assets.catalog, modelId)->path;
                row.container = modelId;
                row.chunk = static_cast<i32>(sub.chunk);
                row.colorspace = colorspaceFromName(sub.colorspace);
                row.duration = sub.duration;
                putAsset(assets.catalog, std::move(row));
                break;
            }
        }
        assets.modelRefByUuid.erase(modelId.value);
        assets.meshRefByUuid.erase(subId.value);
        assets.textureRefByUuid.erase(subId.value);
        return {};
    }

    /// What a reimport changed, diffed by stable sub-id. `skipped` is true when the source bytes +
    /// importer version are unchanged (the content-addressed fast path). `removedFromSource` lists
    /// sub-assets the source no longer produces — kept + reported (cleanup decides their fate), never
    /// silently dropped.
    struct ReimportDelta
    {
        std::vector<Uuid> updated;
        std::vector<Uuid> added;
        std::vector<Uuid> removedFromSource;
        bool skipped = false;
    };

    // Re-bakes a container from its stored source + options when the source bytes changed (else a
    // content-addressed skip). Sub-ids are stable (subIdFor by source name), so the diff matches; an
    // extracted (remapped) sub-asset's external override is preserved — the freshly baked chunk is the
    // dormant fallback, never clobbering the user's edit. Live instances resolve by (modelId, subId), so
    // they pick up the new bytes with no re-instantiation. Caller idles the GPU; this drops sub-id caches.
    auto reimportModel(AssetServer& assets, Uuid modelId) -> Result<ReimportDelta>
    {
        ReimportDelta delta;
        auto model = loadModelAsset(assets, modelId);
        if (!model)
        {
            return Err(std::format("model {} is not loadable", modelId.value));
        }
        const ContainerMetadata oldMeta = model->meta;
        const std::string source = oldMeta.import.sourcePath;
        const std::string currentHash = hashFileFnv(source);
        if (currentHash.empty())
        {
            return Err(std::format("source '{}' is unreadable", source));
        }
        if (currentHash == oldMeta.import.sourceHash && oldMeta.import.importerVersion == ImporterVersion)
        {
            delta.skipped = true;
            return delta;
        }

        std::unordered_set<u64> oldSubs;
        for (const ContainerMetadata::SubAsset& sub : oldMeta.subAssets)
        {
            oldSubs.insert(sub.subId.value);
        }

        auto graph = translateModel(source);
        if (!graph)
        {
            return Err(graph.error());
        }
        auto bake = bakeModel(assets, *graph, ImportOptions::fromJson(oldMeta.import.options), source, modelId);
        if (!bake)
        {
            return Err(bake.error());
        }

        const std::string containerPath = assets.root + "/" + bake->path;
        auto newMeta = readContainerMetadata(containerPath);
        if (!newMeta)
        {
            return Err(newMeta.error());
        }
        std::unordered_set<u64> newSubs;
        for (const ContainerMetadata::SubAsset& sub : newMeta->subAssets)
        {
            newSubs.insert(sub.subId.value);
        }

        // Preserve the remap for sub-assets that still exist (the extracted edit survives the reimport).
        nlohmann::json keptRemap = nlohmann::json::object();
        if (oldMeta.remap.is_object())
        {
            for (const auto& [key, value] : oldMeta.remap.items())
            {
                const u64 sid = std::strtoull(key.c_str(), nullptr, 10);
                if (newSubs.contains(sid))
                {
                    keptRemap[key] = value;
                }
            }
        }
        if (!keptRemap.empty())
        {
            newMeta->remap = keptRemap;
            auto reader = readContainer(containerPath);
            if (reader)
            {
                if (auto wrote = rewriteContainerMeta(containerPath, *reader, *newMeta); !wrote)
                {
                    logWarn(std::format("reimport: could not preserve remap for model {}: {}", modelId.value,
                                        wrote.error()));
                }
            }
        }

        for (const u64 sid : newSubs)
        {
            if (oldSubs.contains(sid))
            {
                delta.updated.push_back(Uuid{ sid });
            }
            else
            {
                delta.added.push_back(Uuid{ sid });
            }
        }
        for (const u64 sid : oldSubs)
        {
            if (!newSubs.contains(sid))
            {
                delta.removedFromSource.push_back(Uuid{ sid });
            }
        }

        // Refresh the catalog rows (remap-aware) and drop the stale reader + every affected sub-id's GPU
        // ref so live instances re-resolve the new bytes without re-instantiation.
        auto finalMeta = readContainerMetadata(containerPath);
        if (finalMeta)
        {
            for (const AssetEntry& row : catalogRowsForModel(*finalMeta, bake->path))
            {
                putAsset(assets.catalog, row);
            }
        }
        assets.modelRefByUuid.erase(modelId.value);
        for (const u64 sid : newSubs)
        {
            assets.meshRefByUuid.erase(sid);
            assets.textureRefByUuid.erase(sid);
        }
        for (const u64 sid : oldSubs)
        {
            assets.meshRefByUuid.erase(sid);
            assets.textureRefByUuid.erase(sid);
        }
        return delta;
    }

    /// One node in the asset dependency graph: an asset and its on-disk byte cost.
    struct RefNode
    {
        Uuid id;
        AssetType type = AssetType::Other;
        Uuid container;
        u64 bytes = 0;
    };

    /// One directed edge: `from` references `to`. `from` may be an entity uuid (EntityAsset) rather
    /// than a catalog asset.
    struct RefEdge
    {
        Uuid from;
        Uuid to;
        enum class Kind : u8
        {
            ContainerChild,
            MaterialTexture,
            EntityAsset
        } kind = Kind::ContainerChild;
    };

    /// The scene → asset → sub-asset reference graph. Read-only/diagnostic (UE's Reference Viewer +
    /// Size Map): who-references-this, what-this-references, and a byte footprint. Rebuilt on demand.
    struct DependencyGraph
    {
        std::vector<RefNode> nodes;
        std::vector<RefEdge> edges;

        auto referencedBy(Uuid id) const -> std::vector<Uuid>
        {
            std::vector<Uuid> out;
            for (const RefEdge& edge : edges)
            {
                if (edge.to.value == id.value)
                {
                    out.push_back(edge.from);
                }
            }
            return out;
        }
        auto referencesOf(Uuid id) const -> std::vector<Uuid>
        {
            std::vector<Uuid> out;
            for (const RefEdge& edge : edges)
            {
                if (edge.from.value == id.value)
                {
                    out.push_back(edge.to);
                }
            }
            return out;
        }
        auto bytesOf(Uuid id) const -> u64
        {
            for (const RefNode& node : nodes)
            {
                if (node.id.value == id.value)
                {
                    return node.bytes;
                }
            }
            return 0;
        }
        // The on-disk footprint: a container's `.smodel` size already counts its embedded sub-assets,
        // so the honest footprint is just the node's own bytes (no double-counting within a container).
        auto footprint(Uuid id) const -> u64
        {
            return bytesOf(id);
        }
    };

    // The on-disk bytes of a catalog row: a model / standalone file's size, or an embedded sub-asset's
    // chunk length (read from its container's TOC).
    auto assetBytes(AssetServer& assets, const AssetEntry& entry) -> u64
    {
        std::error_code ec;
        if (entry.container.value == 0)
        {
            return static_cast<u64>(std::filesystem::file_size(assets.root + "/" + entry.path, ec));
        }
        auto model = loadModelAsset(assets, entry.container);
        if (!model)
        {
            return 0;
        }
        ChunkKind kind = ChunkKind::Texture;
        if (entry.type == AssetType::Mesh)
        {
            kind = ChunkKind::Mesh;
        }
        else if (entry.type == AssetType::Material)
        {
            kind = ChunkKind::Material;
        }
        else if (entry.type == AssetType::Animation)
        {
            kind = ChunkKind::Animation;
        }
        const TocEntry* toc = model->reader.find(kind, entry.id.value);
        if (toc == nullptr)
        {
            return 0;
        }
        return toc->length;
    }

    // Builds the dependency graph: catalog assets as nodes; container→child, material→texture, and
    // scene-entity→asset edges. A snapshot — rebuilt on demand, never cached stale.
    auto buildDependencyGraph(Scene& scene, const AssetCatalog& catalog, AssetServer& assets) -> DependencyGraph
    {
        DependencyGraph graph;
        for (const AssetEntry& entry : catalog.entries)
        {
            graph.nodes.push_back(RefNode{
                .id = entry.id, .type = entry.type, .container = entry.container, .bytes = assetBytes(assets, entry) });
        }
        for (const AssetEntry& entry : catalog.entries)
        {
            if (entry.type == AssetType::Model)
            {
                for (const AssetEntry& child : catalog.entries)
                {
                    if (child.container.value == entry.id.value)
                    {
                        graph.edges.push_back(
                            RefEdge{ .from = entry.id, .to = child.id, .kind = RefEdge::Kind::ContainerChild });
                    }
                }
            }
            if (entry.type == AssetType::Material)
            {
                MaterialAsset material;
                bool resolved = false;
                if (entry.container.value != 0)
                {
                    if (auto loaded = resolveMaterial(assets, entry.container, entry.id))
                    {
                        material = *loaded;
                        resolved = true;
                    }
                }
                else if (auto loaded = loadMaterialAsset(assets, entry.id))
                {
                    material = *loaded;
                    resolved = true;
                }
                if (resolved)
                {
                    for (const Uuid texture : { material.albedoTexture, material.ormTexture, material.normalTexture,
                                                material.emissiveTexture, material.heightTexture })
                    {
                        if (texture.value != 0)
                        {
                            graph.edges.push_back(
                                RefEdge{ .from = entry.id, .to = texture, .kind = RefEdge::Kind::MaterialTexture });
                        }
                    }
                }
            }
        }
        auto entityEdge = [&](Entity entity, Uuid asset)
        {
            if (asset.value == 0)
            {
                return;
            }
            graph.edges.push_back(RefEdge{
                .from = getComponent<IdComponent>(scene, entity).id, .to = asset, .kind = RefEdge::Kind::EntityAsset });
        };
        forEach<MeshComponent>(scene, [&](Entity entity, MeshComponent& mesh) { entityEdge(entity, mesh.mesh); });
        forEach<SkinnedMeshComponent>(scene, [&](Entity entity, SkinnedMeshComponent& skin)
                                      { entityEdge(entity, skin.mesh); });
        forEach<MaterialAssetComponent>(scene, [&](Entity entity, MaterialAssetComponent& material)
                                        { entityEdge(entity, material.material); });
        forEach<ModelInstanceComponent>(scene, [&](Entity entity, ModelInstanceComponent& instance)
                                        { entityEdge(entity, instance.modelId); });
        forEach<MaterialSetComponent>(scene,
                                      [&](Entity entity, MaterialSetComponent& set)
                                      {
                                          for (const MaterialSlot& slot : set.slots)
                                          {
                                              entityEdge(entity, slot.albedoTexture);
                                              entityEdge(entity, slot.metallicRoughnessTexture);
                                              entityEdge(entity, slot.normalTexture);
                                              entityEdge(entity, slot.occlusionTexture);
                                              entityEdge(entity, slot.emissiveTexture);
                                          }
                                      });
        return graph;
    }

    /// How a cleanup candidate is classified. Reported separately (the Unity Broken/Missing/Unused
    /// split); only `Unused` is ever auto-deletable, and even then only after explicit confirm.
    enum class CleanCategory : u8
    {
        Unused,
        OrphanedFile,
        BrokenReference,
        IndirectReview
    };

    auto cleanCategoryName(CleanCategory category) -> const char*
    {
        if (category == CleanCategory::OrphanedFile)
        {
            return "orphaned";
        }
        if (category == CleanCategory::BrokenReference)
        {
            return "broken";
        }
        if (category == CleanCategory::IndirectReview)
        {
            return "review";
        }
        return "unused";
    }

    struct CleanCandidate
    {
        Uuid id;
        std::string path;
        CleanCategory category = CleanCategory::Unused;
        u64 bytes = 0;
        std::string reason;
    };

    struct CleanReportData
    {
        std::vector<CleanCandidate> candidates;
        u64 reclaimableBytes = 0;
    };

    // Every catalog-id string referenced (recursively) by a ScriptComponent override field. These are
    // invisible to the static dependency graph, so an asset only reachable this way is review, not unused.
    auto collectScriptReferencedIds(Scene& scene) -> std::unordered_set<u64>
    {
        std::unordered_set<u64> referenced;
        std::function<void(const nlohmann::json&)> walk = [&](const nlohmann::json& value)
        {
            if (value.is_string())
            {
                const std::string text = value.get<std::string>();
                char* endPointer = nullptr;
                const u64 id = std::strtoull(text.c_str(), &endPointer, 10);
                if (id != 0 && endPointer != nullptr && *endPointer == '\0')
                {
                    referenced.insert(id);
                }
            }
            else if (value.is_object())
            {
                for (const auto& [key, child] : value.items())
                {
                    walk(child);
                }
            }
            else if (value.is_array())
            {
                for (const nlohmann::json& child : value)
                {
                    walk(child);
                }
            }
        };
        forEach<ScriptComponent>(scene,
                                 [&](Entity, ScriptComponent& component)
                                 {
                                     for (const ScriptSlot& slot : component.scripts)
                                     {
                                         walk(slot.overrides);
                                     }
                                 });
        return referenced;
    }

    // Classifies every catalog asset as kept or a cleanup candidate, by reachability from roots (the
    // active scene's asset refs + `exclude`). Unused = unreachable + not script-referenced; a
    // script-referenced unreachable asset is IndirectReview (never auto-deleted); a scene/material edge
    // to a missing id is a BrokenReference. Read-only — produces a report, deletes nothing.
    auto analyzeClean(Scene& scene, const AssetCatalog& catalog, AssetServer& assets, std::span<const Uuid> exclude)
        -> CleanReportData
    {
        CleanReportData report;
        DependencyGraph graph = buildDependencyGraph(scene, catalog, assets);

        std::unordered_set<u64> reachable;
        for (const RefEdge& edge : graph.edges)
        {
            if (edge.kind == RefEdge::Kind::EntityAsset)
            {
                reachable.insert(edge.to.value);
            }
        }
        for (const Uuid id : exclude)
        {
            reachable.insert(id.value);
        }
        std::vector<u64> work(reachable.begin(), reachable.end());
        while (!work.empty())
        {
            const u64 id = work.back();
            work.pop_back();
            for (const Uuid target : graph.referencesOf(Uuid{ id }))
            {
                if (reachable.insert(target.value).second)
                {
                    work.push_back(target.value);
                }
            }
        }
        // A container and its embedded sub-assets are one deletable unit: keeping any one keeps all.
        std::unordered_set<u64> keptContainers;
        for (const AssetEntry& entry : catalog.entries)
        {
            if (reachable.contains(entry.id.value))
            {
                if (entry.type == AssetType::Model)
                {
                    keptContainers.insert(entry.id.value);
                }
                if (entry.container.value != 0)
                {
                    keptContainers.insert(entry.container.value);
                }
            }
        }
        for (const AssetEntry& entry : catalog.entries)
        {
            if (keptContainers.contains(entry.id.value) ||
                (entry.container.value != 0 && keptContainers.contains(entry.container.value)))
            {
                reachable.insert(entry.id.value);
            }
        }

        const std::unordered_set<u64> scriptRefs = collectScriptReferencedIds(scene);

        for (const RefEdge& edge : graph.edges)
        {
            if (edge.kind == RefEdge::Kind::ContainerChild)
            {
                continue;
            }
            if (!catalog.byId.contains(edge.to.value))
            {
                report.candidates.push_back(CleanCandidate{ .id = edge.to,
                                                            .category = CleanCategory::BrokenReference,
                                                            .reason = std::format("referenced by {} but not in the "
                                                                                  "catalog",
                                                                                  edge.from.value) });
            }
        }

        for (const AssetEntry& entry : catalog.entries)
        {
            if (reachable.contains(entry.id.value) || entry.container.value != 0)
            {
                continue;  // kept, or an embedded sub-asset (the container is the deletable unit)
            }
            CleanCandidate candidate;
            candidate.id = entry.id;
            candidate.path = entry.path;
            candidate.bytes = graph.bytesOf(entry.id);
            if (scriptRefs.contains(entry.id.value))
            {
                candidate.category = CleanCategory::IndirectReview;
                candidate.reason = "referenced only by a script field — review before deleting";
            }
            else
            {
                candidate.category = CleanCategory::Unused;
                candidate.reason = "not reachable from the active scene";
                report.reclaimableBytes = report.reclaimableBytes + candidate.bytes;
            }
            report.candidates.push_back(std::move(candidate));
        }
        return report;
    }

    struct DeleteUnusedData
    {
        i32 deleted = 0;
        u64 reclaimedBytes = 0;
    };

    // Deletes only the listed ids that analyzeClean classifies as Unused (refusing without confirm),
    // then rescans so any newly-orphaned cascade resurfaces. Outward-facing + irreversible: every
    // deletion is logged. The caller idles the GPU + clears caches before calling.
    auto deleteUnused(AssetServer& assets, Scene& scene, std::span<const Uuid> ids, bool confirm)
        -> Result<DeleteUnusedData>
    {
        if (!confirm)
        {
            return Err(std::string{ "delete-unused requires confirm=true" });
        }
        const CleanReportData report = analyzeClean(scene, assets.catalog, assets, {});
        std::unordered_set<u64> deletable;
        for (const CleanCandidate& candidate : report.candidates)
        {
            if (candidate.category == CleanCategory::Unused)
            {
                deletable.insert(candidate.id.value);
            }
        }
        DeleteUnusedData result;
        for (const Uuid id : ids)
        {
            if (!deletable.contains(id.value))
            {
                logWarn(std::format("delete-unused: refusing {} (not classified Unused)", id.value));
                continue;
            }
            const AssetEntry* entry = findAsset(assets.catalog, id);
            if (entry == nullptr)
            {
                continue;
            }
            std::error_code ec;
            const std::string full = assets.root + "/" + entry->path;
            const auto bytes = static_cast<u64>(std::filesystem::file_size(full, ec));
            std::filesystem::remove(full, ec);
            std::filesystem::remove(full + ".smeta", ec);  // foreign-file sidecar, if any
            result.deleted = result.deleted + 1;
            result.reclaimedBytes = result.reclaimedBytes + bytes;
            logInfo(std::format("delete-unused: removed '{}' ({} bytes)", entry->path, bytes));
        }
        static_cast<void>(scanAssets(assets));  // rebuild the catalog + surface any cascade
        writeCatalogCache(assets);
        return result;
    }

    // Headless check: bake a synthetic skinned multi-material graph into a .smodel, prefix-read it
    // back (sub-asset count, materials, nodes/skin), and confirm the embedded MESH chunk is a valid
    // standalone .smesh image (loadMesh round-trips its counts). No GPU, no spawn.
    void runBakeModelSelfTest()
    {
        Mesh mesh;
        mesh.vertices.resize(4);
        for (std::size_t i = 0; i < mesh.vertices.size(); i = i + 1)
        {
            mesh.vertices[i].position = glm::vec3(static_cast<f32>(i), 0.0f, 0.0f);
        }
        mesh.indices = { 0, 1, 2, 0, 2, 3 };
        mesh.submeshes = { Submesh{ .firstIndex = 0, .indexCount = 3, .vertexOffset = 0, .materialSlot = 0 },
                           Submesh{ .firstIndex = 3, .indexCount = 3, .vertexOffset = 0, .materialSlot = 1 } };

        ImportedModel graph;
        graph.mesh = mesh;
        graph.hasSkin = true;
        graph.skin.resize(4);
        graph.nodes = { ImportedNode{ .name = "root", .parent = -1 }, ImportedNode{ .name = "joint", .parent = 0 } };
        graph.skinDesc.joints = { 0, 1 };
        graph.skinDesc.inverseBind = { glm::mat4(1.0f), glm::mat4(1.0f) };
        graph.skinDesc.skeletonRoot = 0;
        graph.skinDesc.meshNode = 1;

        ImportedMaterial stone;
        stone.name = "stone";
        stone.hasAlbedo = true;
        stone.albedoBytes = { 1, 2, 3, 4 };
        stone.albedoExt = "png";
        stone.hasNormal = true;
        stone.normalBytes = { 5, 6, 7 };
        stone.normalExt = "png";
        ImportedMaterial metal;
        metal.name = "metal";
        metal.hasMetallicRoughness = true;
        metal.metallicRoughnessBytes = { 8, 9 };
        metal.metallicRoughnessExt = "png";
        graph.materials = { stone, metal };

        AnimClip clip;
        clip.name = "idle";
        clip.duration = 1.0f;
        clip.tracks.push_back(AnimTrack{ .joint = 1, .jointName = "joint" });
        graph.animations = { clip };

        AssetServer testAssets;
        testAssets.root = "/tmp/saffron_bake_test";
        auto bake = bakeModel(testAssets, graph, ImportOptions{}, "/tmp/town.glb", Uuid{ 0 });
        if (!bake)
        {
            logError(std::format("bake self-test: bakeModel failed: {}", bake.error()));
            return;
        }

        const std::string fullPath = testAssets.root + "/" + bake->path;
        auto meta = readContainerMetadata(fullPath);
        if (!meta)
        {
            logError(std::format("bake self-test: prefix read failed: {}", meta.error()));
            return;
        }
        // 1 mesh + 2 materials + 3 textures + 1 animation = 7 sub-assets; 8 catalog rows (+ the Model).
        bool ok = meta->subAssets.size() == 7 && meta->materials.size() == 2 && meta->nodes.is_array() &&
                  meta->nodes.size() == 2 && !meta->skin.is_null() && bake->rows.size() == 8 &&
                  bake->rows.front().type == AssetType::Model;

        auto reader = readContainer(fullPath);
        if (!reader)
        {
            logError(std::format("bake self-test: readContainer failed: {}", reader.error()));
            return;
        }
        const Uuid meshSubId = subIdFor("town", "mesh", "0", 0);
        const TocEntry* meshEntry = reader->find(ChunkKind::Mesh, meshSubId.value);
        ok = ok && meshEntry != nullptr;
        if (meshEntry != nullptr)
        {
            auto meshBytes = reader->readChunk(*meshEntry);
            if (meshBytes)
            {
                const std::string tempMesh = "/tmp/saffron_bake_mesh.smesh";
                if (std::ofstream out(tempMesh, std::ios::binary); out)
                {
                    out.write(reinterpret_cast<const char*>(meshBytes->data()),
                              static_cast<std::streamsize>(meshBytes->size()));
                }
                auto loaded = loadMesh(tempMesh);
                ok = ok && loaded && loaded->vertices.size() == 4 && loaded->indices.size() == 6 &&
                     loaded->submeshes.size() == 2;
            }
            else
            {
                ok = false;
            }
        }

        if (ok)
        {
            logInfo(".smodel bake round-trip OK (one container, embedded materials, mesh chunk loads)");
        }
        else
        {
            logError(".smodel bake round-trip MISMATCH");
        }
    }

    // Headless check (GPU-free): bake a container, open it via loadModelAsset, slice the mesh chunk
    // through ByteSource + loadMeshFromBytes, parse a material via resolveMaterial, and confirm a
    // remap pointing at a missing file falls back to the embedded chunk. The GPU half of resolveMesh/
    // resolveTexture (the upload) is exercised by the phase 07/08 instantiate + render e2e.
    void runChunkLoaderSelfTest()
    {
        Mesh mesh;
        mesh.vertices.resize(3);
        mesh.indices = { 0, 1, 2 };
        mesh.submeshes = { Submesh{ .firstIndex = 0, .indexCount = 3, .vertexOffset = 0, .materialSlot = 0 } };
        ImportedModel graph;
        graph.mesh = mesh;
        ImportedMaterial wood;
        wood.name = "wood";
        wood.baseColor = glm::vec4(0.25f, 0.5f, 0.75f, 1.0f);
        wood.metallic = 0.3f;
        wood.roughness = 0.6f;
        wood.hasAlbedo = true;
        wood.albedoBytes = { 10, 20, 30, 40, 50 };
        wood.albedoExt = "png";
        graph.materials = { wood };

        AssetServer testAssets;
        testAssets.root = "/tmp/saffron_chunk_test";
        auto bake = bakeModel(testAssets, graph, ImportOptions{}, "/tmp/crate.glb", Uuid{ 0 });
        if (!bake)
        {
            logError(std::format("chunk-loader self-test: bake failed: {}", bake.error()));
            return;
        }
        for (const AssetEntry& row : bake->rows)
        {
            putAsset(testAssets.catalog, row);
        }

        auto model = loadModelAsset(testAssets, bake->modelId);
        if (!model)
        {
            logError("chunk-loader self-test: loadModelAsset returned null");
            return;
        }

        const Uuid meshSubId = subIdFor("crate", "mesh", "0", 0);
        const ByteSource meshSource = chunkSourceFor(testAssets, *model, ChunkKind::Mesh, meshSubId);
        bool ok = meshSource.path == (testAssets.root + "/" + bake->path) && meshSource.length != 0;
        if (auto bytes = meshSource.read(); bytes)
        {
            auto sliced = loadMeshFromBytes(std::span<const std::byte>{ *bytes });
            ok = ok && sliced && sliced->vertices.size() == 3 && sliced->indices.size() == 3;
        }
        else
        {
            ok = false;
        }

        const Uuid materialSubId = subIdFor("crate", "material", "wood", 0);
        auto material = resolveMaterial(testAssets, bake->modelId, materialSubId);
        ok = ok && material && material->metallic > 0.29f && material->metallic < 0.31f &&
             material->albedoTexture.value == subIdFor("crate", "texture", "0_albedo", 0).value;

        // A remap whose external file is missing must fall back to the embedded chunk (with a warning).
        model->meta.remap = nlohmann::json::object();
        model->meta.remap[std::to_string(meshSubId.value)] = { { "external", "models/missing-extracted.smesh" } };
        const ByteSource fallback = chunkSourceFor(testAssets, *model, ChunkKind::Mesh, meshSubId);
        ok = ok && fallback.path == (testAssets.root + "/" + bake->path) && fallback.length != 0;

        if (ok)
        {
            logInfo(".smodel chunk-slice loaders OK (slice load, resolveMaterial, remap fallback)");
        }
        else
        {
            logError(".smodel chunk-slice loaders MISMATCH");
        }
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
        // An embedded sub-asset resolves through its container's chunk slice.
        if (entry->container.value != 0)
        {
            return resolveMesh(assets, renderer, entry->container, id);
        }
        std::string fullPath = assets.root + "/" + entry->path;
        if (!std::filesystem::exists(fullPath) && entry->path.starts_with("meshes/"))
        {
            fullPath = assets.root + "/models/" + entry->path.substr(std::string{ "meshes/" }.size());
        }
        return loadMeshFromSource(assets, renderer, id, ByteSource{ .path = fullPath });
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
    void applyImportedMaterials(Scene& scene, Entity entity, const ModelSpawnInput& result)
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
    auto spawnSkinnedModel(Scene& scene, std::string name, const ModelSpawnInput& result) -> Entity
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

    auto spawnModel(Scene& scene, std::string name, const ModelSpawnInput& result) -> Entity
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

    // Expands a `.smodel` container's stored hierarchy into the scene by reconstructing the spawn
    // input from the MetadataChunk (mesh/material/animation sub-ids, node forest, skin) and reusing
    // spawnModel/spawnSkinnedModel. Holds SOFT references: components store sub-ids resolved at draw
    // time through the container (phase 06), so reimport/extract changes flow through. No GPU upload.
    // One asset instantiates into many independent entity trees (or never). Returns the root entity.
    auto instantiateModel(Scene& scene, AssetServer& assets, Uuid modelId, std::string_view name) -> Result<Entity>
    {
        auto model = loadModelAsset(assets, modelId);
        if (!model)
        {
            return Err(std::format("model {} is not loadable", modelId.value));
        }
        const ContainerMetadata& meta = model->meta;

        ModelSpawnInput result;
        for (const ContainerMetadata::SubAsset& sub : meta.subAssets)
        {
            if (sub.type == AssetType::Mesh)
            {
                result.mesh = sub.subId;
                break;
            }
        }
        for (const ContainerMetadata::SubAsset& sub : meta.subAssets)
        {
            if (sub.type != AssetType::Material)
            {
                continue;
            }
            MaterialSlot slot;
            if (auto material = resolveMaterial(assets, modelId, sub.subId))
            {
                slot.baseColor = material->baseColor;
                slot.metallic = material->metallic;
                slot.roughness = material->roughness;
                slot.emissive = material->emissive;
                slot.emissiveStrength = material->emissiveStrength;
                slot.albedoTexture = material->albedoTexture;
                slot.metallicRoughnessTexture = material->ormTexture;
                slot.normalTexture = material->normalTexture;
                slot.emissiveTexture = material->emissiveTexture;
                slot.heightTexture = material->heightTexture;
                slot.normalStrength = material->normalStrength;
                slot.uvTiling = material->uvTiling;
                slot.uvOffset = material->uvOffset;
                slot.heightScale = material->heightScale;
                slot.unlit = material->unlit;
                slot.alphaClip = material->blend == "masked";
                slot.alphaCutoff = material->alphaCutoff;
            }
            else
            {
                logWarn(std::format("model {}: material {} unresolved: {}", modelId.value, sub.subId.value,
                                    material.error()));
            }
            result.materials.push_back(slot);
        }
        for (const ContainerMetadata::SubAsset& sub : meta.subAssets)
        {
            if (sub.type == AssetType::Animation)
            {
                result.animations.push_back(sub.subId);
            }
        }
        result.nodes = importedNodesFromJson(meta.nodes);
        if (!meta.skin.is_null())
        {
            result.skinDesc = importedSkinFromJson(meta.skin);
            result.hasSkin = !result.skinDesc.joints.empty();
        }
        if (!result.materials.empty())
        {
            result.baseColor = result.materials.front().baseColor;
            result.albedoTexture = result.materials.front().albedoTexture;
        }

        Entity root = spawnModel(scene, std::string(name), result);
        addComponent<ModelInstanceComponent>(scene, root).modelId = modelId;
        return root;
    }

    // Headless check (GPU-free): bake an unskinned and a skinned container, instantiate each twice
    // into a scene, and assert two independent entity trees per model with the right mesh sub-id,
    // material slots, skinning, and a ModelInstanceComponent marking each root.
    void runInstantiateSelfTest()
    {
        AssetServer testAssets;
        testAssets.root = "/tmp/saffron_inst_test";

        ImportedModel flat;
        flat.mesh.vertices.resize(3);
        flat.mesh.indices = { 0, 1, 2 };
        flat.mesh.submeshes = { Submesh{ .firstIndex = 0, .indexCount = 3, .vertexOffset = 0, .materialSlot = 0 } };
        ImportedMaterial paint;
        paint.name = "paint";
        paint.baseColor = glm::vec4(0.2f, 0.4f, 0.6f, 1.0f);
        flat.materials = { paint };
        auto flatBake = bakeModel(testAssets, flat, ImportOptions{}, "/tmp/paintbox.glb", Uuid{ 0 });

        ImportedModel rigged;
        rigged.mesh.vertices.resize(4);
        rigged.mesh.indices = { 0, 1, 2, 0, 2, 3 };
        rigged.mesh.submeshes = { Submesh{ .firstIndex = 0, .indexCount = 6, .vertexOffset = 0, .materialSlot = 0 } };
        rigged.hasSkin = true;
        rigged.skin.resize(4);
        rigged.nodes = { ImportedNode{ .name = "root", .parent = -1 }, ImportedNode{ .name = "joint", .parent = 0 } };
        rigged.skinDesc.joints = { 0, 1 };
        rigged.skinDesc.inverseBind = { glm::mat4(1.0f), glm::mat4(1.0f) };
        rigged.skinDesc.skeletonRoot = 0;
        rigged.skinDesc.meshNode = 1;
        ImportedMaterial skin;
        skin.name = "skin";
        rigged.materials = { skin };
        auto riggedBake = bakeModel(testAssets, rigged, ImportOptions{}, "/tmp/dummy.glb", Uuid{ 0 });

        if (!flatBake || !riggedBake)
        {
            logError("instantiate self-test: bake failed");
            return;
        }
        for (const AssetEntry& row : flatBake->rows)
        {
            putAsset(testAssets.catalog, row);
        }
        for (const AssetEntry& row : riggedBake->rows)
        {
            putAsset(testAssets.catalog, row);
        }

        Scene scene;
        auto flatA = instantiateModel(scene, testAssets, flatBake->modelId, "Paint A");
        auto flatB = instantiateModel(scene, testAssets, flatBake->modelId, "Paint B");
        auto riggedA = instantiateModel(scene, testAssets, riggedBake->modelId, "Rig A");
        auto riggedB = instantiateModel(scene, testAssets, riggedBake->modelId, "Rig B");
        if (!flatA || !flatB || !riggedA || !riggedB)
        {
            logError("instantiate self-test: instantiateModel failed");
            return;
        }

        const Uuid flatMeshSubId = subIdFor("paintbox", "mesh", "0", 0);
        bool ok =
            getComponent<IdComponent>(scene, *flatA).id.value != getComponent<IdComponent>(scene, *flatB).id.value &&
            getComponent<IdComponent>(scene, *riggedA).id.value != getComponent<IdComponent>(scene, *riggedB).id.value;
        ok = ok && hasComponent<MeshComponent>(scene, *flatA) &&
             getComponent<MeshComponent>(scene, *flatA).mesh.value == flatMeshSubId.value &&
             hasComponent<MaterialComponent>(scene, *flatA) && hasComponent<ModelInstanceComponent>(scene, *flatA) &&
             getComponent<ModelInstanceComponent>(scene, *flatA).modelId.value == flatBake->modelId.value;
        ok = ok && hasComponent<SkinnedMeshComponent>(scene, *riggedA) &&
             getComponent<SkinnedMeshComponent>(scene, *riggedA).bones.size() == 2 &&
             hasComponent<ModelInstanceComponent>(scene, *riggedA) &&
             getComponent<ModelInstanceComponent>(scene, *riggedB).modelId.value == riggedBake->modelId.value;

        if (ok)
        {
            logInfo(".smodel instantiate OK (one asset -> two independent trees, materials + skinning intact)");
        }
        else
        {
            logError(".smodel instantiate MISMATCH");
        }
    }

    // Headless check (GPU-free): bake a model, extract its material to a standalone .smat keeping the
    // sub-id, confirm the remap + standalone row + that the resolver now reads the external file; then
    // clearExtraction reverts (remap gone, external deleted, row back to the embedded chunk).
    void runExtractSelfTest()
    {
        AssetServer testAssets;
        testAssets.root = "/tmp/saffron_extract_test";
        std::error_code ec;
        std::filesystem::remove_all(testAssets.root, ec);

        ImportedModel graph;
        graph.mesh.vertices.resize(3);
        graph.mesh.indices = { 0, 1, 2 };
        graph.mesh.submeshes = { Submesh{ .firstIndex = 0, .indexCount = 3, .vertexOffset = 0, .materialSlot = 0 } };
        ImportedMaterial brick;
        brick.name = "brick";
        brick.metallic = 0.7f;
        graph.materials = { brick };
        auto bake = bakeModel(testAssets, graph, ImportOptions{}, "/tmp/wall.glb", Uuid{ 0 });
        if (!bake)
        {
            logError(std::format("extract self-test: bake failed: {}", bake.error()));
            return;
        }
        for (const AssetEntry& row : bake->rows)
        {
            putAsset(testAssets.catalog, row);
        }

        const Uuid materialSubId = subIdFor("wall", "material", "brick", 0);
        auto extracted = extractSubAsset(testAssets, bake->modelId, materialSubId, std::string{});
        if (!extracted)
        {
            logError(std::format("extract self-test: extractSubAsset failed: {}", extracted.error()));
            return;
        }

        const std::string externalRel = "materials/" + std::to_string(materialSubId.value) + ".smat";
        const std::string externalFull = testAssets.root + "/" + externalRel;
        const AssetEntry* row = findAsset(testAssets.catalog, materialSubId);
        bool ok = extracted->value == materialSubId.value && std::filesystem::exists(externalFull) && row != nullptr &&
                  row->container.value == 0 && row->path == externalRel;
        if (auto meta = readContainerMetadata(testAssets.root + "/" + bake->path); meta)
        {
            ok = ok && meta->remap.is_object() && meta->remap.contains(std::to_string(materialSubId.value));
        }
        else
        {
            ok = false;
        }
        // The resolver prefers the external file now (metallic survives the round-trip through it).
        if (auto material = resolveMaterial(testAssets, bake->modelId, materialSubId); material)
        {
            ok = ok && material->metallic > 0.69f && material->metallic < 0.71f;
        }
        else
        {
            ok = false;
        }

        auto cleared = clearExtraction(testAssets, bake->modelId, materialSubId);
        const AssetEntry* reverted = findAsset(testAssets.catalog, materialSubId);
        bool revertOk = cleared.has_value() && !std::filesystem::exists(externalFull) && reverted != nullptr &&
                        reverted->container.value == bake->modelId.value;
        if (auto meta = readContainerMetadata(testAssets.root + "/" + bake->path); meta)
        {
            revertOk =
                revertOk && (!meta->remap.is_object() || !meta->remap.contains(std::to_string(materialSubId.value)));
        }
        else
        {
            revertOk = false;
        }

        if (ok && revertOk)
        {
            logInfo(".smodel extract + clear OK (standalone keeps sub-id, remap drives resolution, revert restores)");
        }
        else
        {
            logError(std::format(".smodel extract/clear MISMATCH (extract={}, revert={})", ok, revertOk));
        }
    }

    // Headless check: import a real glTF into a container, then exercise reimport's three outcomes —
    // an unchanged source skips, a drifted sourceHash re-bakes (same stable sub-ids → all `updated`),
    // and the refreshed hash makes the next reimport skip again.
    void runReimportSelfTest()
    {
        AssetServer testAssets;
        testAssets.root = "/tmp/saffron_reimport_test";
        std::error_code ec;
        std::filesystem::remove_all(testAssets.root, ec);

        const std::string source = assetPath("models/cube.gltf");
        auto bake = importModel(testAssets, source, ImportOptions{});
        if (!bake)
        {
            logError(std::format("reimport self-test: import failed: {}", bake.error()));
            return;
        }
        const std::string containerPath = testAssets.root + "/" + bake->path;

        auto first = reimportModel(testAssets, bake->modelId);
        const bool skipUnchanged = first && first->skipped;

        // Force a re-bake by staling the stored source hash.
        if (auto reader = readContainer(containerPath); reader)
        {
            if (auto meta = readContainerMetadata(containerPath); meta)
            {
                meta->import.sourceHash = "stale";
                static_cast<void>(rewriteContainerMeta(containerPath, *reader, *meta));
                testAssets.modelRefByUuid.erase(bake->modelId.value);
            }
        }
        auto second = reimportModel(testAssets, bake->modelId);
        const bool rebaked = second && !second->skipped && !second->updated.empty();

        auto third = reimportModel(testAssets, bake->modelId);
        const bool reskipped = third && third->skipped;

        if (skipUnchanged && rebaked && reskipped)
        {
            logInfo(".smodel reimport OK (skip-if-unchanged, re-bake on drift with stable sub-ids)");
        }
        else
        {
            logError(std::format(".smodel reimport MISMATCH (skip={}, rebake={}, reskip={})", skipUnchanged, rebaked,
                                 reskipped));
        }
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
        std::string shader = "shaders/mesh.spv";  // codegen materials point this at their übershader variant
    };

    // Resolves a loaded MaterialAsset (.smat) to a render-ready SubmeshMaterial, loading its
    // texture handles to bindless GPU textures. A packed ORM also feeds the occlusion slot (AO in
    // R), so one ARM/ORM map drives roughness (G), metalness (B), and AO (R).
    // Maps a resolved MaterialAsset to a SubmeshMaterial, resolving each texture slot through
    // `loadTex` (the main path passes loadTextureAsset; the thumbnail worker passes its own uploader).
    auto buildSubmeshMaterial(const MaterialAsset& mat, const std::function<Ref<GpuTexture>(Uuid)>& loadTex)
        -> SubmeshMaterial
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
            sm.albedoTexture = loadTex(mat.albedoTexture);
        }
        if (mat.ormTexture.value != 0)
        {
            sm.metallicRoughnessTexture = loadTex(mat.ormTexture);
            sm.occlusionTexture = loadTex(mat.ormTexture);
        }
        if (mat.normalTexture.value != 0)
        {
            sm.normalTexture = loadTex(mat.normalTexture);
        }
        if (mat.emissiveTexture.value != 0)
        {
            sm.emissiveTexture = loadTex(mat.emissiveTexture);
        }
        if (mat.heightTexture.value != 0)
        {
            sm.heightTexture = loadTex(mat.heightTexture);
        }
        return sm;
    }

    auto resolveMaterialAsset(AssetServer& assets, Renderer& renderer, const MaterialAsset& mat) -> SubmeshMaterial
    {
        return buildSubmeshMaterial(mat, [&](Uuid id) { return loadTextureAsset(assets, renderer, id); });
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
                // A non-foldable graph renders via its compiled übershader variant (built at
                // material-set-graph time). Fall back to the shared übershader if it isn't on disk yet.
                if (auto raw = loadMaterialAssetRaw(assets, matId);
                    raw && raw->graph.is_object() && !raw->graph.empty())
                {
                    MaterialAsset probe = *raw;
                    if (!lowerGraphToParams(raw->graph, probe))
                    {
                        const std::string spv = assets.root + "/materials/" + std::to_string(matId.value) + "_mesh.spv";
                        if (std::filesystem::exists(spv))
                        {
                            out.shader = spv;
                        }
                    }
                }
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
        auto model = translateModel(assetPath("models/editor-camera.glb"));
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
                item.material.shader = materials.shader;
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
                    item.material.shader = materials.shader;
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

    // === Async thumbnail generation ===
    //
    // A worker thread does the cold-cache work (decode + GPU upload + render + readback + PNG cache
    // write) so the frame loop never blocks on it. The job is a self-contained recipe resolved on the
    // main thread at enqueue (paths + resolved material), so the worker never touches the catalog or
    // the GPU caches; uploaded resources are handed back for the main thread to insert into the
    // caches. GPU thread-safety is the renderer's queue + bindless mutexes plus the worker's own
    // command pool (bindThumbnailWorkerThread). A cache miss replies `pending`; the editor retries,
    // and the completed PNG is then a plain disk-cache hit.

    // One texture the worker must decode + upload, resolved from the catalog at enqueue.
    struct ThumbnailTextureSource
    {
        Uuid id;
        std::string path;  // absolute source file under assets/
        bool hdr = false;  // .hdr float source -> uploadTextureFloat + tonemapped preview
        bool srgb = true;  // LDR colorspace: albedo/emissive sRGB, data maps linear
    };

    struct ThumbnailJob
    {
        Uuid id;
        u32 size = 0;
        AssetType type = AssetType::Texture;
        std::string cachePath;                                 // <projectRoot>/cache/thumbnails/<…>.png
        ThumbnailTextureSource texture;                        // type == Texture
        std::string meshPath;                                  // type == Mesh (standalone .smesh, absolute)
        std::vector<std::byte> meshBytes;                      // type == Mesh, embedded: the .smesh chunk image
        MaterialAsset material;                                // type == Material (parent-resolved)
        std::vector<ThumbnailTextureSource> materialTextures;  // the material's referenced textures
    };

    struct ThumbnailReply
    {
        std::vector<u8> png;
        u32 width = 0;
        u32 height = 0;
        bool pending = false;  // true: enqueued, not ready — the caller should retry
    };

    struct ThumbnailWorker
    {
        std::thread thread;
        std::mutex mutex;  // guards queue + inFlight + failed + the handback vectors
        std::condition_variable cv;
        std::deque<ThumbnailJob> queue;
        std::unordered_set<std::string> inFlight;  // cachePaths queued/running (dedup retries)
        std::unordered_set<std::string> failed;    // cachePaths that failed (settle to no-thumbnail)
        std::vector<std::pair<Uuid, Ref<GpuTexture>>> textureHandback;  // -> main inserts into the cache
        std::vector<std::pair<Uuid, Ref<GpuMesh>>> meshHandback;
        bool stop = false;
    };

    // Decode + upload one texture (worker or sync path); records (id, Ref) for cache handback.
    auto thumbnailUploadTexture(Renderer& renderer, const ThumbnailTextureSource& src,
                                std::vector<std::pair<Uuid, Ref<GpuTexture>>>& handback) -> Ref<GpuTexture>
    {
        if (src.hdr)
        {
            auto decoded = decodeImageHdr(src.path);
            if (!decoded)
            {
                logWarn(decoded.error());
                return nullptr;
            }
            auto tex = uploadTextureFloat(renderer, decoded->rgba.data(), decoded->width, decoded->height);
            if (!tex)
            {
                logWarn(tex.error());
                return nullptr;
            }
            handback.emplace_back(src.id, *tex);
            return *tex;
        }
        auto decoded = decodeImage(src.path);
        if (!decoded)
        {
            logWarn(decoded.error());
            return nullptr;
        }
        auto tex = uploadTexture(renderer, decoded->rgba.data(), decoded->width, decoded->height, src.srgb);
        if (!tex)
        {
            logWarn(tex.error());
            return nullptr;
        }
        handback.emplace_back(src.id, *tex);
        return *tex;
    }

    // Generates the PNG for a resolved job (no cache write, no catalog/map access). Uploaded GPU
    // resources are appended to texOut/meshOut for the caller to cache. Runs on the worker thread
    // (worker command pool, queue/bindless mutexes) or, with no worker, inline on the main thread.
    auto generateThumbnail(Renderer& renderer, const ThumbnailJob& job,
                           std::vector<std::pair<Uuid, Ref<GpuTexture>>>& texOut,
                           std::vector<std::pair<Uuid, Ref<GpuMesh>>>& meshOut) -> Result<ThumbnailPng>
    {
        if (job.type == AssetType::Texture)
        {
            auto tex = thumbnailUploadTexture(renderer, job.texture, texOut);
            if (!tex)
            {
                return Err(std::string{ "texture failed to load" });
            }
            return encodeTextureThumbnailPng(renderer, tex, job.size,
                                             job.texture.hdr ? PngTransfer::Tonemap : PngTransfer::Clamp);
        }
        if (job.type == AssetType::Mesh)
        {
            // Embedded sub-assets ship the .smesh chunk image in meshBytes (resolved on the main thread,
            // since the worker has no AssetServer); standalone meshes load from their file path.
            auto mesh = job.meshBytes.empty() ? loadMesh(job.meshPath)
                                              : loadMeshFromBytes(std::span<const std::byte>{ job.meshBytes });
            if (!mesh)
            {
                return Err(mesh.error());
            }
            auto meshRef = uploadMesh(renderer, *mesh);
            if (!meshRef)
            {
                return Err(meshRef.error());
            }
            meshOut.emplace_back(job.id, *meshRef);
            return encodeAssetThumbnailPng(renderer, *meshRef, job.size);
        }
        if (job.type == AssetType::Material)
        {
            std::unordered_map<u64, Ref<GpuTexture>> local;
            for (const ThumbnailTextureSource& src : job.materialTextures)
            {
                if (auto tex = thumbnailUploadTexture(renderer, src, texOut))
                {
                    local[src.id.value] = tex;
                }
            }
            const SubmeshMaterial sm =
                buildSubmeshMaterial(job.material,
                                     [&](Uuid tid) -> Ref<GpuTexture>
                                     {
                                         auto it = local.find(tid.value);
                                         return it != local.end() ? it->second : Ref<GpuTexture>{};
                                     });
            auto tex = renderMaterialPreview(renderer, sm, job.size);
            if (!tex)
            {
                return Err(tex.error());
            }
            return encodeTextureThumbnailPng(renderer, *tex, job.size);
        }
        return Err(std::string{ "asset has no thumbnail" });
    }

    // Insert handed-back GPU resources into the caches (skipping uuids already cached) — main thread.
    void insertThumbnailHandback(AssetServer& assets, std::vector<std::pair<Uuid, Ref<GpuTexture>>>& textures,
                                 std::vector<std::pair<Uuid, Ref<GpuMesh>>>& meshes)
    {
        for (auto& [id, tex] : textures)
        {
            if (tex && !assets.textureRefByUuid.contains(id.value))
            {
                assets.textureRefByUuid[id.value] = tex;
            }
        }
        for (auto& [id, mesh] : meshes)
        {
            if (mesh && !assets.meshRefByUuid.contains(id.value))
            {
                assets.meshRefByUuid[id.value] = mesh;
            }
        }
    }

    void thumbnailWorkerLoop(ThumbnailWorker* worker, Renderer* renderer)
    {
        bindThumbnailWorkerThread(*renderer);  // this thread allocates from the dedicated worker pool
        for (;;)
        {
            ThumbnailJob job;
            {
                std::unique_lock<std::mutex> lock(worker->mutex);
                worker->cv.wait(lock, [&] { return worker->stop || !worker->queue.empty(); });
                if (worker->stop)
                {
                    return;  // teardown / project switch: abandon any queued jobs
                }
                job = std::move(worker->queue.front());
                worker->queue.pop_front();
            }

            std::vector<std::pair<Uuid, Ref<GpuTexture>>> texOut;
            std::vector<std::pair<Uuid, Ref<GpuMesh>>> meshOut;
            auto png = generateThumbnail(*renderer, job, texOut, meshOut);

            const std::lock_guard<std::mutex> lock(worker->mutex);
            worker->inFlight.erase(job.cachePath);
            if (png)
            {
                if (auto written = writeThumbnailCache(job.cachePath, png->bytes); !written)
                {
                    logWarn(written.error());
                }
                for (auto& h : texOut)
                {
                    worker->textureHandback.push_back(std::move(h));
                }
                for (auto& h : meshOut)
                {
                    worker->meshHandback.push_back(std::move(h));
                }
            }
            else
            {
                logWarn(std::format("thumbnail {}: {}", job.id.value, png.error()));
                worker->failed.insert(job.cachePath);  // a missing thumbnail settles to the type icon
            }
        }
    }

    void startThumbnailWorker(AssetServer& assets, Renderer& renderer)
    {
        if (assets.thumbnailWorker)
        {
            return;
        }
        // Build the shared lazy pipelines/sphere on the main thread first; the worker must never be
        // the one to initialize them (that would race a concurrent main-thread read).
        if (auto warmed = prewarmThumbnailResources(renderer); !warmed)
        {
            logWarn(std::format("thumbnail worker not started: {}", warmed.error()));
            return;
        }
        auto worker = std::make_shared<ThumbnailWorker>();
        assets.thumbnailWorker = worker;
        worker->thread = std::thread(thumbnailWorkerLoop, worker.get(), &renderer);
    }

    // Drains the queue and joins the worker, then drops it. Called before waitGpuIdle/renderer
    // teardown: the worker's last submit has completed (its fences), and its un-handed-back textures
    // are referenced by no frame, so dropping them here frees their GPU resources safely.
    void stopThumbnailWorker(AssetServer& assets)
    {
        if (!assets.thumbnailWorker)
        {
            return;
        }
        ThumbnailWorker& worker = *assets.thumbnailWorker;
        {
            const std::lock_guard<std::mutex> lock(worker.mutex);
            worker.stop = true;
        }
        worker.cv.notify_all();
        if (worker.thread.joinable())
        {
            worker.thread.join();
        }
        assets.thumbnailWorker.reset();
    }

    // Abandon queued jobs + dedup/failed state + un-drained handbacks (a project switch; the GPU is
    // idle at the clearAssetCaches call site, so dropping the handback Refs frees them safely). An
    // already-running job finishes harmlessly and its single handback is dropped on the next switch.
    void clearThumbnailQueue(AssetServer& assets)
    {
        if (!assets.thumbnailWorker)
        {
            return;
        }
        ThumbnailWorker& worker = *assets.thumbnailWorker;
        const std::lock_guard<std::mutex> lock(worker.mutex);
        worker.queue.clear();
        worker.inFlight.clear();
        worker.failed.clear();
        worker.textureHandback.clear();
        worker.meshHandback.clear();
    }

    // Main thread: move the worker's finished uploads into the GPU caches. Call once per frame.
    void drainThumbnailCompletions(AssetServer& assets)
    {
        if (!assets.thumbnailWorker)
        {
            return;
        }
        ThumbnailWorker& worker = *assets.thumbnailWorker;
        std::vector<std::pair<Uuid, Ref<GpuTexture>>> textures;
        std::vector<std::pair<Uuid, Ref<GpuMesh>>> meshes;
        {
            const std::lock_guard<std::mutex> lock(worker.mutex);
            textures.swap(worker.textureHandback);
            meshes.swap(worker.meshHandback);
        }
        insertThumbnailHandback(assets, textures, meshes);
    }

    // Resolves {asset, size} to a thumbnail: a disk-cache hit returns the PNG; a miss generates it
    // (synchronously when there is no worker, else enqueued with a `pending` reply for the editor to
    // retry). Materials key on resolved state, mesh/texture on the source-file stat (phase 3/4).
    auto requestThumbnail(AssetServer& assets, Renderer& renderer, Uuid id, u32 size) -> Result<ThumbnailReply>
    {
        const AssetEntry* match = findAsset(assets.catalog, id);
        if (match == nullptr)
        {
            return Err(std::format("no asset {}", id.value));
        }

        ThumbnailJob job;
        job.id = id;
        job.size = size;
        job.type = match->type;
        std::string stamp;
        if (match->type == AssetType::Material)
        {
            auto loaded = loadMaterialAsset(assets, id);
            if (!loaded)
            {
                return Err(loaded.error());
            }
            job.material = *loaded;
            stamp = thumbnailMaterialStamp(*loaded);
            for (Uuid tid : { loaded->albedoTexture, loaded->ormTexture, loaded->normalTexture, loaded->emissiveTexture,
                              loaded->heightTexture })
            {
                if (tid.value == 0)
                {
                    continue;
                }
                const AssetEntry* te = findAsset(assets.catalog, tid);
                if (te != nullptr && te->type == AssetType::Texture)
                {
                    job.materialTextures.push_back(
                        ThumbnailTextureSource{ tid, assets.root + "/" + te->path, te->hdr, !te->linear });
                }
            }
        }
        else if (match->type == AssetType::Texture)
        {
            stamp = thumbnailSourceStamp(assets, *match);
            job.texture = ThumbnailTextureSource{ id, assets.root + "/" + match->path, match->hdr, !match->linear };
        }
        else if (match->type == AssetType::Mesh && match->container.value == 0)
        {
            stamp = thumbnailSourceStamp(assets, *match);
            job.meshPath = assets.root + "/" + match->path;
        }
        else if (match->type == AssetType::Mesh || match->type == AssetType::Model)
        {
            // An embedded mesh, or a model (previewed as its primary mesh sub-asset): resolve the
            // .smesh chunk bytes from the container on this (main) thread — the worker has no
            // AssetServer, so it parses the bytes we hand it rather than touching the catalog.
            Uuid containerId = match->container;
            Uuid meshSubId = id;
            if (match->type == AssetType::Model)
            {
                containerId = id;
                meshSubId = Uuid{ 0 };
                if (auto model = loadModelAsset(assets, id))
                {
                    for (const ContainerMetadata::SubAsset& sub : model->meta.subAssets)
                    {
                        if (sub.type == AssetType::Mesh)
                        {
                            meshSubId = sub.subId;
                            break;
                        }
                    }
                }
                if (meshSubId.value == 0)
                {
                    return Err(std::format("model {} has no mesh to preview", id.value));
                }
            }
            auto container = loadModelAsset(assets, containerId);
            if (!container)
            {
                return Err(std::format("model {} is not loadable", containerId.value));
            }
            const ByteSource source = chunkSourceFor(assets, *container, ChunkKind::Mesh, meshSubId);
            if (source.path.empty())
            {
                return Err(std::format("no mesh chunk for sub-asset {}", meshSubId.value));
            }
            auto bytes = source.read();
            if (!bytes)
            {
                return Err(bytes.error());
            }
            stamp = thumbnailSourceStamp(assets, *match);
            job.type = AssetType::Mesh;
            job.meshBytes = std::move(*bytes);
        }
        else
        {
            return Err(std::string{ "asset has no thumbnail" });
        }
        job.cachePath = stamp.empty() ? std::string{} : thumbnailCachePath(assets, id, size, stamp).string();

        if (!job.cachePath.empty())
        {
            if (auto hit = readThumbnailCache(job.cachePath))
            {
                return ThumbnailReply{ std::move(hit->bytes), hit->width, hit->height, false };
            }
        }

        // No worker, or no cache key to dedup/persist against: generate inline on the calling thread.
        if (!assets.thumbnailWorker || job.cachePath.empty())
        {
            std::vector<std::pair<Uuid, Ref<GpuTexture>>> texOut;
            std::vector<std::pair<Uuid, Ref<GpuMesh>>> meshOut;
            auto png = generateThumbnail(renderer, job, texOut, meshOut);
            if (!png)
            {
                return Err(png.error());
            }
            insertThumbnailHandback(assets, texOut, meshOut);
            if (!job.cachePath.empty())
            {
                if (auto written = writeThumbnailCache(job.cachePath, png->bytes); !written)
                {
                    logWarn(written.error());
                }
            }
            return ThumbnailReply{ std::move(png->bytes), png->width, png->height, false };
        }

        // Worker path: dedup on cachePath, enqueue once, reply pending.
        ThumbnailWorker& worker = *assets.thumbnailWorker;
        const std::lock_guard<std::mutex> lock(worker.mutex);
        if (worker.failed.contains(job.cachePath))
        {
            return Err(std::string{ "thumbnail generation failed" });
        }
        if (!worker.inFlight.contains(job.cachePath))
        {
            worker.inFlight.insert(job.cachePath);
            worker.queue.push_back(std::move(job));
            worker.cv.notify_one();
        }
        return ThumbnailReply{ {}, 0, 0, true };
    }
}
