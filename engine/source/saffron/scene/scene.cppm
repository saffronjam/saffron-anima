module;

// entt + glm are header-heavy C++ libraries, so this module uses classic
// includes (no `import std`), like the rendering/ui modules.
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <expected>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

export module Saffron.Scene;

import Saffron.Core;
import Saffron.Json;

export namespace se
{
    struct NameComponent
    {
        std::string name;
    };

    struct IdComponent
    {
        Uuid id;
    };

    struct TransformComponent
    {
        glm::vec3 translation{ 0.0f };
        glm::vec3 scale{ 1.0f };
        glm::vec3 rotation{ 0.0f };  // Euler XYZ radians; the editor edits these directly
    };

    // References a mesh asset by stable id; the AssetServer resolves it to a GPU mesh.
    struct MeshComponent
    {
        Uuid mesh;
    };

    // Per-entity material applied to the whole mesh. albedoTexture == 0 means "none"
    // (the renderer binds its default white texture, so baseColor shows directly).
    // metallic/roughness drive the Cook-Torrance BRDF; emissive adds unlit radiance.
    struct MaterialComponent
    {
        glm::vec4 baseColor{ 1.0f };
        Uuid albedoTexture;
        f32 metallic = 0.0f;
        f32 roughness = 1.0f;
        glm::vec3 emissive{ 0.0f };
        f32 emissiveStrength = 1.0f;
        bool unlit = false;  // skip lighting (albedo * base color only) — a distinct PSO
    };

    // A perspective camera; its view comes from the entity's TransformComponent.
    struct CameraComponent
    {
        f32 fov = 45.0f;        // vertical field of view, degrees
        f32 nearPlane = 0.1f;
        f32 farPlane = 100.0f;
        bool primary = true;    // the scene renders through the first primary camera
    };

    // A directional light; the scene shades through the first one. direction points
    // the way the light travels.
    struct DirectionalLightComponent
    {
        glm::vec3 direction{ -0.5f, -1.0f, -0.3f };
        glm::vec3 color{ 1.0f };
        f32 intensity = 1.0f;
        f32 ambient = 0.15f;
    };

    // An omnidirectional light positioned at the entity's Transform translation, with
    // smooth distance falloff out to range. Culled into clusters by the light system.
    struct PointLightComponent
    {
        glm::vec3 color{ 1.0f };
        f32 intensity = 5.0f;
        f32 range = 10.0f;
    };

    // A cone light at the entity's Transform translation, aimed by direction. Falls
    // off by distance (range) and by angle between innerAngle and outerAngle (degrees).
    struct SpotLightComponent
    {
        glm::vec3 direction{ 0.0f, -1.0f, 0.0f };
        glm::vec3 color{ 1.0f };
        f32 intensity = 5.0f;
        f32 range = 10.0f;
        f32 innerAngle = 20.0f;  // full intensity inside this half-angle
        f32 outerAngle = 30.0f;  // zero past this half-angle
    };

    // A reflection probe at the entity's Transform translation. Captures a local cubemap of
    // the scene, prefilters it like the global IBL, and supplies specular ambient to meshes
    // inside its influence sphere (influenceRadius). Outside every probe, meshes fall back to
    // the global IBL. boxProjection re-projects the prefiltered reflection ray against the
    // influence box for parallax-correct local reflections (off = infinite-distance cube).
    struct ReflectionProbeComponent
    {
        f32 influenceRadius = 10.0f;   // sphere of effect around the probe origin
        f32 intensity = 1.0f;          // probe specular multiplier
        bool boxProjection = false;    // parallax-correct against the influence box
        glm::vec3 boxExtent{ 10.0f };  // half-extents for box projection (used when boxProjection)
        bool dirty = true;             // capture pending; set on add/edit, cleared after capture (runtime only)
    };

    auto transformMatrix(const TransformComponent& transform) -> glm::mat4
    {
        glm::mat4 translation = glm::translate(glm::mat4(1.0f), transform.translation);
        glm::mat4 rotation = glm::mat4_cast(glm::quat(transform.rotation));
        glm::mat4 scale = glm::scale(glm::mat4(1.0f), transform.scale);
        return translation * rotation * scale;
    }

    // A project asset (a model imported + baked to a mesh, or a texture). The catalog
    // maps these by id; each entry carries a human name (UTF-8, renameable) and the
    // relative path to the baked .smesh / copied texture under the asset root.
    enum class AssetType { Mesh, Texture, Other };

