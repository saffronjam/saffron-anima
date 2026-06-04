module;

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <charconv>
#include <format>
#include <optional>
#include <string>

module Saffron.Control;

import Saffron.Core;
import Saffron.Json;
import Saffron.Rendering;
import Saffron.Scene;
import Saffron.SceneEdit;
import Saffron.Assets;

namespace se
{
    auto selectorParams(const EntitySelector& selector) -> json
    {
        return json{ { "entity", selector.value } };
    }

    auto resolveEntity(EngineContext& ctx, const EntitySelector& selector) -> Result<Entity>
    {
        return resolveEntity(ctx, selectorParams(selector));
    }

    auto toGlm(const Vec3& value) -> glm::vec3
    {
        return glm::vec3{ value.x, value.y, value.z };
    }

    auto fromGlm(const glm::vec3& value) -> Vec3
    {
        return Vec3{ value.x, value.y, value.z };
    }

    auto cameraDto(const SceneEditCamera& camera) -> EditorCamera
    {
        return EditorCamera{ fromGlm(camera.position), camera.yaw,      camera.pitch,     camera.fov,
                             camera.nearPlane,         camera.farPlane, camera.moveSpeed, camera.lookSpeed };
    }

    auto environmentDto(Scene& scene) -> EnvironmentDto
    {
        return EnvironmentDto{ environmentToJson(scene.environment) };
    }

    auto gizmoStateDto(const SceneEditContext& editor) -> GizmoState
    {
        return GizmoState{ editor.gizmoOp == GizmoOp::Rotate  ? GizmoOpDto::Rotate
                           : editor.gizmoOp == GizmoOp::Scale ? GizmoOpDto::Scale
                                                              : GizmoOpDto::Translate,
                           editor.gizmoSpace == GizmoSpace::Local ? GizmoSpaceDto::Local : GizmoSpaceDto::World };
    }

    auto vec3Json(const Vec3& value) -> json
    {
        return dtoToJson(value);
    }

    auto vec4Json(const Vec4& value) -> json
    {
        return dtoToJson(value);
    }

