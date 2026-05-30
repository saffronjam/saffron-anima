module;

// entt + glm are header-heavy C++ libraries, so this module uses classic
// includes (no `import std`), like the rendering/ui modules.
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <format>
#include <string>
#include <utility>

export module Saffron.Scene;

import Saffron.Core;

export namespace se
{
    // --- Components: plain value structs --------------------------------------
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

    glm::mat4 transformMatrix(const TransformComponent& transform)
    {
        glm::mat4 translation = glm::translate(glm::mat4(1.0f), transform.translation);
        glm::mat4 rotation = glm::mat4_cast(transform.rotation);
        glm::mat4 scale = glm::scale(glm::mat4(1.0f), transform.scale);
        return translation * rotation * scale;
    }

    // --- Scene + Entity handle ------------------------------------------------
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

    // Exercises the ECS end-to-end (entt views + glm transforms) at runtime.
    // Kept here so template instantiation stays inside the entt/glm-safe TU.
    void runSceneSelfTest()
    {
        Scene scene;
        createEntity(scene, "Camera");
        Entity cube = createEntity(scene, "Cube");
        getComponent<TransformComponent>(scene, cube).translation = glm::vec3(1.0f, 2.0f, 3.0f);

        u32 count = 0;
        forEach<NameComponent, TransformComponent>(
            scene,
            [&count](Entity, NameComponent& name, TransformComponent& transform)
            {
                logInfo(std::format("  entity '{}' at ({:.1f}, {:.1f}, {:.1f})",
                                    name.name, transform.translation.x,
                                    transform.translation.y, transform.translation.z));
                count = count + 1;
            });
        logInfo(std::format("scene self-test: {} entities iterated", count));
    }
}