    struct AssetEntry
    {
        Uuid id;
        std::string name;
        AssetType type = AssetType::Mesh;
        std::string path;  // relative to the asset root
        bool hdr = false;  // texture: decode as linear float (.hdr); else sRGB RGBA8
    };

    struct AssetCatalog
    {
        std::vector<AssetEntry> entries;
        std::unordered_map<u64, std::size_t> byId;  // id -> index into entries
    };

    auto findAsset(const AssetCatalog& catalog, Uuid id) -> const AssetEntry*
    {
        auto it = catalog.byId.find(id.value);
        if (it == catalog.byId.end())
        {
            return nullptr;
        }
        return &catalog.entries[it->second];
    }

    void putAsset(AssetCatalog& catalog, AssetEntry entry)
    {
        auto it = catalog.byId.find(entry.id.value);
        if (it != catalog.byId.end())
        {
            catalog.entries[it->second] = std::move(entry);
            return;
        }
        catalog.byId[entry.id.value] = catalog.entries.size();
        catalog.entries.push_back(std::move(entry));
    }

    auto renameAsset(AssetCatalog& catalog, Uuid id, std::string name) -> bool
    {
        auto it = catalog.byId.find(id.value);
        if (it == catalog.byId.end())
        {
            return false;
        }
        catalog.entries[it->second].name = std::move(name);
        return true;
    }

    // A name not already used by another entry (appends " (2)", " (3)", … on collision).
    auto uniqueName(const AssetCatalog& catalog, const std::string& base) -> std::string
    {
        bool taken = false;
        for (const AssetEntry& entry : catalog.entries)
        {
            if (entry.name == base)
            {
                taken = true;
            }
        }
        if (!taken)
        {
            return base;
        }
        for (u32 suffix = 2; ; suffix = suffix + 1)
        {
            std::string candidate = base + " (" + std::to_string(suffix) + ")";
            bool clash = false;
            for (const AssetEntry& entry : catalog.entries)
            {
                if (entry.name == candidate)
                {
                    clash = true;
                }
            }
            if (!clash)
            {
                return candidate;
            }
        }
    }

    // How the visible sky background is produced. Color = a flat fill; Texture = an
    // equirectangular panorama asset; Procedural = the renderer's baked procedural-sky
    // environment cube (the same cube that feeds IBL, so background and lighting match).
    enum class SkyMode
    {
        Color,
        Texture,
        Procedural,
    };

    // Scene-wide environment / sky state. Global frame state (no transform, not picked,
    // not in the hierarchy), so it lives on the Scene rather than as an entity component.
    // The renderer resolves it into a SkyRenderSettings each frame (see renderScene).
    struct SceneEnvironment
    {
        SkyMode skyMode = SkyMode::Procedural;
        glm::vec3 clearColor{ 0.05f, 0.06f, 0.08f };  // Color mode + clear fallback
        Uuid skyTexture;                              // Texture mode panorama; 0 = none
        f32 skyIntensity = 1.0f;
        f32 skyRotation = 0.0f;                       // yaw radians applied to the sky lookup
        f32 exposure = 1.0f;                          // reserved; tonemap exposure is set via the renderer
        bool visible = true;
        bool useSkyForAmbient = true;                 // drive fallback ambient from ambientColor below
        glm::vec3 ambientColor{ 1.0f };               // non-IBL fallback ambient tint
        f32 ambientIntensity = 0.15f;
    };

    struct Scene
    {
        entt::registry registry;
        SceneEnvironment environment;
        const AssetCatalog* catalog = nullptr;  // borrowed; set per-frame by the client, not owned or serialized
    };

    // A lightweight, copyable handle — just an entt id. The Scene is always passed
    // explicitly to the free functions (Go-style: pass the world). An Entity is a
    // plain index, so it never dangles against a relocated Scene.
    struct Entity
    {
        entt::entity handle = entt::null;
    };

    auto valid(const Scene& scene, Entity entity) -> bool
    {
        return scene.registry.valid(entity.handle);
    }

    // Component access expressed as free generic functions (Go-style: generic
    // functions over the world + handle, not member templates on a class).
    template <typename C, typename... Args>
    auto addComponent(Scene& scene, Entity entity, Args&&... args) -> C&
    {
        return scene.registry.emplace<C>(entity.handle, std::forward<Args>(args)...);
    }

