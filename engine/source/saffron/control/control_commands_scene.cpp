module;

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <charconv>
#include <format>
#include <string>

module Saffron.Control;

import Saffron.Core;
import Saffron.Rendering;
import Saffron.Scene;
import Saffron.Editor;
import Saffron.Assets;

namespace se
{
    void registerSceneCommands(CommandRegistry& reg)
    {
        registerCommand(reg, "list-entities", "list all entities",
            [](EngineContext& ctx, const json&) -> Result<json>
            {
                json entities = json::array();
                forEach<IdComponent, NameComponent>(ctx.editor.scene,
                    [&](Entity, IdComponent& id, NameComponent& name)
        {
                        entities.push_back(json{ { "id", id.id.value }, { "name", name.name } });
                    });
                return json{ { "entities", std::move(entities) } };
            });

        registerCommand(reg, "list-components", "list registered component types",
            [](EngineContext& ctx, const json&) -> Result<json>
            {
                json names = json::array();
                for (const ComponentTraits& traits : ctx.editor.registry.rows)
                {
                    names.push_back(traits.name);
                }
                return json{ { "components", std::move(names) } };
            });

        registerCommand(reg, "create-entity", "create-entity {name}",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const std::string name = asString(positionalOr(params, "name", 0), "Entity");
                Entity entity = createEntity(ctx.editor.scene, name);
                return entityRef(ctx.editor.scene, entity);
            });

