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
    // Server-side billboard hit-test: the nearest light/camera entity whose screen-space
    // glyph contains `mouse` (viewport pixels). Mirrors the overlay's ~12px glyph half-size.
    auto pickBillboard(EditorContext& editor, const CameraView& cam, u32 width, u32 height, glm::vec2 mouse) -> Entity
    {
        if (width == 0 || height == 0)
        {
            return Entity{ entt::null };
        }
        constexpr f32 half = 13.0f;  // a touch larger than the drawn glyph for easier clicking
        Entity hit{ entt::null };
        f32 best = half;
        auto test = [&](Entity e)
        {
            if (!hasComponent<TransformComponent>(editor.scene, e)) { return; }
            const glm::vec3 pos = getComponent<TransformComponent>(editor.scene, e).translation;
            const GizmoProjection p = viewportProject(cam, width, height, pos);
            if (!p.visible) { return; }
            const glm::vec2 d = glm::abs(mouse - p.pixel);
            if (d.x <= half && d.y <= half)
            {
                const f32 dist = glm::length(mouse - p.pixel);
                if (dist <= best) { best = dist; hit = e; }
            }
        };
        forEach<PointLightComponent>(editor.scene, [&](Entity e, PointLightComponent&) { test(e); });
        forEach<SpotLightComponent>(editor.scene, [&](Entity e, SpotLightComponent&) { test(e); });
        forEach<CameraComponent>(editor.scene, [&](Entity e, CameraComponent&) { test(e); });
        return hit;
    }

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
                ctx.editor.sceneVersion += 1;
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
        registerCommand(reg, "set-material",
            "set-material {entity, baseColor?:{x,y,z,w}, albedoTexture?:uuid, metallic?, roughness?, "
            "emissive?:{x,y,z}, emissiveStrength?, unlit?:0|1}",
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
                if (params.contains("metallic")) { body["metallic"] = params["metallic"]; }
                if (params.contains("roughness")) { body["roughness"] = params["roughness"]; }
                if (params.contains("emissive")) { body["emissive"] = params["emissive"]; }
                if (params.contains("emissiveStrength")) { body["emissiveStrength"] = params["emissiveStrength"]; }
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

        registerCommand(reg, "pick",
            "pick {u=0.5, v=0.5} — pick at viewport UV (0,0 = top-left); tests billboards then mesh AABBs",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const json uParam = positionalOr(params, "u", 0);
                const json vParam = positionalOr(params, "v", 1);
                f32 u = 0.5f;
                f32 v = 0.5f;
                if (uParam.is_number()) { u = static_cast<f32>(uParam.get<double>()); }
                if (vParam.is_number()) { v = static_cast<f32>(vParam.get<double>()); }
                const CameraView cam = editorCameraView(ctx.editor.camera);
                const u32 width = viewportWidth(ctx.renderer);
                const u32 height = viewportHeight(ctx.renderer);
                const glm::vec2 mouse{ u * static_cast<f32>(width), v * static_cast<f32>(height) };

                // Billboards first (light/camera glyphs aren't in the mesh AABB set), then the
                // mesh ray-pick. The glyph hit rect mirrors the overlay's ~12px half-size.
                const Entity billboard = pickBillboard(ctx.editor, cam, width, height, mouse);
                if (billboard.handle != entt::null)
                {
                    setSelection(ctx.editor, billboard);
                    json result = entityRef(ctx.editor.scene, billboard);
                    result["hit"] = true;
                    result["kind"] = "billboard";
                    return result;
                }

                const Entity hit = pickEntity(ctx.editor.scene, ctx.assets, ctx.renderer, cam,
                                              glm::vec2{ u * 2.0f - 1.0f, v * 2.0f - 1.0f });
                setSelection(ctx.editor, hit);
                if (hit.handle == entt::null)
                {
                    return json{ { "hit", false } };
                }
                json result = entityRef(ctx.editor.scene, hit);
                result["hit"] = true;
                result["kind"] = "mesh";
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

        registerCommand(reg, "get-environment", "get-environment — dump the scene sky/environment settings",
            [](EngineContext& ctx, const json&) -> Result<json>
            {
                return environmentToJson(ctx.editor.scene.environment);
            });

        // Merges the provided fields over the current environment (same wire shape as the
        // scene file's "environment" block) so unspecified fields are preserved. Pass a typed
        // object via --json for numeric/bool fields; individual named flags also overlay.
        registerCommand(reg, "set-environment",
            "set-environment {--json {...} | skyMode?:color|texture|procedural, clearColor?:{x,y,z}, "
            "skyTexture?:uuid, skyIntensity?, skyRotation?, visible?:bool, useSkyForAmbient?:bool, "
            "ambientColor?:{x,y,z}, ambientIntensity?}",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                json body = environmentToJson(ctx.editor.scene.environment);
                const json blob = positionalOr(params, "json", 0);
                if (blob.is_object())
                {
                    for (auto it = blob.begin(); it != blob.end(); ++it) { body[it.key()] = it.value(); }
                }
                if (params.contains("skyMode")) { body["skyMode"] = params["skyMode"]; }
                if (params.contains("clearColor")) { body["clearColor"] = params["clearColor"]; }
                if (params.contains("skyTexture")) { body["skyTexture"] = params["skyTexture"]; }
                if (params.contains("skyIntensity")) { body["skyIntensity"] = params["skyIntensity"]; }
                if (params.contains("skyRotation")) { body["skyRotation"] = params["skyRotation"]; }
                if (params.contains("exposure")) { body["exposure"] = params["exposure"]; }
                if (params.contains("visible")) { body["visible"] = params["visible"]; }
                if (params.contains("useSkyForAmbient")) { body["useSkyForAmbient"] = params["useSkyForAmbient"]; }
                if (params.contains("ambientColor")) { body["ambientColor"] = params["ambientColor"]; }
                if (params.contains("ambientIntensity")) { body["ambientIntensity"] = params["ambientIntensity"]; }
                ctx.editor.scene.environment = environmentFromJson(body);
                return environmentToJson(ctx.editor.scene.environment);
            });

        registerCommand(reg, "get-selection",
            "get-selection — the current editor selection + scene/selection version stamps",
            [](EngineContext& ctx, const json&) -> Result<json>
            {
                json out;
                out["selectionVersion"] = ctx.editor.selectionVersion;
                out["sceneVersion"] = ctx.editor.sceneVersion;
                const Entity sel = ctx.editor.selected;
                if (sel.handle != entt::null && valid(ctx.editor.scene, sel))
                {
                    out["entity"] = entityRef(ctx.editor.scene, sel);
                }
                else
                {
                    out["entity"] = nullptr;
                }
                return out;
            });

        registerCommand(reg, "deselect", "deselect — clear the editor selection",
            [](EngineContext& ctx, const json&) -> Result<json>
            {
                setSelection(ctx.editor, Entity{ entt::null });
                return json{ { "selectionVersion", ctx.editor.selectionVersion } };
            });

        registerCommand(reg, "add-entity",
            "add-entity {preset=empty|cube|model|point-light|spot-light|directional-light|camera}",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const std::string preset = asString(positionalOr(params, "preset", 0), "empty");
                Scene& scene = ctx.editor.scene;
                Entity e{ entt::null };
                if (preset == "empty")
                {
                    e = createEntity(scene, "Entity");
                }
                else if (preset == "cube" || preset == "model")
                {
                    auto cube = importModel(ctx.assets, ctx.renderer, assetPath("models/cube.gltf"));
                    if (!cube) { return Err(cube.error()); }
                    e = spawnModel(scene, "Cube", *cube);
                }
                else if (preset == "point-light")
                {
                    e = createEntity(scene, "Point Light");
                    addComponent<PointLightComponent>(scene, e);
                    getComponent<TransformComponent>(scene, e).translation = glm::vec3(0.0f, 2.0f, 0.0f);
                }
                else if (preset == "spot-light")
                {
                    e = createEntity(scene, "Spot Light");
                    addComponent<SpotLightComponent>(scene, e);
                    getComponent<TransformComponent>(scene, e).translation = glm::vec3(0.0f, 4.0f, 0.0f);
                }
                else if (preset == "directional-light")
                {
                    e = createEntity(scene, "Directional Light");
                    addComponent<DirectionalLightComponent>(scene, e);
                }
                else if (preset == "camera")
                {
                    e = createEntity(scene, "Camera");
                    addComponent<CameraComponent>(scene, e);
                }
                else
                {
                    return Err(std::format("unknown preset '{}'", preset));
                }
                ctx.editor.sceneVersion += 1;
                setSelection(ctx.editor, e);
                return entityRef(scene, e);
            });

        registerCommand(reg, "copy-entity", "copy-entity {entity} — deep-duplicate it (selects the copy)",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                auto src = resolveEntity(ctx, params);
                if (!src) { return Err(src.error()); }
                Scene& scene = ctx.editor.scene;
                const std::string copyName = getComponent<NameComponent>(scene, *src).name + " (copy)";
                Entity fresh = createEntity(scene, copyName);
                // deserialize add-defaults each missing component and applies fromJson, so we
                // do not call addDefault (which would double-emplace Name/Transform that
                // createEntity already added). Copying the Name component overwrites the
                // "(copy)" suffix, so restore it afterwards.
                for (const ComponentTraits& t : ctx.editor.registry.rows)
                {
                    if (t.has(scene, *src))
                    {
                        static_cast<void>(t.deserialize(scene, fresh, t.serialize(scene, *src)));
                    }
                }
                getComponent<NameComponent>(scene, fresh).name = copyName;
                ctx.editor.sceneVersion += 1;
                setSelection(ctx.editor, fresh);
                return entityRef(scene, fresh);
            });

        registerCommand(reg, "set-component-field",
            "set-component-field {entity, component, field, value} — merge one field "
            "(value may be a uuid string, number, bool, or json object)",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                auto entity = resolveEntity(ctx, params);
                if (!entity) { return Err(entity.error()); }
                const std::string comp = asString(positionalOr(params, "component", 1), "");
                const std::string field = asString(positionalOr(params, "field", 2), "");
                if (comp.empty() || field.empty())
                {
                    return Err(std::string{ "usage: set-component-field {entity, component, field, value}" });
                }
                const ComponentTraits* row = findByName(ctx.editor.registry, comp);
                if (row == nullptr) { return Err(std::format("unknown component '{}'", comp)); }
                if (!row->has(ctx.editor.scene, *entity))
                {
                    row->addDefault(ctx.editor.scene, *entity);
                }
                json body = row->serialize(ctx.editor.scene, *entity);
                json value = positionalOr(params, "value", 3);
                // The CLI passes a bare uuid as a string; coerce a fully-numeric string to u64
                // so a value<u64> deserialize does not abort under JSON_NOEXCEPTION.
                if (value.is_string())
                {
                    const std::string s = value.get<std::string>();
                    u64 n = 0;
                    const std::from_chars_result fc = std::from_chars(s.data(), s.data() + s.size(), n);
                    if (fc.ec == std::errc{} && fc.ptr == s.data() + s.size()) { value = n; }
                }
                body[field] = value;
                auto result = row->deserialize(ctx.editor.scene, *entity, body);
                if (!result) { return Err(result.error()); }
                return json{ { "set", row->name }, { "field", field } };
            });

        registerCommand(reg, "get-camera", "get-camera — the editor fly-camera state",
            [](EngineContext& ctx, const json&) -> Result<json>
            {
                const EditorCamera& c = ctx.editor.camera;
                return json{ { "position", vec3ToJson(c.position) },
                             { "yaw", c.yaw }, { "pitch", c.pitch }, { "fov", c.fov },
                             { "near", c.nearPlane }, { "far", c.farPlane },
                             { "moveSpeed", c.moveSpeed }, { "lookSpeed", c.lookSpeed } };
            });

        registerCommand(reg, "set-camera",
            "set-camera {position?, yaw?, pitch?, fov?, near?, far?, moveSpeed?, lookSpeed?}",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                auto readF32 = [&params](const char* key, f32 fallback) -> f32
                {
                    return params.contains(key) && params[key].is_number()
                        ? static_cast<f32>(params[key].get<double>()) : fallback;
                };
                EditorCamera& c = ctx.editor.camera;
                if (params.contains("position")) { c.position = vec3FromJson(params["position"]); }
                c.yaw = readF32("yaw", c.yaw);
                c.pitch = readF32("pitch", c.pitch);
                c.fov = readF32("fov", c.fov);
                c.nearPlane = readF32("near", c.nearPlane);
                c.farPlane = readF32("far", c.farPlane);
                c.moveSpeed = readF32("moveSpeed", c.moveSpeed);
                c.lookSpeed = readF32("lookSpeed", c.lookSpeed);
                return json{ { "position", vec3ToJson(c.position) }, { "yaw", c.yaw }, { "pitch", c.pitch },
                             { "fov", c.fov }, { "near", c.nearPlane }, { "far", c.farPlane },
                             { "moveSpeed", c.moveSpeed }, { "lookSpeed", c.lookSpeed } };
            });

        registerCommand(reg, "get-gizmo", "get-gizmo — the gizmo op + space",
            [](EngineContext& ctx, const json&) -> Result<json>
            {
                return json{ { "op", gizmoOpName(ctx.editor.gizmoOp) },
                             { "space", gizmoSpaceName(ctx.editor.gizmoSpace) } };
            });

        registerCommand(reg, "set-gizmo", "set-gizmo {op?:translate|rotate|scale, space?:world|local}",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                if (params.contains("op"))
                {
                    ctx.editor.gizmoOp = gizmoOpFromName(asString(params["op"], "translate"));
                }
                if (params.contains("space"))
                {
                    ctx.editor.gizmoSpace = gizmoSpaceFromName(asString(params["space"], "world"));
                }
                return json{ { "op", gizmoOpName(ctx.editor.gizmoOp) },
                             { "space", gizmoSpaceName(ctx.editor.gizmoSpace) } };
            });

        registerCommand(reg, "gizmo-pointer",
            "gizmo-pointer {phase:hover|begin|drag|end, x, y} — drive the overlay gizmo (x,y are NDC [-1,1])",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const std::string phase = asString(positionalOr(params, "phase", 0), "hover");
                const json xParam = positionalOr(params, "x", 1);
                const json yParam = positionalOr(params, "y", 2);
                f32 x = 0.0f;
                f32 y = 0.0f;
                if (xParam.is_number()) { x = static_cast<f32>(xParam.get<double>()); }
                if (yParam.is_number()) { y = static_cast<f32>(yParam.get<double>()); }

                // Keep mode/space in sync with the backend-neutral gizmo state (the single source).
                syncNativeGizmo(ctx.editor);
                const CameraView cam = editorCameraView(ctx.editor.camera);
                const u32 width = viewportWidth(ctx.renderer);
                const u32 height = viewportHeight(ctx.renderer);
                // NDC [-1,1] (top-left = -1,-1) → viewport pixels, matching the SDL pointer path.
                const glm::vec2 mouse{ (x * 0.5f + 0.5f) * static_cast<f32>(width),
                                       (y * 0.5f + 0.5f) * static_cast<f32>(height) };

                NativeGizmoState& gizmo = ctx.editor.nativeGizmo;
                if (phase == "hover")
                {
                    gizmo.hovered = hitNativeGizmo(ctx.editor, cam, width, height, mouse);
                }
                else if (phase == "begin")
                {
                    gizmo.hovered = hitNativeGizmo(ctx.editor, cam, width, height, mouse);
                    if (gizmo.hovered != NativeGizmoHandle::None &&
                        ctx.editor.selected.handle != entt::null &&
                        hasComponent<TransformComponent>(ctx.editor.scene, ctx.editor.selected))
                    {
                        gizmo.active = gizmo.hovered;
                        gizmo.dragging = true;
                        gizmo.startMouse = mouse;
                        gizmo.target = ctx.editor.selected;
                        TransformComponent& transform =
                            getComponent<TransformComponent>(ctx.editor.scene, ctx.editor.selected);
                        gizmo.startTranslation = transform.translation;
                        gizmo.startRotation = transform.rotation;
                        gizmo.startScale = transform.scale;
                    }
                }
                else if (phase == "drag")
                {
                    applyNativeGizmoDrag(ctx.editor, cam, width, height, mouse);
                }
                else if (phase == "end")
                {
                    gizmo.dragging = false;
                    gizmo.active = NativeGizmoHandle::None;
                    gizmo.target = Entity{ entt::null };
                }
                else
                {
                    return Err(std::format("gizmo-pointer: unknown phase '{}'", phase));
                }

                const NativeGizmoHandle h = gizmo.dragging ? gizmo.active : gizmo.hovered;
                const char* handleName =
                    h == NativeGizmoHandle::X       ? "x"
                    : h == NativeGizmoHandle::Y     ? "y"
                    : h == NativeGizmoHandle::Z     ? "z"
                    : h == NativeGizmoHandle::XY    ? "xy"
                    : h == NativeGizmoHandle::YZ    ? "yz"
                    : h == NativeGizmoHandle::XZ    ? "xz"
                    : h == NativeGizmoHandle::Screen  ? "screen"
                    : h == NativeGizmoHandle::Uniform ? "uniform"
                                                      : "none";
                return json{ { "hovered", handleName }, { "dragging", gizmo.dragging } };
            });

        registerCommand(reg, "dump-schema",
            "dump-schema — live component / environment / render-stats shapes for codegen",
            [](EngineContext& ctx, const json&) -> Result<json>
            {
                json components = json::object();
                for (const ComponentTraits& row : ctx.editor.registry.rows)
                {
                    // A fresh scratch entity per component; createEntity already gives it
                    // Name/Transform, so only add the others (addDefault double-emplaces).
                    Entity scratch = createEntity(ctx.editor.scene, "__schema__");
                    if (!row.has(ctx.editor.scene, scratch)) { row.addDefault(ctx.editor.scene, scratch); }
                    components[row.name] = json{ { "removable", row.removable },
                                                 { "fields", row.serialize(ctx.editor.scene, scratch) } };
                    destroyEntity(ctx.editor.scene, scratch);
                }
                json out;
                out["components"] = std::move(components);
                out["environment"] = environmentToJson(ctx.editor.scene.environment);
                out["renderStats"] = renderStatsJson(ctx.renderer);
                return out;
            });
    }
}