    template <typename C>
    auto getComponent(Scene& scene, Entity entity) -> C&
    {
        return scene.registry.get<C>(entity.handle);
    }

    template <typename C>
    auto hasComponent(const Scene& scene, Entity entity) -> bool
    {
        return scene.registry.all_of<C>(entity.handle);
    }

    template <typename C>
    void removeComponent(Scene& scene, Entity entity)
    {
        scene.registry.remove<C>(entity.handle);
    }

    auto createEntity(Scene& scene, std::string name) -> Entity
    {
        Entity entity{ scene.registry.create() };
        addComponent<IdComponent>(scene, entity, newUuid());
        addComponent<NameComponent>(scene, entity, std::move(name));
        addComponent<TransformComponent>(scene, entity);
        return entity;
    }

    void destroyEntity(Scene& scene, Entity entity)
    {
        scene.registry.destroy(entity.handle);
    }

    // Iterate every entity carrying the given components.
    // The callback receives (Entity, C&...).
    template <typename... C, typename Fn>
    void forEach(Scene& scene, Fn&& fn)
    {
        auto view = scene.registry.view<C...>();
        for (entt::entity handle : view)
        {
            fn(Entity{ handle }, view.template get<C>(handle)...);
        }
    }

    // The resolved primary camera: its view matrix + projection parameters. valid is
    // false when the scene has no primary camera. The projection is left un-flipped;
    // the renderer applies the Vulkan Y-flip where it samples, and the editor gizmo
    // consumes it as-is (one source of truth for both).
    struct CameraView
    {
        glm::mat4 view{ 1.0f };
        f32 fov = 45.0f;
        f32 nearPlane = 0.1f;
        f32 farPlane = 100.0f;
        bool valid = false;
    };

    auto primaryCamera(Scene& scene) -> CameraView
    {
        CameraView result;
        forEach<TransformComponent, CameraComponent>(scene,
            [&](Entity, TransformComponent& transform, CameraComponent& camera)
        {
                if (result.valid || !camera.primary)
                {
                    return;
                }
                const glm::mat4 model =
                    glm::translate(glm::mat4(1.0f), transform.translation) * glm::mat4_cast(glm::quat(transform.rotation));
                result.view = glm::inverse(model);
                result.fov = camera.fov;
                result.nearPlane = camera.nearPlane;
                result.farPlane = camera.farPlane;
                result.valid = true;
            });
        return result;
    }

    // Un-flipped perspective projection for the resolved camera (GL clip convention).
    auto cameraProjection(const CameraView& camera, f32 aspect) -> glm::mat4
    {
        return glm::perspective(glm::radians(camera.fov), aspect, camera.nearPlane, camera.farPlane);
    }

    // glm <-> json use named fields; quat storage order is config-dependent, so
    // never serialize positionally.
    auto vec3ToJson(const glm::vec3& v) -> nlohmann::json
    {
        return nlohmann::json{ { "x", v.x }, { "y", v.y }, { "z", v.z } };
    }

    auto vec3FromJson(const nlohmann::json& j) -> glm::vec3
    {
        return glm::vec3{ jsonF32Or(j, "x", 0.0f), jsonF32Or(j, "y", 0.0f), jsonF32Or(j, "z", 0.0f) };
    }


    auto vec4ToJson(const glm::vec4& v) -> nlohmann::json
    {
        return nlohmann::json{ { "x", v.x }, { "y", v.y }, { "z", v.z }, { "w", v.w } };
    }

    auto vec4FromJson(const nlohmann::json& j) -> glm::vec4
    {
        return glm::vec4{ jsonF32Or(j, "x", 1.0f), jsonF32Or(j, "y", 1.0f),
                          jsonF32Or(j, "z", 1.0f), jsonF32Or(j, "w", 1.0f) };
    }

    auto skyModeName(SkyMode mode) -> const char*
    {
        switch (mode)
        {
            case SkyMode::Color: return "color";
            case SkyMode::Texture: return "texture";
            case SkyMode::Procedural: return "procedural";
        }
        return "procedural";
    }

    auto skyModeFromName(const std::string& name) -> SkyMode
    {
        if (name == "color") { return SkyMode::Color; }
        if (name == "texture") { return SkyMode::Texture; }
        if (name == "procedural") { return SkyMode::Procedural; }
        logWarn(std::format("unknown sky mode '{}', defaulting to procedural", name));
        return SkyMode::Procedural;
    }