    // Server-side billboard hit-test: the nearest meshless light/camera entity whose
    // screen-space glyph contains `mouse` (viewport pixels).
    auto pickBillboard(SceneEditContext& editor, const CameraView& cam, u32 width, u32 height, glm::vec2 mouse)
        -> Entity
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
            if (!hasComponent<TransformComponent>(editor.scene, e) || hasComponent<MeshComponent>(editor.scene, e))
            {
                return;
            }
            const glm::vec3 pos = getComponent<TransformComponent>(editor.scene, e).translation;
            const GizmoProjection p = viewportProject(cam, width, height, pos);
            if (!p.visible)
            {
                return;
            }
            const glm::vec2 d = glm::abs(mouse - p.pixel);
            if (d.x <= half && d.y <= half)
            {
                const f32 dist = glm::length(mouse - p.pixel);
                if (dist <= best)
                {
                    best = dist;
                    hit = e;
                }
            }
        };
        forEach<PointLightComponent>(editor.scene, [&](Entity e, PointLightComponent&) { test(e); });
        forEach<SpotLightComponent>(editor.scene,
                                    [&](Entity e, SpotLightComponent&)
                                    {
                                        if (!hasComponent<PointLightComponent>(editor.scene, e))
                                        {
                                            test(e);
                                        }
                                    });
        forEach<CameraComponent>(editor.scene,
                                 [&](Entity e, CameraComponent&)
                                 {
                                     if (!hasComponent<PointLightComponent>(editor.scene, e) &&
                                         !hasComponent<SpotLightComponent>(editor.scene, e))
                                     {
                                         test(e);
                                     }
                                 });
        return hit;
    }

    void registerSceneCommands(CommandRegistry& reg)
    {
        registerCommand<EmptyParams, EntityList>(
            reg, "list-entities", "list all entities",
            [](EngineContext& ctx, const EmptyParams&) -> Result<EntityList>
            {
                EntityList out;
                forEach<IdComponent, NameComponent>(
                    ctx.sceneEdit.scene, [&](Entity, IdComponent& id, NameComponent& name)
                    { out.entities.push_back(EntityRef{ WireUuid{ id.id.value }, name.name }); });
                return out;
            });

        registerCommand<EmptyParams, ComponentList>(reg, "list-components", "list registered component types",
                                                    [](EngineContext& ctx, const EmptyParams&) -> Result<ComponentList>
                                                    {
                                                        ComponentList out;
                                                        for (const ComponentTraits& traits :
                                                             ctx.sceneEdit.registry.rows)
                                                        {
                                                            out.components.push_back(traits.name);
                                                        }
                                                        return out;
                                                    });

        registerCommand<CreateEntityParams, EntityRef>(
            reg, "create-entity", "create-entity {name}",
            [](EngineContext& ctx, const CreateEntityParams& params) -> Result<EntityRef>
            {
                Entity entity = createEntity(ctx.sceneEdit.scene, params.name);
                ctx.sceneEdit.sceneVersion += 1;
                return entityRefDto(ctx.sceneEdit.scene, entity);
            });

        registerCommand<EntityParams, DestroyEntityResult>(
            reg, "destroy-entity", "destroy-entity {entity}",
            [](EngineContext& ctx, const EntityParams& params) -> Result<DestroyEntityResult>
            {
                auto entity = resolveEntity(ctx, params.entity);
                if (!entity)
                {
                    return Err(entity.error());
                }
                const u64 id = getComponent<IdComponent>(ctx.sceneEdit.scene, *entity).id.value;
                if (ctx.sceneEdit.selected.handle == entity->handle)
                {
                    setSelection(ctx.sceneEdit, Entity{ entt::null });
                }
                destroyEntity(ctx.sceneEdit.scene, *entity);
                ctx.sceneEdit.sceneVersion += 1;
                return DestroyEntityResult{ WireUuid{ id } };
            });

        registerCommand<ComponentParams, AddComponentResult>(
            reg, "add-component", "add-component {entity, component}",
            [](EngineContext& ctx, const ComponentParams& params) -> Result<AddComponentResult>
            {
                auto entity = resolveEntity(ctx, params.entity);
                if (!entity)
                {
                    return Err(entity.error());
                }
                const ComponentTraits* row = findByName(ctx.sceneEdit.registry, params.component);
                if (row == nullptr)
                {
                    return Err(std::format("unknown component '{}'", params.component));
                }
                if (row->has(ctx.sceneEdit.scene, *entity))
                {
                    return Err(std::format("entity already has '{}'", params.component));
                }
                row->addDefault(ctx.sceneEdit.scene, *entity);
                ctx.sceneEdit.sceneVersion += 1;
                return AddComponentResult{ row->name };
            });

        registerCommand<ComponentParams, RemoveComponentResult>(
            reg, "remove-component", "remove-component {entity, component}",
            [](EngineContext& ctx, const ComponentParams& params) -> Result<RemoveComponentResult>
            {
                auto entity = resolveEntity(ctx, params.entity);
                if (!entity)
                {
                    return Err(entity.error());
                }
                const ComponentTraits* row = findByName(ctx.sceneEdit.registry, params.component);
                if (row == nullptr)
                {
                    return Err(std::format("unknown component '{}'", params.component));
                }
                if (!row->removable)
                {
                    return Err(std::format("component '{}' is not removable", row->name));
                }
                row->remove(ctx.sceneEdit.scene, *entity);
                ctx.sceneEdit.sceneVersion += 1;
                return RemoveComponentResult{ row->name };
            });

        // Applies a component's serialized form. Routing through the registry's
        // deserialize keeps the wire shape identical to scene files.
        registerCommand<SetComponentParams, SetComponentResult>(
            reg, "set-component", "set-component {entity, component, json}",
            [](EngineContext& ctx, const SetComponentParams& params) -> Result<SetComponentResult>
            {
                auto entity = resolveEntity(ctx, params.entity);
                if (!entity)
                {
                    return Err(entity.error());
                }
                const ComponentTraits* row = findByName(ctx.sceneEdit.registry, params.component);
                if (row == nullptr)
                {
                    return Err(std::format("unknown component '{}'", params.component));
                }
                auto result = row->deserialize(ctx.sceneEdit.scene, *entity, params.json);
                if (!result)
                {
                    return Err(result.error());
                }
                // A raw Relationship write changes the durable parent uuid; relink so the
                // caches follow (a cyclic parent is cut back to root with a warning).
                if (row->name == "Relationship")
                {
                    relinkHierarchy(ctx.sceneEdit.scene);
                }
                ctx.sceneEdit.sceneVersion += 1;
                return SetComponentResult{ row->name };
            });

        // Routes through the Transform row's deserialize so the wire shape matches
        // scene files exactly: {translation:{x,y,z}, rotation:{x,y,z} Euler radians, scale:{x,y,z}}.
        registerCommand<SetTransformParams, EntityRef>(
            reg, "set-transform", "set-transform {entity, translation?, rotation?, scale?}",
            [](EngineContext& ctx, const SetTransformParams& params) -> Result<EntityRef>
            {
                auto entity = resolveEntity(ctx, params.entity);
                if (!entity)
                {
                    return Err(entity.error());
                }
                const ComponentTraits* row = findByName(ctx.sceneEdit.registry, "Transform");
                if (row == nullptr)
                {
                    return Err(std::string{ "Transform component is not registered" });
                }
                if (!row->has(ctx.sceneEdit.scene, *entity))
                {
                    return Err(std::string{ "entity has no Transform" });
                }
                // Merge provided fields over the current transform so unspecified
                // fields (e.g. scale) are preserved rather than reset to defaults.
                json body = row->serialize(ctx.sceneEdit.scene, *entity);
                if (params.translation)
                {
                    body["translation"] = vec3Json(*params.translation);
                }
                if (params.rotation)
                {
                    body["rotation"] = vec3Json(*params.rotation);
                }
                if (params.scale)
                {
                    body["scale"] = vec3Json(*params.scale);
                }
                auto result = row->deserialize(ctx.sceneEdit.scene, *entity, body);
                if (!result)
                {
                    return Err(result.error());
                }
                ctx.sceneEdit.sceneVersion += 1;
                return entityRefDto(ctx.sceneEdit.scene, *entity);
            });

        // Adds/updates the entity's Material, merging the provided fields over its
        // current value (baseColor as {x,y,z,w}).
        registerCommand<SetMaterialParams, EntityRef>(
            reg, "set-material",
            "set-material {entity, baseColor?:{x,y,z,w}, albedoTexture?:uuid, metallic?, roughness?, "
            "emissive?:{x,y,z}, emissiveStrength?, unlit?:0|1}",
            [](EngineContext& ctx, const SetMaterialParams& params) -> Result<EntityRef>
            {
                auto entity = resolveEntity(ctx, params.entity);
                if (!entity)
                {
                    return Err(entity.error());
                }
                const ComponentTraits* row = findByName(ctx.sceneEdit.registry, "Material");
                if (row == nullptr)
                {
                    return Err(std::string{ "Material component is not registered" });
                }
                if (!row->has(ctx.sceneEdit.scene, *entity))
                {
                    row->addDefault(ctx.sceneEdit.scene, *entity);
                }
                json body = row->serialize(ctx.sceneEdit.scene, *entity);
                if (params.baseColor)
                {
                    body["baseColor"] = vec4Json(*params.baseColor);
                }
                if (params.albedoTexture)
                {
                    body["albedoTexture"] = params.albedoTexture->value;
                }
                if (params.metallic)
                {
                    body["metallic"] = *params.metallic;
                }
                if (params.roughness)
                {
                    body["roughness"] = *params.roughness;
                }
                if (params.emissive)
                {
                    body["emissive"] = vec3Json(*params.emissive);
                }
                if (params.emissiveStrength)
                {
                    body["emissiveStrength"] = *params.emissiveStrength;
                }
                if (params.unlit)
                {
                    body["unlit"] = *params.unlit;
                }
                auto result = row->deserialize(ctx.sceneEdit.scene, *entity, body);
                if (!result)
                {
                    return Err(result.error());
                }
                ctx.sceneEdit.sceneVersion += 1;
                return entityRefDto(ctx.sceneEdit.scene, *entity);
            });

        // Sets the directional light (the given entity, else the first one),
        // merging provided fields (direction/color as {x,y,z}) over its current value.
        registerCommand<SetLightParams, EntityRef>(
            reg, "set-light", "set-light {entity?, direction?, color?, intensity?, ambient?}",
            [](EngineContext& ctx, const SetLightParams& params) -> Result<EntityRef>
            {
                const ComponentTraits* row = findByName(ctx.sceneEdit.registry, "DirectionalLight");
                if (row == nullptr)
                {
                    return Err(std::string{ "DirectionalLight component is not registered" });
                }
                Entity target{ entt::null };
                if (params.entity)
                {
                    auto resolved = resolveEntity(ctx, *params.entity);
                    if (!resolved)
                    {
                        return Err(resolved.error());
                    }
                    target = *resolved;
                }
                else
                {
                    forEach<DirectionalLightComponent>(ctx.sceneEdit.scene,
                                                       [&](Entity entity, DirectionalLightComponent&)
                                                       {
                                                           if (target.handle == entt::null)
                                                           {
                                                               target = entity;
                                                           }
                                                       });
                }
                if (target.handle == entt::null || !row->has(ctx.sceneEdit.scene, target))
                {
                    return Err(std::string{ "no DirectionalLight to set" });
                }
                json body = row->serialize(ctx.sceneEdit.scene, target);
                if (params.direction)
                {
                    body["direction"] = vec3Json(*params.direction);
                }
                if (params.color)
                {
                    body["color"] = vec3Json(*params.color);
                }
                if (params.intensity)
                {
                    body["intensity"] = *params.intensity;
                }
                if (params.ambient)
                {
                    body["ambient"] = *params.ambient;
                }
                auto result = row->deserialize(ctx.sceneEdit.scene, target, body);
                if (!result)
                {
                    return Err(result.error());
                }
                ctx.sceneEdit.sceneVersion += 1;
                return entityRefDto(ctx.sceneEdit.scene, target);
            });

        registerCommand<EntityParams, EntityRef>(reg, "select", "select {entity}",
                                                 [](EngineContext& ctx, const EntityParams& params) -> Result<EntityRef>
                                                 {
                                                     auto entity = resolveEntity(ctx, params.entity);
                                                     if (!entity)
                                                     {
                                                         return Err(entity.error());
                                                     }
                                                     setSelection(ctx.sceneEdit, *entity);
                                                     return entityRefDto(ctx.sceneEdit.scene, *entity);
                                                 });

        registerCommand<PickParams, PickResult>(
            reg, "pick", "pick {u=0.5, v=0.5} — pick at viewport UV (0,0 = top-left); tests billboards then mesh AABBs",
            [](EngineContext& ctx, const PickParams& params) -> Result<PickResult>
            {
                const f32 u = params.u.value_or(0.5f);
                const f32 v = params.v.value_or(0.5f);
                const CameraView cam = sceneEditCameraView(ctx.sceneEdit.camera);
                const u32 width = viewportWidth(ctx.renderer);
                const u32 height = viewportHeight(ctx.renderer);
                const glm::vec2 mouse{ u * static_cast<f32>(width), v * static_cast<f32>(height) };

                // Billboards first (light/camera glyphs aren't in the mesh AABB set), then the
                // mesh ray-pick. The glyph hit rect mirrors the overlay's ~12px half-size.
                const Entity billboard = pickBillboard(ctx.sceneEdit, cam, width, height, mouse);
                if (billboard.handle != entt::null)
                {
                    setSelection(ctx.sceneEdit, billboard);
                    EntityRef ref = entityRefDto(ctx.sceneEdit.scene, billboard);
                    return PickResult{ true, ref.id, ref.name, PickKind::Billboard };
                }

                const Entity hit = pickEntity(ctx.sceneEdit.scene, ctx.assets, ctx.renderer, cam,
                                              glm::vec2{ u * 2.0f - 1.0f, 1.0f - v * 2.0f });
                setSelection(ctx.sceneEdit, hit);
                if (hit.handle == entt::null)
                {
                    return PickResult{ false, std::nullopt, std::nullopt, std::nullopt };
                }
                EntityRef ref = entityRefDto(ctx.sceneEdit.scene, hit);
                return PickResult{ true, ref.id, ref.name, PickKind::Mesh };
            });

        registerCommand<EntityParams, InspectResult>(
            reg, "inspect", "inspect {entity} — dump all its components as json",
            [](EngineContext& ctx, const EntityParams& params) -> Result<InspectResult>
            {
                auto entity = resolveEntity(ctx, params.entity);
                if (!entity)
                {
                    return Err(entity.error());
                }
                json components = json::object();
                for (const ComponentTraits& row : ctx.sceneEdit.registry.rows)
                {
                    if (row.has(ctx.sceneEdit.scene, *entity))
                    {
                        components[row.name] = row.serialize(ctx.sceneEdit.scene, *entity);
                    }
                }
                EntityRef ref = entityRefDto(ctx.sceneEdit.scene, *entity);
                return InspectResult{ ref.id, ref.name, std::move(components) };
            });

        registerCommand<EntityParams, EntityRef>(
            reg, "focus", "focus {entity} — aim the editor camera at it",
            [](EngineContext& ctx, const EntityParams& params) -> Result<EntityRef>
            {
                auto entity = resolveEntity(ctx, params.entity);
                if (!entity)
                {
                    return Err(entity.error());
                }
                if (!hasComponent<TransformComponent>(ctx.sceneEdit.scene, *entity))
                {
                    return Err(std::string{ "entity has no Transform" });
                }
                const glm::vec3 target = getComponent<TransformComponent>(ctx.sceneEdit.scene, *entity).translation;
                ctx.sceneEdit.camera.position = target - sceneEditCameraForward(ctx.sceneEdit.camera) * 5.0f;
                return entityRefDto(ctx.sceneEdit.scene, *entity);
            });

        registerCommand<EmptyParams, EnvironmentDto>(
            reg, "get-environment", "get-environment — dump the scene sky/environment settings",
            [](EngineContext& ctx, const EmptyParams&) -> Result<EnvironmentDto>
            { return environmentDto(ctx.sceneEdit.scene); });

        // Merges the provided fields over the current environment (same wire shape as the
        // scene file's "environment" block) so unspecified fields are preserved. Pass a typed
        // object via --json for numeric/bool fields; individual named flags also overlay.
        registerCommand<SetEnvironmentParams, EnvironmentDto>(
            reg, "set-environment",
            "set-environment {--json {...} | skyMode?:color|texture|procedural, clearColor?:{x,y,z}, "
            "skyTexture?:uuid, skyIntensity?, skyRotation?, visible?:bool, useSkyForAmbient?:bool, "
            "ambientColor?:{x,y,z}, ambientIntensity?}",
            [](EngineContext& ctx, const SetEnvironmentParams& params) -> Result<EnvironmentDto>
            {
                json body = environmentToJson(ctx.sceneEdit.scene.environment);
                if (params.json && params.json->is_object())
                {
                    for (auto it = params.json->begin(); it != params.json->end(); ++it)
                    {
                        body[it.key()] = it.value();
                    }
                }
                if (params.skyMode)
                {
                    body["skyMode"] = *params.skyMode;
                }
                if (params.clearColor)
                {
                    body["clearColor"] = vec3Json(*params.clearColor);
                }
                if (params.skyTexture)
                {
                    body["skyTexture"] = params.skyTexture->value;
                }
                if (params.skyIntensity)
                {
                    body["skyIntensity"] = *params.skyIntensity;
                }
                if (params.skyRotation)
                {
                    body["skyRotation"] = *params.skyRotation;
                }
                if (params.exposure)
                {
                    body["exposure"] = *params.exposure;
                }
                if (params.visible)
                {
                    body["visible"] = *params.visible;
                }
                if (params.useSkyForAmbient)
                {
                    body["useSkyForAmbient"] = *params.useSkyForAmbient;
                }
                if (params.ambientColor)
                {
                    body["ambientColor"] = vec3Json(*params.ambientColor);
                }
                if (params.ambientIntensity)
                {
                    body["ambientIntensity"] = *params.ambientIntensity;
                }
                ctx.sceneEdit.scene.environment = environmentFromJson(body);
                ctx.sceneEdit.sceneVersion += 1;
                return environmentDto(ctx.sceneEdit.scene);
            });

        // Merges atmosphere fields over the current environment's "atmosphere" block (same wire
        // shape as the scene file), so unspecified fields are preserved. enabled flips the
        // procedural-atmosphere envCube source on/off; the re-bake follows in renderScene.
        registerCommand<SetAtmosphereParams, EnvironmentDto>(
            reg, "set-atmosphere",
            "set-atmosphere {--json {...} | enabled?:bool, planetRadius?, atmosphereHeight?, "
            "rayleighScattering?:{x,y,z}, rayleighScaleHeight?, mieScattering?, mieScaleHeight?, "
            "mieAnisotropy?, ozoneAbsorption?:{x,y,z}, sunDiskAngularRadius?, sunDiskIntensity?}",
            [](EngineContext& ctx, const SetAtmosphereParams& params) -> Result<EnvironmentDto>
            {
                json body = environmentToJson(ctx.sceneEdit.scene.environment);
                json atmos = body["atmosphere"];
                if (params.json && params.json->is_object())
                {
                    for (auto it = params.json->begin(); it != params.json->end(); ++it)
                    {
                        atmos[it.key()] = it.value();
                    }
                }
                if (params.enabled) { atmos["enabled"] = *params.enabled; }
                if (params.planetRadius) { atmos["planetRadius"] = *params.planetRadius; }
                if (params.atmosphereHeight) { atmos["atmosphereHeight"] = *params.atmosphereHeight; }
                if (params.rayleighScattering) { atmos["rayleighScattering"] = vec3Json(*params.rayleighScattering); }
                if (params.rayleighScaleHeight) { atmos["rayleighScaleHeight"] = *params.rayleighScaleHeight; }
                if (params.mieScattering) { atmos["mieScattering"] = *params.mieScattering; }
                if (params.mieScaleHeight) { atmos["mieScaleHeight"] = *params.mieScaleHeight; }
                if (params.mieAnisotropy) { atmos["mieAnisotropy"] = *params.mieAnisotropy; }
                if (params.ozoneAbsorption) { atmos["ozoneAbsorption"] = vec3Json(*params.ozoneAbsorption); }
                if (params.sunDiskAngularRadius) { atmos["sunDiskAngularRadius"] = *params.sunDiskAngularRadius; }
                if (params.sunDiskIntensity) { atmos["sunDiskIntensity"] = *params.sunDiskIntensity; }
                body["atmosphere"] = atmos;
                ctx.sceneEdit.scene.environment = environmentFromJson(body);
                ctx.sceneEdit.sceneVersion += 1;
                return environmentDto(ctx.sceneEdit.scene);
            });

        registerCommand<EmptyParams, SelectionResult>(
            reg, "get-selection", "get-selection — the current editor selection + scene/selection version stamps",
            [](EngineContext& ctx, const EmptyParams&) -> Result<SelectionResult>
            {
                SelectionResult out{ static_cast<i32>(ctx.sceneEdit.selectionVersion),
                                     static_cast<i32>(ctx.sceneEdit.sceneVersion), std::nullopt };
                const Entity sel = ctx.sceneEdit.selected;
                if (sel.handle != entt::null && valid(ctx.sceneEdit.scene, sel))
                {
                    out.entity = entityRefDto(ctx.sceneEdit.scene, sel);
                }
                return out;
            });

        registerCommand<EmptyParams, DeselectResult>(
            reg, "deselect", "deselect — clear the editor selection",
            [](EngineContext& ctx, const EmptyParams&) -> Result<DeselectResult>
            {
                setSelection(ctx.sceneEdit, Entity{ entt::null });
                return DeselectResult{ static_cast<i32>(ctx.sceneEdit.selectionVersion) };
            });

        registerCommand<AddEntityParams, EntityRef>(
            reg, "add-entity",
            "add-entity {preset=empty|cube|model|point-light|spot-light|directional-light|camera|reflection-probe}",
            [](EngineContext& ctx, const AddEntityParams& params) -> Result<EntityRef>
            {
                Scene& scene = ctx.sceneEdit.scene;
                Entity e{ entt::null };
                const AddEntityPreset preset = params.preset.value_or(AddEntityPreset::Empty);
                if (preset == AddEntityPreset::Empty)
                {
                    e = createEntity(scene, "Entity");
                }
                else if (preset == AddEntityPreset::Cube || preset == AddEntityPreset::Model)
                {
                    if (!ctx.sceneEdit.projectLoaded)
                    {
                        return Err(std::string{ "no project loaded" });
                    }
                    auto cube = importModel(ctx.assets, ctx.renderer, assetPath("models/cube.gltf"));
                    if (!cube)
                    {
                        return Err(cube.error());
                    }
                    e = spawnModel(scene, "Cube", *cube);
                }
                else if (preset == AddEntityPreset::PointLight)
                {
                    e = createEntity(scene, "Point Light");
                    addComponent<PointLightComponent>(scene, e);
                    getComponent<TransformComponent>(scene, e).translation = glm::vec3(0.0f, 2.0f, 0.0f);
                }
                else if (preset == AddEntityPreset::SpotLight)
                {
                    e = createEntity(scene, "Spot Light");
                    addComponent<SpotLightComponent>(scene, e);
                    getComponent<TransformComponent>(scene, e).translation = glm::vec3(0.0f, 4.0f, 0.0f);
                }
                else if (preset == AddEntityPreset::DirectionalLight)
                {
                    e = createEntity(scene, "Directional Light");
                    addComponent<DirectionalLightComponent>(scene, e);
                }
                else if (preset == AddEntityPreset::Camera)
                {
                    e = createEntity(scene, "Camera");
                    addComponent<CameraComponent>(scene, e);
                }
                else if (preset == AddEntityPreset::ReflectionProbe)
                {
                    e = createEntity(scene, "Reflection Probe");
                    addComponent<ReflectionProbeComponent>(scene, e);
                    getComponent<TransformComponent>(scene, e).translation = glm::vec3(0.0f, 2.0f, 0.0f);
                }
                ctx.sceneEdit.sceneVersion += 1;
                setSelection(ctx.sceneEdit, e);
                return entityRefDto(scene, e);
            });

        registerCommand<EntityParams, EntityRef>(
            reg, "copy-entity", "copy-entity {entity} — deep-duplicate it (selects the copy)",
            [](EngineContext& ctx, const EntityParams& params) -> Result<EntityRef>
            {
                auto src = resolveEntity(ctx, params.entity);
                if (!src)
                {
                    return Err(src.error());
                }
                Scene& scene = ctx.sceneEdit.scene;
                const std::string copyName = getComponent<NameComponent>(scene, *src).name + " (copy)";
                Entity fresh = createEntity(scene, copyName);
                // deserialize add-defaults each missing component and applies fromJson, so we
                // do not call addDefault (which would double-emplace Name/Transform that
                // createEntity already added). Copying the Name component overwrites the
                // "(copy)" suffix, so restore it afterwards.
                for (const ComponentTraits& t : ctx.sceneEdit.registry.rows)
                {
                    if (t.has(scene, *src))
                    {
                        static_cast<void>(t.deserialize(scene, fresh, t.serialize(scene, *src)));
                    }
                }
                getComponent<NameComponent>(scene, fresh).name = copyName;
                // The round-trip duplicated the source's parent uuid (the copy joins the
                // source's parent as a sibling); relink so the copy lands in its parent's
                // children cache.
                relinkHierarchy(scene);
                ctx.sceneEdit.sceneVersion += 1;
                setSelection(ctx.sceneEdit, fresh);
                return entityRefDto(scene, fresh);
            });

        registerCommand<RenameEntityParams, EntityRef>(
            reg, "rename-entity", "rename-entity {entity, name} — set its Name component",
            [](EngineContext& ctx, const RenameEntityParams& params) -> Result<EntityRef>
            {
                auto entity = resolveEntity(ctx, params.entity);
                if (!entity)
                {
                    return Err(entity.error());
                }
                if (params.name.empty())
                {
                    return Err(std::string{ "usage: rename-entity {entity, name}" });
                }
                getComponent<NameComponent>(ctx.sceneEdit.scene, *entity).name = params.name;
                ctx.sceneEdit.sceneVersion += 1;
                return entityRefDto(ctx.sceneEdit.scene, *entity);
            });

        registerCommand<SetComponentFieldParams, SetComponentFieldResult>(
            reg, "set-component-field",
            "set-component-field {entity, component, field, value} — merge one field "
            "(value may be a uuid string, number, bool, or json object)",
            [](EngineContext& ctx, const SetComponentFieldParams& params) -> Result<SetComponentFieldResult>
            {
                auto entity = resolveEntity(ctx, params.entity);
                if (!entity)
                {
                    return Err(entity.error());
                }
                if (params.component.empty() || params.field.empty())
                {
                    return Err(std::string{ "usage: set-component-field {entity, component, field, value}" });
                }
                const ComponentTraits* row = findByName(ctx.sceneEdit.registry, params.component);
                if (row == nullptr)
                {
                    return Err(std::format("unknown component '{}'", params.component));
                }
                if (!row->has(ctx.sceneEdit.scene, *entity))
                {
                    row->addDefault(ctx.sceneEdit.scene, *entity);
                }
                json body = row->serialize(ctx.sceneEdit.scene, *entity);
                json value = params.value;
                // The CLI passes every value as a string; a fully-numeric one becomes a u64 so
                // numeric/id fields land as numbers, while non-numeric strings pass through.
                if (value.is_string())
                {
                    const std::string s = value.get<std::string>();
                    u64 n = 0;
                    const std::from_chars_result fc = std::from_chars(s.data(), s.data() + s.size(), n);
                    if (fc.ec == std::errc{} && fc.ptr == s.data() + s.size())
                    {
                        value = n;
                    }
                }
                body[params.field] = value;
                auto result = row->deserialize(ctx.sceneEdit.scene, *entity, body);
                if (!result)
                {
                    return Err(result.error());
                }
                // A raw Relationship write changes the durable parent uuid; relink so the
                // caches follow (a cyclic parent is cut back to root with a warning).
                if (row->name == "Relationship")
                {
                    relinkHierarchy(ctx.sceneEdit.scene);
                }
                ctx.sceneEdit.sceneVersion += 1;
                return SetComponentFieldResult{ row->name, params.field };
            });

        registerCommand<EmptyParams, EditorCamera>(reg, "get-camera", "get-camera — the editor fly-camera state",
                                                   [](EngineContext& ctx, const EmptyParams&) -> Result<EditorCamera>
                                                   { return cameraDto(ctx.sceneEdit.camera); });

        registerCommand<SetCameraParams, EditorCamera>(
            reg, "set-camera", "set-camera {position?, yaw?, pitch?, fov?, near?, far?, moveSpeed?, lookSpeed?}",
            [](EngineContext& ctx, const SetCameraParams& params) -> Result<EditorCamera>
            {
                SceneEditCamera& c = ctx.sceneEdit.camera;
                if (params.position)
                {
                    c.position = toGlm(*params.position);
                }
                if (params.yaw)
                {
                    c.yaw = *params.yaw;
                }
                if (params.pitch)
                {
                    c.pitch = *params.pitch;
                }
                if (params.fov)
                {
                    c.fov = *params.fov;
                }
                if (params.near)
                {
                    c.nearPlane = *params.near;
                }
                if (params.far)
                {
                    c.farPlane = *params.far;
                }
                if (params.moveSpeed)
                {
                    c.moveSpeed = *params.moveSpeed;
                }
                if (params.lookSpeed)
                {
                    c.lookSpeed = *params.lookSpeed;
                }
                return cameraDto(c);
            });

        registerCommand<EmptyParams, GizmoState>(reg, "get-gizmo", "get-gizmo — the gizmo op + space",
                                                 [](EngineContext& ctx, const EmptyParams&) -> Result<GizmoState>
                                                 { return gizmoStateDto(ctx.sceneEdit); });

        registerCommand<SetGizmoParams, GizmoState>(
            reg, "set-gizmo", "set-gizmo {op?:translate|rotate|scale, space?:world|local}",
            [](EngineContext& ctx, const SetGizmoParams& params) -> Result<GizmoState>
            {
                if (params.op)
                {
                    ctx.sceneEdit.gizmoOp = *params.op == GizmoOpDto::Rotate  ? GizmoOp::Rotate
                                            : *params.op == GizmoOpDto::Scale ? GizmoOp::Scale
                                                                              : GizmoOp::Translate;
                }
                if (params.space)
                {
                    ctx.sceneEdit.gizmoSpace =
                        *params.space == GizmoSpaceDto::Local ? GizmoSpace::Local : GizmoSpace::World;
                }
                return gizmoStateDto(ctx.sceneEdit);
            });

        registerCommand<GizmoPointerParams, GizmoPointerResult>(
            reg, "gizmo-pointer",
            "gizmo-pointer {phase:hover|begin|drag|end, x, y} — drive the overlay gizmo (x,y are NDC [-1,1])",
            [](EngineContext& ctx, const GizmoPointerParams& params) -> Result<GizmoPointerResult>
            {
                // Keep mode/space in sync with the backend-neutral gizmo state (the single source).
                syncNativeGizmo(ctx.sceneEdit);
                const CameraView cam = sceneEditCameraView(ctx.sceneEdit.camera);
                const u32 width = viewportWidth(ctx.renderer);
                const u32 height = viewportHeight(ctx.renderer);
                // NDC [-1,1] (top-left = -1,-1) → viewport pixels, matching the SDL pointer path.
                const f32 x = params.x.value_or(0.0f);
                const f32 y = params.y.value_or(0.0f);
                const glm::vec2 mouse{ (x * 0.5f + 0.5f) * static_cast<f32>(width),
                                       (y * 0.5f + 0.5f) * static_cast<f32>(height) };

                NativeGizmoState& gizmo = ctx.sceneEdit.nativeGizmo;
                const GizmoPointerPhase phase = params.phase.value_or(GizmoPointerPhase::Hover);
                if (phase == GizmoPointerPhase::Hover)
                {
                    gizmo.hovered = hitNativeGizmo(ctx.sceneEdit, cam, width, height, mouse);
                }
                else if (phase == GizmoPointerPhase::Begin)
                {
                    gizmo.hovered = hitNativeGizmo(ctx.sceneEdit, cam, width, height, mouse);
                    if (gizmo.hovered != NativeGizmoHandle::None && ctx.sceneEdit.selected.handle != entt::null &&
                        hasComponent<TransformComponent>(ctx.sceneEdit.scene, ctx.sceneEdit.selected))
                    {
                        gizmo.active = gizmo.hovered;
                        gizmo.dragging = true;
                        gizmo.startMouse = mouse;
                        gizmo.target = ctx.sceneEdit.selected;
                        TransformComponent& transform =
                            getComponent<TransformComponent>(ctx.sceneEdit.scene, ctx.sceneEdit.selected);
                        gizmo.startTranslation = transform.translation;
                        gizmo.startRotation = transform.rotation;
                        gizmo.startScale = transform.scale;
                    }
                }
                else if (phase == GizmoPointerPhase::Drag)
                {
                    applyNativeGizmoDrag(ctx.sceneEdit, cam, width, height, mouse);
                }
                else if (phase == GizmoPointerPhase::End)
                {
                    gizmo.dragging = false;
                    gizmo.active = NativeGizmoHandle::None;
                    gizmo.target = Entity{ entt::null };
                }

                const NativeGizmoHandle h = gizmo.dragging ? gizmo.active : gizmo.hovered;
                const char* handleName = h == NativeGizmoHandle::X         ? "x"
                                         : h == NativeGizmoHandle::Y       ? "y"
                                         : h == NativeGizmoHandle::Z       ? "z"
                                         : h == NativeGizmoHandle::XY      ? "xy"
                                         : h == NativeGizmoHandle::YZ      ? "yz"
                                         : h == NativeGizmoHandle::XZ      ? "xz"
                                         : h == NativeGizmoHandle::Screen  ? "screen"
                                         : h == NativeGizmoHandle::Uniform ? "uniform"
                                                                           : "none";
                return GizmoPointerResult{ handleName, gizmo.dragging };
            });

        registerCommand<SetProbesParams, SetProbesResult>(
            reg, "set-probes", "set-probes {0|1} — toggle reflection-probe specular sampling",
            [](EngineContext& ctx, const SetProbesParams& params) -> Result<SetProbesResult>
            {
                setReflectionProbes(ctx.renderer, params.enabled.value_or(true));
                return SetProbesResult{ reflectionProbesEnabled(ctx.renderer) };
            });

        registerCommand<EmptyParams, RecaptureProbesResult>(
            reg, "recapture-probes", "recapture-probes — mark every reflection probe dirty (forces re-capture)",
            [](EngineContext& ctx, const EmptyParams&) -> Result<RecaptureProbesResult>
            {
                u32 count = 0;
                forEach<ReflectionProbeComponent>(ctx.sceneEdit.scene,
                                                  [&](Entity, ReflectionProbeComponent& probe)
                                                  {
                                                      probe.dirty = true;
                                                      count = count + 1;
                                                  });
                return RecaptureProbesResult{ count };
            });

        registerCommand<EmptyParams, ListProbesResult>(
            reg, "list-probes", "list-probes — captured reflection probes (origin/radius/intensity/valid)",
            [](EngineContext& ctx, const EmptyParams&) -> Result<ListProbesResult>
            {
                ListProbesResult out{ reflectionProbesEnabled(ctx.renderer), 0, {} };
                const ReflectionProbes& refl = ctx.renderer.reflection;
                out.count = refl.count;
                for (u32 i = 0; i < refl.count; i = i + 1)
                {
                    const ReflectionProbe& probe = refl.probes[i];
                    out.probes.push_back(ProbeRef{ i, WireUuid{ probe.entity }, fromGlm(probe.origin),
                                                   probe.influenceRadius, probe.intensity, probe.boxProjection,
                                                   probe.valid, probe.dirty });
                }
                return out;
            });
    }
}