        registerCommand(reg, "destroy-entity", "destroy-entity {entity}",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                auto entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return Err(entity.error());
                }
                const u64 id = getComponent<IdComponent>(ctx.editor.scene, *entity).id.value;
                if (ctx.editor.selected.handle == entity->handle)
                {
                    setSelection(ctx.editor, Entity{ entt::null });
                }
                destroyEntity(ctx.editor.scene, *entity);
                return json{ { "destroyed", id } };
            });

        registerCommand(reg, "add-component", "add-component {entity, component}",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                auto entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return Err(entity.error());
                }
                const std::string name = asString(positionalOr(params, "component", 1), "");
                const ComponentTraits* row = findByName(ctx.editor.registry, name);
                if (row == nullptr)
                {
                    return Err(std::format("unknown component '{}'", name));
                }
                if (row->has(ctx.editor.scene, *entity))
                {
                    return Err(std::format("entity already has '{}'", name));
                }
                row->addDefault(ctx.editor.scene, *entity);
                return json{ { "added", row->name } };
            });

        registerCommand(reg, "remove-component", "remove-component {entity, component}",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                auto entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return Err(entity.error());
                }
                const std::string name = asString(positionalOr(params, "component", 1), "");
                const ComponentTraits* row = findByName(ctx.editor.registry, name);
                if (row == nullptr)
                {
                    return Err(std::format("unknown component '{}'", name));
                }
                if (!row->removable)
                {
                    return Err(std::format("component '{}' is not removable", row->name));
                }
                row->remove(ctx.editor.scene, *entity);
                return json{ { "removed", row->name } };
            });

        // Applies a component's serialized form. Routing through the registry's
        // deserialize keeps the wire shape identical to scene files.
        registerCommand(reg, "set-component", "set-component {entity, component, json}",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                auto entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return Err(entity.error());
                }
                const std::string name = asString(positionalOr(params, "component", 1), "");
                const ComponentTraits* row = findByName(ctx.editor.registry, name);
                if (row == nullptr)
                {
                    return Err(std::format("unknown component '{}'", name));
                }
                const json body = params.value("json", json::object());
                auto result = row->deserialize(ctx.editor.scene, *entity, body);
                if (!result)
                {
                    return Err(result.error());
                }
                return json{ { "set", row->name } };
            });

        // Routes through the Transform row's deserialize so the wire shape matches
        // scene files exactly: {translation:{x,y,z}, rotation:{x,y,z} Euler radians, scale:{x,y,z}}.
        registerCommand(reg, "set-transform", "set-transform {entity, translation?, rotation?, scale?}",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                auto entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return Err(entity.error());
                }
                const ComponentTraits* row = findByName(ctx.editor.registry, "Transform");
                if (row == nullptr)
                {
                    return Err(std::string{ "Transform component is not registered" });
                }
                if (!row->has(ctx.editor.scene, *entity))
                {
                    return Err(std::string{ "entity has no Transform" });
                }
                // Merge provided fields over the current transform so unspecified
                // fields (e.g. scale) are preserved rather than reset to defaults.
                json body = row->serialize(ctx.editor.scene, *entity);
                if (params.contains("translation")) { body["translation"] = params["translation"]; }
                if (params.contains("rotation")) { body["rotation"] = params["rotation"]; }
                if (params.contains("scale")) { body["scale"] = params["scale"]; }
                auto result = row->deserialize(ctx.editor.scene, *entity, body);
                if (!result)
                {
                    return Err(result.error());
                }
                return entityRef(ctx.editor.scene, *entity);
            });

        // Adds/updates the entity's Material, merging the provided fields over its
        // current value (baseColor as {x,y,z,w}).
        registerCommand(reg, "set-material", "set-material {entity, baseColor?:{x,y,z,w}, albedoTexture?:uuid, unlit?:0|1}",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                auto entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return Err(entity.error());
                }
                const ComponentTraits* row = findByName(ctx.editor.registry, "Material");
                if (row == nullptr)
                {
                    return Err(std::string{ "Material component is not registered" });
                }
                if (!row->has(ctx.editor.scene, *entity))
                {
                    row->addDefault(ctx.editor.scene, *entity);
                }
                json body = row->serialize(ctx.editor.scene, *entity);
                if (params.contains("baseColor")) { body["baseColor"] = params["baseColor"]; }
                if (params.contains("albedoTexture"))
                {
                    // The se CLI passes a bare uuid as a string; coerce it to a number so
                    // the component's value<u64> deserialize doesn't abort (JSON_NOEXCEPTION).
                    const json& a = params["albedoTexture"];
                    if (a.is_string())
                    {
                        const std::string s = a.get<std::string>();
                        u64 id = 0;
                        std::from_chars(s.data(), s.data() + s.size(), id);
                        body["albedoTexture"] = id;
                    }
                    else
                    {
                        body["albedoTexture"] = a;
                    }
                }
                if (params.contains("unlit"))
                {
                    const json& u = params["unlit"];
                    bool unlit = false;
                    if (u.is_number())
                    {
                        unlit = u.get<double>() != 0.0;
                    }
                    else if (u.is_boolean())
                    {
                        unlit = u.get<bool>();
                    }
                    else if (u.is_string())
                    {
                        const std::string s = u.get<std::string>();
                        unlit = !(s == "0" || s == "false" || s == "off");
                    }
                    body["unlit"] = unlit;
                }
                auto result = row->deserialize(ctx.editor.scene, *entity, body);
                if (!result)
                {
                    return Err(result.error());
                }
                return entityRef(ctx.editor.scene, *entity);
            });

        // Sets the directional light (the given entity, else the first one),
        // merging provided fields (direction/color as {x,y,z}) over its current value.
        registerCommand(reg, "set-light", "set-light {entity?, direction?, color?, intensity?, ambient?}",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const ComponentTraits* row = findByName(ctx.editor.registry, "DirectionalLight");
                if (row == nullptr)
                {
                    return Err(std::string{ "DirectionalLight component is not registered" });
                }
                Entity target{ entt::null };
                const json selector = positionalOr(params, "entity", 0);
                if (!selector.is_null())
                {
                    auto resolved = resolveEntity(ctx, params);
                    if (!resolved)
                    {
                        return Err(resolved.error());
                    }
                    target = *resolved;
                }
                else
                {
                    forEach<DirectionalLightComponent>(ctx.editor.scene, [&](Entity entity, DirectionalLightComponent&)
        {
                        if (target.handle == entt::null)
                        {
                            target = entity;
                        }
                    });
                }
                if (target.handle == entt::null || !row->has(ctx.editor.scene, target))
                {
                    return Err(std::string{ "no DirectionalLight to set" });
                }
                json body = row->serialize(ctx.editor.scene, target);
                if (params.contains("direction")) { body["direction"] = params["direction"]; }
                if (params.contains("color")) { body["color"] = params["color"]; }
                if (params.contains("intensity")) { body["intensity"] = params["intensity"]; }
                if (params.contains("ambient")) { body["ambient"] = params["ambient"]; }
                auto result = row->deserialize(ctx.editor.scene, target, body);
                if (!result)
                {
                    return Err(result.error());
                }
                return entityRef(ctx.editor.scene, target);
            });

        registerCommand(reg, "select", "select {entity}",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                auto entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return Err(entity.error());
                }
                setSelection(ctx.editor, *entity);
                return entityRef(ctx.editor.scene, *entity);
            });

        registerCommand(reg, "pick", "pick {u=0.5, v=0.5} — ray-pick at viewport UV (0,0 = top-left)",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const json uParam = positionalOr(params, "u", 0);
                const json vParam = positionalOr(params, "v", 1);
                f32 u = 0.5f;
                f32 v = 0.5f;
                if (uParam.is_number()) { u = static_cast<f32>(uParam.get<double>()); }
                if (vParam.is_number()) { v = static_cast<f32>(vParam.get<double>()); }
                const CameraView cam = editorCameraView(ctx.editor.camera);
                const Entity hit = pickEntity(ctx.editor.scene, ctx.assets, ctx.renderer, cam,
                                              glm::vec2{ u * 2.0f - 1.0f, v * 2.0f - 1.0f });
                setSelection(ctx.editor, hit);
                if (hit.handle == entt::null)
                {
                    return json{ { "hit", false } };
                }
                json result = entityRef(ctx.editor.scene, hit);
                result["hit"] = true;
                return result;
            });

        registerCommand(reg, "inspect", "inspect {entity} — dump all its components as json",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                auto entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return Err(entity.error());
                }
                json components = json::object();
                for (const ComponentTraits& row : ctx.editor.registry.rows)
                {
                    if (row.has(ctx.editor.scene, *entity))
                    {
                        components[row.name] = row.serialize(ctx.editor.scene, *entity);
                    }
                }
                json result = entityRef(ctx.editor.scene, *entity);
                result["components"] = std::move(components);
                return result;
            });

        registerCommand(reg, "focus", "focus {entity} — aim the editor camera at it",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                auto entity = resolveEntity(ctx, params);
                if (!entity)
                {
                    return Err(entity.error());
                }
                if (!hasComponent<TransformComponent>(ctx.editor.scene, *entity))
                {
                    return Err(std::string{ "entity has no Transform" });
                }
                const glm::vec3 target = getComponent<TransformComponent>(ctx.editor.scene, *entity).translation;
                ctx.editor.camera.position = target - editorCameraForward(ctx.editor.camera) * 5.0f;
                return entityRef(ctx.editor.scene, *entity);
            });
    }
}