    auto environmentToJson(const SceneEnvironment& env) -> nlohmann::json
    {
        return nlohmann::json{
            { "skyMode", skyModeName(env.skyMode) },
            { "clearColor", vec3ToJson(env.clearColor) },
            { "skyTexture", uuidToJson(env.skyTexture.value) },
            { "skyIntensity", env.skyIntensity },
            { "skyRotation", env.skyRotation },
            { "exposure", env.exposure },
            { "visible", env.visible },
            { "useSkyForAmbient", env.useSkyForAmbient },
            { "ambientColor", vec3ToJson(env.ambientColor) },
            { "ambientIntensity", env.ambientIntensity },
        };
    }

    // Reads an environment block, filling defaults for any missing field (so a partial or
    // absent block — e.g. a migrated v1 scene — yields a sensible default environment).
    auto environmentFromJson(const nlohmann::json& j) -> SceneEnvironment
    {
        SceneEnvironment env;
        if (!j.is_object())
        {
            return env;
        }
        env.skyMode = skyModeFromName(jsonStringOr(j, "skyMode", "procedural"));
        if (j.contains("clearColor")) { env.clearColor = vec3FromJson(j["clearColor"]); }
        env.skyTexture = Uuid{ jsonU64Or(j, "skyTexture", 0) };
        env.skyIntensity = jsonF32Or(j, "skyIntensity", 1.0f);
        env.skyRotation = jsonF32Or(j, "skyRotation", 0.0f);
        env.exposure = jsonF32Or(j, "exposure", 1.0f);
        env.visible = jsonBoolOr(j, "visible", true);
        env.useSkyForAmbient = jsonBoolOr(j, "useSkyForAmbient", true);
        if (j.contains("ambientColor")) { env.ambientColor = vec3FromJson(j["ambientColor"]); }
        env.ambientIntensity = jsonF32Or(j, "ambientIntensity", 0.15f);
        return env;
    }

    // ComponentTraits is a struct of std::function fields (a Go-interface itable);
    // every cross-cutting feature dispatches through it instead of a switch.
    //
    // Version history: 1 = entities only; 2 = adds the top-level "environment" block.
    // sceneFromJson migrates a v1 document by defaulting the environment.
    inline constexpr int SceneVersion = 2;

    struct ComponentTraits
    {
        entt::id_type id = 0;   // == entt::type_hash<C>::value(); the storage() join key
        std::string name;       // stable JSON key + UI header, e.g. "Transform"
        bool removable = true;
        std::function<bool(Scene&, Entity)> has;
        std::function<void(Scene&, Entity)> addDefault;
        std::function<void(Scene&, Entity)> remove;
        std::function<void(Scene&, Entity, Scene&, Entity)> copyTo;  // clone src -> dst
        std::function<nlohmann::json(Scene&, Entity)> serialize;
        std::function<Result<void>(Scene&, Entity, const nlohmann::json&)> deserialize;
        std::function<void(Scene&, Entity)> drawInspector;  // opaque here; ImGui body lives in the editor
    };

    struct ComponentRegistry
    {
        std::vector<ComponentTraits> rows;
        std::unordered_map<entt::id_type, std::size_t> byId;
        std::unordered_map<std::string, std::size_t> byName;
    };

    // Register a component ONCE. Synthesizes every closure from the existing
    // generic addComponent/getComponent/hasComponent/removeComponent. Adding a
    // new component type elsewhere = one call to this; zero edits to the rest.
    template <typename C>
    void registerComponent(ComponentRegistry& reg, std::string name,
                           std::function<void(Scene&, Entity)> drawFn,
                           std::function<nlohmann::json(const C&)> toJson,
                           std::function<Result<void>(C&, const nlohmann::json&)> fromJson,
                           bool removable = true)
    {
        ComponentTraits traits;
        traits.id = entt::type_hash<C>::value();
        traits.name = name;
        traits.removable = removable;
        traits.has = [](Scene& s, Entity e) -> auto { return hasComponent<C>(s, e); };
        traits.addDefault = [](Scene& s, Entity e) -> auto { addComponent<C>(s, e); };
        traits.remove = [](Scene& s, Entity e) -> auto { removeComponent<C>(s, e); };
        traits.copyTo = [](Scene& src, Entity from, Scene& dst, Entity to)
        -> auto {
            if (hasComponent<C>(src, from))
            {
                addComponent<C>(dst, to, getComponent<C>(src, from));
            }
        };
        traits.serialize = [toJson](Scene& s, Entity e) -> nlohmann::json
        {
            return toJson(getComponent<C>(s, e));
        };
        traits.deserialize = [fromJson](Scene& s, Entity e, const nlohmann::json& j) -> Result<void>
        {
            if (!hasComponent<C>(s, e))
            {
                addComponent<C>(s, e);
            }
            return fromJson(getComponent<C>(s, e), j);
        };
        traits.drawInspector = std::move(drawFn);

        const std::size_t index = reg.rows.size();
        reg.byId[traits.id] = index;
        reg.byName[name] = index;
        reg.rows.push_back(std::move(traits));
    }

