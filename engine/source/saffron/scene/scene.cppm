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
        glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };  // (w, x, y, z) identity
    };

    // References a mesh asset by stable id; the AssetServer resolves it to a GPU mesh.
    struct MeshComponent
    {
        Uuid mesh;
    };

    // A perspective camera; its view comes from the entity's TransformComponent.
    struct CameraComponent
    {
        f32 fov = 45.0f;        // vertical field of view, degrees
        f32 nearPlane = 0.1f;
        f32 farPlane = 100.0f;
        bool primary = true;    // the scene renders through the first primary camera
    };

    glm::mat4 transformMatrix(const TransformComponent& transform)
    {
        glm::mat4 translation = glm::translate(glm::mat4(1.0f), transform.translation);
        glm::mat4 rotation = glm::mat4_cast(transform.rotation);
        glm::mat4 scale = glm::scale(glm::mat4(1.0f), transform.scale);
        return translation * rotation * scale;
    }

    struct Scene
    {
        entt::registry registry;
    };

    // A lightweight, copyable handle — just an entt id. The Scene is always passed
    // explicitly to the free functions (Go-style: pass the world). An Entity is a
    // plain index, so it never dangles against a relocated Scene.
    struct Entity
    {
        entt::entity handle = entt::null;
    };

    bool valid(const Scene& scene, Entity entity)
    {
        return scene.registry.valid(entity.handle);
    }

    // Component access expressed as free generic functions (Go-style: generic
    // functions over the world + handle, not member templates on a class).
    template <typename C, typename... Args>
    C& addComponent(Scene& scene, Entity entity, Args&&... args)
    {
        return scene.registry.emplace<C>(entity.handle, std::forward<Args>(args)...);
    }

    template <typename C>
    C& getComponent(Scene& scene, Entity entity)
    {
        return scene.registry.get<C>(entity.handle);
    }

    template <typename C>
    bool hasComponent(const Scene& scene, Entity entity)
    {
        return scene.registry.all_of<C>(entity.handle);
    }

    template <typename C>
    void removeComponent(Scene& scene, Entity entity)
    {
        scene.registry.remove<C>(entity.handle);
    }

    Entity createEntity(Scene& scene, std::string name)
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

    // glm <-> json use named fields; quat storage order is config-dependent, so
    // never serialize positionally.
    nlohmann::json vec3ToJson(const glm::vec3& v)
    {
        return nlohmann::json{ { "x", v.x }, { "y", v.y }, { "z", v.z } };
    }

    glm::vec3 vec3FromJson(const nlohmann::json& j)
    {
        if (!j.is_object())
        {
            return glm::vec3{ 0.0f };
        }
        return glm::vec3{ j.value("x", 0.0f), j.value("y", 0.0f), j.value("z", 0.0f) };
    }

    nlohmann::json quatToJson(const glm::quat& q)
    {
        return nlohmann::json{ { "w", q.w }, { "x", q.x }, { "y", q.y }, { "z", q.z } };
    }

    glm::quat quatFromJson(const nlohmann::json& j)
    {
        if (!j.is_object())
        {
            return glm::quat{ 1.0f, 0.0f, 0.0f, 0.0f };
        }
        return glm::quat{ j.value("w", 1.0f), j.value("x", 0.0f), j.value("y", 0.0f), j.value("z", 0.0f) };
    }

    // ComponentTraits is a struct of std::function fields (a Go-interface itable);
    // every cross-cutting feature dispatches through it instead of a switch.
    inline constexpr int SceneVersion = 1;

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
        std::function<std::expected<void, std::string>(Scene&, Entity, const nlohmann::json&)> deserialize;
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
                           std::function<std::expected<void, std::string>(C&, const nlohmann::json&)> fromJson,
                           bool removable = true)
    {
        ComponentTraits traits;
        traits.id = entt::type_hash<C>::value();
        traits.name = name;
        traits.removable = removable;
        traits.has = [](Scene& s, Entity e) { return hasComponent<C>(s, e); };
        traits.addDefault = [](Scene& s, Entity e) { addComponent<C>(s, e); };
        traits.remove = [](Scene& s, Entity e) { removeComponent<C>(s, e); };
        traits.copyTo = [](Scene& src, Entity from, Scene& dst, Entity to)
        {
            if (hasComponent<C>(src, from))
            {
                addComponent<C>(dst, to, getComponent<C>(src, from));
            }
        };
        traits.serialize = [toJson](Scene& s, Entity e) -> nlohmann::json
        {
            return toJson(getComponent<C>(s, e));
        };
        traits.deserialize = [fromJson](Scene& s, Entity e, const nlohmann::json& j) -> std::expected<void, std::string>
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

    const ComponentTraits* findById(const ComponentRegistry& reg, entt::id_type id)
    {
        auto it = reg.byId.find(id);
        if (it == reg.byId.end())
        {
            return nullptr;
        }
        return &reg.rows[it->second];
    }

    const ComponentTraits* findByName(const ComponentRegistry& reg, const std::string& name)
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
    nlohmann::json serializeEntity(ComponentRegistry& reg, Scene& scene, Entity entity)
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

    std::expected<void, std::string> deserializeEntity(ComponentRegistry& reg, Scene& scene, Entity entity,
                                                       const nlohmann::json& components)
    {
        for (auto it = components.begin(); it != components.end(); ++it)
        {
            const ComponentTraits* traits = findByName(reg, it.key());
            if (traits == nullptr)
            {
                logWarn(std::format("unknown component '{}', skipping", it.key()));
                continue;
            }
            std::expected<void, std::string> result = traits->deserialize(scene, entity, it.value());
            if (!result)
            {
                return std::unexpected(std::format("{}: {}", it.key(), result.error()));
            }
        }
        return {};
    }

    std::expected<void, std::string> writeScene(ComponentRegistry& reg, Scene& scene, const std::string& path)
    {
        nlohmann::json doc;
        doc["version"] = SceneVersion;
        doc["entities"] = nlohmann::json::array();
        forEach<IdComponent>(scene, [&](Entity entity, IdComponent& id)
        {
            nlohmann::json entry;
            entry["id"] = id.id.value;
            entry["components"] = serializeEntity(reg, scene, entity);
            doc["entities"].push_back(std::move(entry));
        });

        std::ofstream out(path);
        if (!out)
        {
            return std::unexpected(std::format("cannot open '{}' for writing", path));
        }
        out << doc.dump(2);
        return {};
    }

    std::expected<void, std::string> readScene(ComponentRegistry& reg, Scene& scene, const std::string& path)
    {
        std::ifstream in(path);
        if (!in)
        {
            return std::unexpected(std::format("cannot open '{}'", path));
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
        if (doc.is_discarded())
        {
            return std::unexpected(std::format("'{}': JSON parse error", path));
        }
        if (!doc.is_object())
        {
            return std::unexpected(std::string{ "scene root is not an object" });
        }
        const int version = doc.value("version", 0);
        if (version != SceneVersion)
        {
            return std::unexpected(std::format("unsupported scene version {}", version));
        }
        if (!doc.contains("entities") || !doc["entities"].is_array())
        {
            return std::unexpected(std::string{ "scene missing 'entities' array" });
        }

        scene.registry.clear();
        std::unordered_map<u64, entt::entity> uuidToHandle;

        // Pass 1: create entities (preserving uuids — do NOT use createEntity, which
        // would mint fresh ones) and deserialize their components.
        for (const nlohmann::json& entry : doc["entities"])
        {
            if (!entry.is_object())
            {
                return std::unexpected(std::string{ "entity entry is not an object" });
            }
            const u64 uuid = entry.value("id", u64{ 0 });
            if (uuid == 0)
            {
                return std::unexpected(std::string{ "entity missing 'id'" });
            }
            entt::entity handle = scene.registry.create();
            scene.registry.emplace<IdComponent>(handle, Uuid{ uuid });
            uuidToHandle.emplace(uuid, handle);

            if (entry.contains("components") && entry["components"].is_object())
            {
                std::expected<void, std::string> result =
                    deserializeEntity(reg, scene, Entity{ handle }, entry["components"]);
                if (!result)
                {
                    return std::unexpected(result.error());
                }
            }
        }

        // Pass 2: resolve cross-entity references (uuid -> live handle). No
        // reference-holding components exist yet; the hook is ready for them.
        static_cast<void>(uuidToHandle);
        return {};
    }

    // Headless round-trip check: build a registry, populate a scene, write + read
    // it back, and confirm the data survives. Replaces the old ECS smoke test.
    void runSceneSerializationSelfTest()
    {
        ComponentRegistry reg;
        registerComponent<NameComponent>(reg, "Name",
            [](Scene&, Entity) {},
            [](const NameComponent& c) { return nlohmann::json{ { "name", c.name } }; },
            [](NameComponent& c, const nlohmann::json& j) -> std::expected<void, std::string>
            {
                c.name = j.value("name", std::string{});
                return {};
            },
            false);
        registerComponent<TransformComponent>(reg, "Transform",
            [](Scene&, Entity) {},
            [](const TransformComponent& t)
            {
                return nlohmann::json{ { "translation", vec3ToJson(t.translation) },
                                       { "scale", vec3ToJson(t.scale) },
                                       { "rotation", quatToJson(t.rotation) } };
            },
            [](TransformComponent& t, const nlohmann::json& j) -> std::expected<void, std::string>
            {
                t.translation = vec3FromJson(j.value("translation", nlohmann::json::object()));
                t.scale = vec3FromJson(j.value("scale", nlohmann::json::object()));
                t.rotation = quatFromJson(j.value("rotation", nlohmann::json::object()));
                return {};
            },
            false);

        Scene scene;
        createEntity(scene, "Camera");
        Entity cube = createEntity(scene, "Cube");
        getComponent<TransformComponent>(scene, cube).translation = glm::vec3(1.0f, 2.0f, 3.0f);

        const std::string path = "/tmp/saffron_scene_selftest.json";
        std::expected<void, std::string> wrote = writeScene(reg, scene, path);
        if (!wrote)
        {
            logError(std::format("scene self-test write failed: {}", wrote.error()));
            return;
        }

        Scene loaded;
        std::expected<void, std::string> read = readScene(reg, loaded, path);
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