    auto findById(const ComponentRegistry& reg, entt::id_type id) -> const ComponentTraits*
    {
        auto it = reg.byId.find(id);
        if (it == reg.byId.end())
        {
            return nullptr;
        }
        return &reg.rows[it->second];
    }

    auto findByName(const ComponentRegistry& reg, const std::string& name) -> const ComponentTraits*
    {
        auto it = reg.byName.find(name);
        if (it == reg.byName.end())
        {
            return nullptr;
        }
        return &reg.rows[it->second];
    }

    // Scene& is non-const because entt views/storage iteration require it; these
    // functions do not logically mutate the scene.
    auto serializeEntity(ComponentRegistry& reg, Scene& scene, Entity entity) -> nlohmann::json
    {
        nlohmann::json components = nlohmann::json::object();
        for (auto&& [id, set] : scene.registry.storage())
        {
            if (!set.contains(entity.handle))
            {
                continue;
            }
            const ComponentTraits* traits = findById(reg, id);
            if (traits == nullptr)
            {
                continue;  // unregistered/internal storage (e.g. IdComponent) — skipped
            }
            components[traits->name] = traits->serialize(scene, entity);
        }
        return components;
    }

    auto deserializeEntity(ComponentRegistry& reg, Scene& scene, Entity entity,
                                                       const nlohmann::json& components) -> Result<void>
    {
        for (auto it = components.begin(); it != components.end(); ++it)
        {
            const ComponentTraits* traits = findByName(reg, it.key());
            if (traits == nullptr)
            {
                logWarn(std::format("unknown component '{}', skipping", it.key()));
                continue;
            }
            auto result = traits->deserialize(scene, entity, it.value());
            if (!result)
            {
                return Err(std::format("{}: {}", it.key(), result.error()));
            }
        }
        return {};
    }

    // Serializes the scene to a `{version, entities:[{id,components}]}` document (no file
    // IO), so it can be embedded in a larger project document.
    auto sceneToJson(ComponentRegistry& reg, Scene& scene) -> nlohmann::json
    {
        nlohmann::json doc;
        doc["version"] = SceneVersion;
        doc["environment"] = environmentToJson(scene.environment);
        doc["entities"] = nlohmann::json::array();
        forEach<IdComponent>(scene, [&](Entity entity, IdComponent& id)
        {
            nlohmann::json entry;
            entry["id"] = uuidToJson(id.id.value);
            entry["components"] = serializeEntity(reg, scene, entity);
            doc["entities"].push_back(std::move(entry));
        });
        return doc;
    }

    // Replaces the scene's entities from a `sceneToJson` document.
    auto sceneFromJson(ComponentRegistry& reg, Scene& scene, const nlohmann::json& doc) -> Result<void>
    {
        if (!doc.is_object())
        {
            return Err(std::string{ "scene root is not an object" });
        }
        const int version = static_cast<int>(jsonU64Or(doc, "version", 0));
        if (version < 1 || version > SceneVersion)
        {
            return Err(std::format("unsupported scene version {}", version));
        }
        if (!doc.contains("entities") || !doc["entities"].is_array())
        {
            return Err(std::string{ "scene missing 'entities' array" });
        }

        // v1 has no "environment" block; environmentFromJson defaults it. v2+ carries one.
        scene.environment = environmentFromJson(doc.contains("environment") ? doc["environment"] : nlohmann::json{});

        scene.registry.clear();
        std::unordered_map<u64, entt::entity> uuidToHandle;

        // Create entities preserving uuids (NOT createEntity, which mints fresh ones)
        // and deserialize their components.
        for (const nlohmann::json& entry : doc["entities"])
        {
            if (!entry.is_object())
            {
                return Err(std::string{ "entity entry is not an object" });
            }
            const u64 uuid = jsonU64Or(entry, "id", 0);
            if (uuid == 0)
            {
                return Err(std::string{ "entity missing 'id'" });
            }
            entt::entity handle = scene.registry.create();
            scene.registry.emplace<IdComponent>(handle, Uuid{ uuid });
            uuidToHandle.emplace(uuid, handle);

            if (entry.contains("components") && entry["components"].is_object())
            {
                Result<void> result =
                    deserializeEntity(reg, scene, Entity{ handle }, entry["components"]);
                if (!result)
                {
                    return Err(result.error());
                }
            }
        }

        // Resolve cross-entity references (uuid -> live handle). No reference-holding
        // components exist yet; the hook is ready for them.
        static_cast<void>(uuidToHandle);
        return {};
    }

    auto writeScene(ComponentRegistry& reg, Scene& scene, const std::string& path) -> Result<void>
    {
        std::ofstream out(path);
        if (!out)
        {
            return Err(std::format("cannot open '{}' for writing", path));
        }
        out << dumpJson(sceneToJson(reg, scene), 2);
        out.flush();
        if (!out)
        {
            return Err(std::format("write failed for '{}'", path));
        }
        return {};
    }

    auto readScene(ComponentRegistry& reg, Scene& scene, const std::string& path) -> Result<void>
    {
        std::ifstream in(path);
        if (!in)
        {
            return Err(std::format("cannot open '{}'", path));
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        auto doc = parseJson(text);
        if (!doc)
        {
            return Err(std::format("'{}': {}", path, doc.error()));
        }
        return sceneFromJson(reg, scene, *doc);
    }

    // Headless round-trip check: build a registry, populate a scene, write + read
    // it back, and confirm the data survives. Replaces the old ECS smoke test.
    void runSceneSerializationSelfTest()
    {
        ComponentRegistry reg;
        registerComponent<NameComponent>(reg, "Name",
            [](Scene&, Entity) {},
            [](const NameComponent& c) -> nlohmann::json { return nlohmann::json{ { "name", c.name } }; },
            [](NameComponent& c, const nlohmann::json& j) -> Result<void>
            {
                c.name = jsonStringOr(j, "name", std::string{});
                return {};
            },
            false);
        registerComponent<TransformComponent>(reg, "Transform",
            [](Scene&, Entity) {},
            [](const TransformComponent& t)
            -> nlohmann::json {
                return nlohmann::json{ { "translation", vec3ToJson(t.translation) },
                                       { "scale", vec3ToJson(t.scale) },
                                       { "rotation", vec3ToJson(t.rotation) } };
            },
            [](TransformComponent& t, const nlohmann::json& j) -> Result<void>
            {
                t.translation = vec3FromJson(j.value("translation", nlohmann::json::object()));
                t.scale = vec3FromJson(j.value("scale", nlohmann::json::object()));
                t.rotation = vec3FromJson(j.value("rotation", nlohmann::json::object()));
                return {};
            },
            false);

        Scene scene;
        createEntity(scene, "Camera");
        Entity cube = createEntity(scene, "Cube");
        getComponent<TransformComponent>(scene, cube).translation = glm::vec3(1.0f, 2.0f, 3.0f);

        const std::string path = "/tmp/saffron_scene_selftest.json";
        auto wrote = writeScene(reg, scene, path);
        if (!wrote)
        {
            logError(std::format("scene self-test write failed: {}", wrote.error()));
            return;
        }

        Scene loaded;
        auto read = readScene(reg, loaded, path);
        if (!read)
        {
            logError(std::format("scene self-test read failed: {}", read.error()));
            return;
        }

        u32 count = 0;
        glm::vec3 cubePos{ 0.0f };
        forEach<NameComponent, TransformComponent>(loaded,
            [&](Entity, NameComponent& name, TransformComponent& transform)
        {
                count = count + 1;
                if (name.name == "Cube")
                {
                    cubePos = transform.translation;
                }
            });
        logInfo(std::format("scene round-trip: {} entities, cube at ({:.1f}, {:.1f}, {:.1f})",
                            count, cubePos.x, cubePos.y, cubePos.z));
    }
}
