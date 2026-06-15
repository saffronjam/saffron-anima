module;

#include <nlohmann/json.hpp>
#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module Saffron.Control;

import Saffron.Core;
import Saffron.Json;
import Saffron.Geometry;
import Saffron.Rendering;
import Saffron.Scene;
import Saffron.SceneEdit;
import Saffron.Assets;

namespace sa
{
    auto selectorParams(const EntitySelector& selector) -> json
    {
        return json{ { "entity", selector.value } };
    }

    auto resolveEntity(EngineContext& ctx, const EntitySelector& selector) -> Result<Entity>
    {
        return resolveEntity(ctx, selectorParams(selector));
    }

    // An absent, 0, "0", or empty selector means "the scene root" (detach), so a detach
    // never tries to resolve entity 0.
    auto isRootSelector(const EntitySelector& selector) -> bool
    {
        const json& value = selector.value;
        if (value.is_null())
        {
            return true;
        }
        if (value.is_number_unsigned())
        {
            return value.get<u64>() == 0;
        }
        if (value.is_string())
        {
            const std::string text = value.get<std::string>();
            return text.empty() || text == "0";
        }
        return false;
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

    auto gizmoOpDto(GizmoOp op) -> GizmoOpDto
    {
        if (op == GizmoOp::Rotate)
        {
            return GizmoOpDto::Rotate;
        }
        if (op == GizmoOp::Scale)
        {
            return GizmoOpDto::Scale;
        }
        return GizmoOpDto::Translate;
    }

    auto gizmoOpFromDto(GizmoOpDto op) -> GizmoOp
    {
        if (op == GizmoOpDto::Rotate)
        {
            return GizmoOp::Rotate;
        }
        if (op == GizmoOpDto::Scale)
        {
            return GizmoOp::Scale;
        }
        return GizmoOp::Translate;
    }

    auto gizmoSpaceDto(GizmoSpace space) -> GizmoSpaceDto
    {
        if (space == GizmoSpace::Local)
        {
            return GizmoSpaceDto::Local;
        }
        return GizmoSpaceDto::World;
    }

    auto gizmoSpaceFromDto(GizmoSpaceDto space) -> GizmoSpace
    {
        if (space == GizmoSpaceDto::Local)
        {
            return GizmoSpace::Local;
        }
        return GizmoSpace::World;
    }

    auto nativeGizmoHandleName(NativeGizmoHandle handle) -> const char*
    {
        switch (handle)
        {
        case NativeGizmoHandle::X:
            return "x";
        case NativeGizmoHandle::Y:
            return "y";
        case NativeGizmoHandle::Z:
            return "z";
        case NativeGizmoHandle::XY:
            return "xy";
        case NativeGizmoHandle::YZ:
            return "yz";
        case NativeGizmoHandle::XZ:
            return "xz";
        case NativeGizmoHandle::Screen:
            return "screen";
        case NativeGizmoHandle::Uniform:
            return "uniform";
        case NativeGizmoHandle::None:
            return "none";
        }
        return "none";
    }

    auto gizmoStateDto(const SceneEditContext& editor) -> GizmoState
    {
        return GizmoState{ gizmoOpDto(editor.gizmoOp), gizmoSpaceDto(editor.gizmoSpace), editor.preserveChildren };
    }

    auto playStateResultDto(const SceneEditContext& editor) -> PlayStateResult
    {
        return PlayStateResult{ playStateName(editor.playState),           static_cast<i32>(editor.playVersion),
                                static_cast<i32>(editor.sceneVersion),     editor.hadPrimaryCamera,
                                static_cast<i32>(editor.animationVersion), WireUuid{ editor.previewAsset.value } };
    }

    auto normalizeScriptKey(std::string key) -> std::string
    {
        std::ranges::transform(key, key.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return key;
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
        Scene& scene = activeScene(editor);
        constexpr f32 half = 13.0f;  // a touch larger than the drawn glyph for easier clicking
        Entity hit{ entt::null };
        f32 best = half;
        auto test = [&](Entity e)
        {
            if (!hasComponent<TransformComponent>(scene, e) || hasComponent<MeshComponent>(scene, e))
            {
                return;
            }
            const glm::vec3 pos = worldTranslation(scene, e);
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
        forEach<PointLightComponent>(scene, [&](Entity e, PointLightComponent&) { test(e); });
        forEach<SpotLightComponent>(scene,
                                    [&](Entity e, SpotLightComponent&)
                                    {
                                        if (!hasComponent<PointLightComponent>(scene, e))
                                        {
                                            test(e);
                                        }
                                    });
        forEach<CameraComponent>(scene,
                                 [&](Entity e, CameraComponent&)
                                 {
                                     if (!hasComponent<PointLightComponent>(scene, e) &&
                                         !hasComponent<SpotLightComponent>(scene, e))
                                     {
                                         test(e);
                                     }
                                 });
        return hit;
    }

    auto debugOverlaysState(const DebugOverlayOptions& opts) -> DebugOverlaysResult
    {
        return DebugOverlaysResult{ opts.bounds, opts.sceneAabb, opts.lightVolumes, opts.grid, opts.colliders };
    }

    auto fitColliderToMesh(EngineContext& ctx, Entity entity) -> bool
    {
        Scene& scene = activeScene(ctx.sceneEdit);
        if (!hasComponent<ColliderComponent>(scene, entity))
        {
            return false;
        }
        Uuid meshId{ 0 };
        if (hasComponent<MeshComponent>(scene, entity))
        {
            meshId = getComponent<MeshComponent>(scene, entity).mesh;
        }
        else if (hasComponent<SkinnedMeshComponent>(scene, entity))
        {
            meshId = getComponent<SkinnedMeshComponent>(scene, entity).mesh;
        }
        if (meshId.value == 0)
        {
            return false;  // no mesh to size against — keep the collider's defaults
        }
        // Read the AABB from the baked .smesh CPU vertices (the same source cooking uses), so the fit
        // needs no GPU upload and works identically in Edit and headless.
        const Result<Mesh> mesh = loadMeshCpuAsset(ctx.assets, meshId);
        if (!mesh || mesh->vertices.empty())
        {
            return false;
        }
        glm::vec3 lo = mesh->vertices.front().position;
        glm::vec3 hi = lo;
        for (const Vertex& vertex : mesh->vertices)
        {
            lo = glm::min(lo, vertex.position);
            hi = glm::max(hi, vertex.position);
        }
        if (glm::all(glm::lessThanEqual(hi - lo, glm::vec3(0.0f))))
        {
            return false;  // a single degenerate point — nothing to size against (a planar mesh is fine)
        }
        auto& collider = getComponent<ColliderComponent>(scene, entity);
        // The Jolt body is built scale-free (worldTranslation + worldRotation only, physics.cpp), so the
        // entity's WORLD scale must be baked into the fitted extents + offset for the collider to match the
        // scaled visual mesh. Per-axis world scale = the length of each worldMatrix basis column (exact for
        // a TRS transform). Mesh-local AABB, then scaled into the body-local (unscaled) frame.
        const glm::mat4 world = worldMatrix(scene, entity);
        const glm::vec3 worldScale{ glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                                    glm::length(glm::vec3(world[2])) };
        const glm::vec3 half = (hi - lo) * 0.5f * worldScale;
        collider.offset = (lo + hi) * 0.5f * worldScale;
        collider.sourceMesh = meshId;  // cook source for hull/mesh; analytic shapes ignore it
        switch (collider.shape)
        {
        case ColliderComponent::Shape::Box:
        case ColliderComponent::Shape::ConvexHull:
        case ColliderComponent::Shape::Mesh:
            // Hull/mesh fit a fallback box into halfExtents; the cook uses the actual geometry.
            collider.halfExtents = half;
            break;
        case ColliderComponent::Shape::Sphere:
            // Bounding sphere of the box (never smaller than the mesh); radius packed in .x.
            collider.halfExtents = glm::vec3(glm::max(glm::max(half.x, half.y), half.z));
            break;
        case ColliderComponent::Shape::Capsule:
        {
            // Y-up capsule: long axis = Y, radius = the larger of X/Z, half-height excludes the caps.
            const float radius = glm::max(half.x, half.z);
            const float halfHeight = glm::max(0.0f, half.y - radius);
            collider.halfExtents = glm::vec3(radius, halfHeight, radius);
            break;
        }
        }
        return true;
    }

    auto fitBoneCapsules(EngineContext& ctx, Entity rig) -> bool
    {
        Scene& scene = activeScene(ctx.sceneEdit);
        if (!hasComponent<SkinnedMeshComponent>(scene, rig))
        {
            return false;
        }
        const SkinnedMeshComponent& skin = getComponent<SkinnedMeshComponent>(scene, rig);
        const std::size_t count = skin.boneHandles.size();
        if (count == 0)
        {
            return false;
        }
        if (!hasComponent<BonePhysicsComponent>(scene, rig))
        {
            addComponent<BonePhysicsComponent>(scene, rig);
        }
        auto& phys = getComponent<BonePhysicsComponent>(scene, rig);
        phys.bones.resize(count);
        // Rest-pose world positions per joint (Edit reads the authored rest skeleton).
        std::vector<glm::vec3> restPos(count);
        std::vector<u64> uuid(count, 0);
        for (std::size_t i = 0; i < count; i = i + 1)
        {
            const Entity joint{ skin.boneHandles[i] };
            if (valid(scene, joint))
            {
                restPos[i] = worldTranslation(scene, joint);
                if (hasComponent<IdComponent>(scene, joint))
                {
                    uuid[i] = getComponent<IdComponent>(scene, joint).id.value;
                }
            }
        }
        for (std::size_t i = 0; i < count; i = i + 1)
        {
            // Capsule half-height spans toward the farthest child joint; radius a fraction of that.
            float length = 0.0f;
            for (std::size_t child = 0; child < count; child = child + 1)
            {
                const Entity childJoint{ skin.boneHandles[child] };
                if (child == i || !valid(scene, childJoint) || !hasComponent<RelationshipComponent>(scene, childJoint))
                {
                    continue;
                }
                if (getComponent<RelationshipComponent>(scene, childJoint).parent.value == uuid[i] && uuid[i] != 0)
                {
                    length = glm::max(length, glm::length(restPos[child] - restPos[i]));
                }
            }
            const float halfHeight = length > 0.001f ? length * 0.5f : 0.05f;  // leaf default
            const float radius = glm::max(halfHeight * 0.3f, 0.03f);
            phys.bones[i].shapeHalfExtents = glm::vec3(radius, halfHeight, radius);
        }
        return true;
    }

    void registerSceneCommands(CommandRegistry& reg)
    {
        registerCommand<EmptyParams, EntityList>(
            reg, "list-entities", "list all entities",
            [](EngineContext& ctx, const EmptyParams&) -> Result<EntityList>
            {
                EntityList out;
                forEach<IdComponent, NameComponent>(
                    activeScene(ctx.sceneEdit),
                    [&](Entity entity, IdComponent& id, NameComponent& name)
                    {
                        EntityListEntry entry{ WireUuid{ id.id.value }, name.name, std::nullopt, std::nullopt };
                        // Omit parentId for roots (and bone for non-joints) so the
                        // optional fields stay genuinely optional.
                        if (hasComponent<RelationshipComponent>(activeScene(ctx.sceneEdit), entity))
                        {
                            const u64 parent =
                                getComponent<RelationshipComponent>(activeScene(ctx.sceneEdit), entity).parent.value;
                            if (parent != 0)
                            {
                                entry.parentId = WireUuid{ parent };
                            }
                        }
                        if (hasComponent<BoneComponent>(activeScene(ctx.sceneEdit), entity))
                        {
                            entry.bone = true;
                        }
                        out.entities.push_back(std::move(entry));
                    });
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
                Entity entity = createEntity(activeScene(ctx.sceneEdit), params.name);
                ctx.sceneEdit.sceneVersion += 1;
                return entityRefDto(activeScene(ctx.sceneEdit), entity);
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
                const u64 id = getComponent<IdComponent>(activeScene(ctx.sceneEdit), *entity).id.value;
                // destroyEntity takes the whole subtree, so clear the selection when it
                // sits anywhere under the doomed root (walk the selection's ancestry).
                Scene& scene = activeScene(ctx.sceneEdit);
                entt::entity cursor = entt::null;
                if (scene.registry.valid(ctx.sceneEdit.selected.handle))
                {
                    cursor = ctx.sceneEdit.selected.handle;
                }
                while (cursor != entt::null)
                {
                    if (cursor == entity->handle)
                    {
                        setSelection(ctx.sceneEdit, Entity{ entt::null });
                        break;
                    }
                    if (scene.registry.all_of<RelationshipComponent>(cursor))
                    {
                        cursor = scene.registry.get<RelationshipComponent>(cursor).parentHandle;
                    }
                    else
                    {
                        cursor = entt::null;
                    }
                }
                destroyEntity(scene, *entity);
                ctx.sceneEdit.sceneVersion += 1;
                return DestroyEntityResult{ WireUuid{ id } };
            });

        registerCommand<SetParentParams, EntityRef>(
            reg, "set-parent", "set-parent {entity, parent?} — reparent (absent/0 parent detaches to root)",
            [](EngineContext& ctx, const SetParentParams& params) -> Result<EntityRef>
            {
                auto child = resolveEntity(ctx, params.entity);
                if (!child)
                {
                    return Err(child.error());
                }
                Entity newParent{ entt::null };
                if (params.parent && !isRootSelector(*params.parent))
                {
                    auto parent = resolveEntity(ctx, *params.parent);
                    if (!parent)
                    {
                        return Err(parent.error());
                    }
                    newParent = *parent;
                }
                // setParent carries the self/cycle guards and the world-preserving rebase;
                // the selection stays intact (only sceneVersion bumps).
                auto ok = setParent(activeScene(ctx.sceneEdit), *child, newParent);
                if (!ok)
                {
                    return Err(ok.error());
                }
                ctx.sceneEdit.sceneVersion += 1;
                return entityRefDto(activeScene(ctx.sceneEdit), *child);
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
                if (row->has(activeScene(ctx.sceneEdit), *entity))
                {
                    return Err(std::format("entity already has '{}'", params.component));
                }
                row->addDefault(activeScene(ctx.sceneEdit), *entity);
                // Auto-fit a Collider's shape to the entity mesh AABB on add (the locked decision).
                // The registry onAdd hook can't see the asset/renderer handles, so it runs here.
                if (row->name == "Collider")
                {
                    static_cast<void>(fitColliderToMesh(ctx, *entity));
                }
                else if (row->name == "KinematicBones")
                {
                    static_cast<void>(fitBoneCapsules(ctx, *entity));  // auto-fit per-bone capsules
                }
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
                row->remove(activeScene(ctx.sceneEdit), *entity);
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
                auto result = row->deserialize(activeScene(ctx.sceneEdit), *entity, params.json);
                if (!result)
                {
                    return Err(result.error());
                }
                // A raw Relationship write changes the durable parent uuid; relink so the
                // caches follow (a cyclic parent is cut back to root with a warning).
                if (row->name == "Relationship")
                {
                    relinkHierarchy(activeScene(ctx.sceneEdit));
                }
                ctx.sceneEdit.sceneVersion += 1;
                return SetComponentResult{ row->name };
            });

        // Routes through the Transform row's deserialize so the wire shape matches
        // scene files exactly: {translation:{x,y,z}, rotation:{x,y,z} Euler radians, scale:{x,y,z}}.
        registerCommand<SetTransformParams, EntityRef>(
            reg, "set-transform", "set-transform {entity, translation?, rotation?, scale?, smooth?:0|1}",
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
                if (!row->has(activeScene(ctx.sceneEdit), *entity))
                {
                    return Err(std::string{ "entity has no Transform" });
                }
                // With preserve-children, freeze each direct child's world pose so the
                // write below can rebase their locals (the children visually stay put).
                std::vector<std::pair<entt::entity, glm::mat4>> childWorlds;
                if (ctx.sceneEdit.preserveChildren &&
                    hasComponent<RelationshipComponent>(activeScene(ctx.sceneEdit), *entity))
                {
                    for (entt::entity child :
                         getComponent<RelationshipComponent>(activeScene(ctx.sceneEdit), *entity).children)
                    {
                        if (hasComponent<TransformComponent>(activeScene(ctx.sceneEdit), Entity{ child }))
                        {
                            childWorlds.emplace_back(child,
                                                     composeWorldMatrix(activeScene(ctx.sceneEdit), Entity{ child }));
                        }
                    }
                }
                // Smooth edits become per-frame animation targets (stepEditSmoothing)
                // instead of writes — except under preserve-children, where every write
                // must rebase the subtree, so the edit applies exact.
                if (params.smooth.value_or(false) && childWorlds.empty())
                {
                    TransformSmoothTarget& target = transformSmoothEntryFor(ctx.sceneEdit, *entity);
                    if (params.translation)
                    {
                        target.translation = toGlm(*params.translation);
                    }
                    if (params.rotation)
                    {
                        target.rotation = toGlm(*params.rotation);
                    }
                    if (params.scale)
                    {
                        target.scale = toGlm(*params.scale);
                    }
                    ctx.sceneEdit.sceneVersion += 1;
                    return entityRefDto(activeScene(ctx.sceneEdit), *entity);
                }
                cancelTransformSmoothing(ctx.sceneEdit, *entity);
                // Merge provided fields over the current transform so unspecified
                // fields (e.g. scale) are preserved rather than reset to defaults.
                json body = row->serialize(activeScene(ctx.sceneEdit), *entity);
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
                auto result = row->deserialize(activeScene(ctx.sceneEdit), *entity, body);
                if (!result)
                {
                    return Err(result.error());
                }
                if (!childWorlds.empty())
                {
                    const glm::mat4 invWorld = glm::inverse(composeWorldMatrix(activeScene(ctx.sceneEdit), *entity));
                    for (const auto& [child, world] : childWorlds)
                    {
                        setLocalFromMatrix(activeScene(ctx.sceneEdit), Entity{ child }, invWorld * world);
                    }
                }
                ctx.sceneEdit.sceneVersion += 1;
                return entityRefDto(activeScene(ctx.sceneEdit), *entity);
            });

        // Adds/updates the entity's Material, merging the provided fields over its
        // current value (baseColor as {x,y,z,w}).
        registerCommand<SetMaterialParams, EntityRef>(
            reg, "set-material",
            "set-material {entity, baseColor?:{x,y,z,w}, albedoTexture?:uuid, metallicRoughnessTexture?:uuid, "
            "metallic?, roughness?, emissive?:{x,y,z}, emissiveStrength?, unlit?:0|1, slot?, smooth?:0|1}",
            [](EngineContext& ctx, const SetMaterialParams& params) -> Result<EntityRef>
            {
                if (previewing(ctx.sceneEdit))
                {
                    return Err("exit the asset preview first");
                }
                auto entity = resolveEntity(ctx, params.entity);
                if (!entity)
                {
                    return Err(entity.error());
                }
                // Slot path: merge the given fields into one slot of the MaterialSet (direct
                // writes; per-slot smoothing is not animated).
                if (params.slot)
                {
                    const ComponentTraits* setRow = findByName(ctx.sceneEdit.registry, "MaterialSet");
                    if (setRow == nullptr)
                    {
                        return Err(std::string{ "MaterialSet component is not registered" });
                    }
                    if (!setRow->has(activeScene(ctx.sceneEdit), *entity))
                    {
                        return Err(std::string{ "entity has no MaterialSet component" });
                    }
                    json setBody = setRow->serialize(activeScene(ctx.sceneEdit), *entity);
                    if (!setBody.contains("slots") || !setBody["slots"].is_array() ||
                        *params.slot >= setBody["slots"].size())
                    {
                        return Err(std::format("material slot {} out of range", *params.slot));
                    }
                    json& slot = setBody["slots"][*params.slot];
                    if (params.baseColor)
                    {
                        slot["baseColor"] = vec4Json(*params.baseColor);
                    }
                    if (params.albedoTexture)
                    {
                        slot["albedoTexture"] = params.albedoTexture->value;
                    }
                    if (params.metallicRoughnessTexture)
                    {
                        slot["metallicRoughnessTexture"] = params.metallicRoughnessTexture->value;
                    }
                    if (params.metallic)
                    {
                        slot["metallic"] = *params.metallic;
                    }
                    if (params.roughness)
                    {
                        slot["roughness"] = *params.roughness;
                    }
                    if (params.emissive)
                    {
                        slot["emissive"] = vec3Json(*params.emissive);
                    }
                    if (params.emissiveStrength)
                    {
                        slot["emissiveStrength"] = *params.emissiveStrength;
                    }
                    if (params.unlit)
                    {
                        slot["unlit"] = *params.unlit;
                    }
                    if (auto applied = setRow->deserialize(activeScene(ctx.sceneEdit), *entity, setBody); !applied)
                    {
                        return Err(applied.error());
                    }
                    ctx.sceneEdit.sceneVersion += 1;
                    return entityRefDto(activeScene(ctx.sceneEdit), *entity);
                }
                const ComponentTraits* row = findByName(ctx.sceneEdit.registry, "Material");
                if (row == nullptr)
                {
                    return Err(std::string{ "Material component is not registered" });
                }
                if (!row->has(activeScene(ctx.sceneEdit), *entity))
                {
                    row->addDefault(activeScene(ctx.sceneEdit), *entity);
                }
                const bool smooth = params.smooth.value_or(false);
                json body = row->serialize(activeScene(ctx.sceneEdit), *entity);
                // With smooth, numeric fields become per-frame animation targets instead of
                // direct writes (merging only texture/unlit here keeps the JSON round-trip
                // from stomping the component's mid-animation values back).
                if (params.baseColor && !smooth)
                {
                    body["baseColor"] = vec4Json(*params.baseColor);
                }
                if (params.albedoTexture)
                {
                    body["albedoTexture"] = params.albedoTexture->value;
                }
                if (params.metallicRoughnessTexture)
                {
                    body["metallicRoughnessTexture"] = params.metallicRoughnessTexture->value;
                }
                if (params.metallic && !smooth)
                {
                    body["metallic"] = *params.metallic;
                }
                if (params.roughness && !smooth)
                {
                    body["roughness"] = *params.roughness;
                }
                if (params.emissive && !smooth)
                {
                    body["emissive"] = vec3Json(*params.emissive);
                }
                if (params.emissiveStrength && !smooth)
                {
                    body["emissiveStrength"] = *params.emissiveStrength;
                }
                if (params.unlit)
                {
                    body["unlit"] = *params.unlit;
                }
                auto result = row->deserialize(activeScene(ctx.sceneEdit), *entity, body);
                if (!result)
                {
                    return Err(result.error());
                }
                if (smooth)
                {
                    MaterialSmoothTarget& target = materialSmoothEntryFor(ctx.sceneEdit, *entity);
                    if (params.baseColor)
                    {
                        target.baseColor = glm::vec4{ params.baseColor->x, params.baseColor->y, params.baseColor->z,
                                                      params.baseColor->w };
                    }
                    if (params.metallic)
                    {
                        target.metallic = *params.metallic;
                    }
                    if (params.roughness)
                    {
                        target.roughness = *params.roughness;
                    }
                    if (params.emissive)
                    {
                        target.emissive = glm::vec3{ params.emissive->x, params.emissive->y, params.emissive->z };
                    }
                    if (params.emissiveStrength)
                    {
                        target.emissiveStrength = *params.emissiveStrength;
                    }
                }
                else
                {
                    cancelMaterialSmoothing(ctx.sceneEdit, *entity);
                }
                ctx.sceneEdit.sceneVersion += 1;
                return entityRefDto(activeScene(ctx.sceneEdit), *entity);
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
                    forEach<DirectionalLightComponent>(activeScene(ctx.sceneEdit),
                                                       [&](Entity entity, DirectionalLightComponent&)
                                                       {
                                                           if (target.handle == entt::null)
                                                           {
                                                               target = entity;
                                                           }
                                                       });
                }
                if (target.handle == entt::null || !row->has(activeScene(ctx.sceneEdit), target))
                {
                    return Err(std::string{ "no DirectionalLight to set" });
                }
                json body = row->serialize(activeScene(ctx.sceneEdit), target);
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
                auto result = row->deserialize(activeScene(ctx.sceneEdit), target, body);
                if (!result)
                {
                    return Err(result.error());
                }
                ctx.sceneEdit.sceneVersion += 1;
                return entityRefDto(activeScene(ctx.sceneEdit), target);
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
                                                     return entityRefDto(activeScene(ctx.sceneEdit), *entity);
                                                 });

        registerCommand<PickParams, PickResult>(
            reg, "pick", "pick {u=0.5, v=0.5} — pick at viewport UV (0,0 = top-left); tests billboards then mesh AABBs",
            [](EngineContext& ctx, const PickParams& params) -> Result<PickResult>
            {
                const f32 u = params.u.value_or(0.5f);
                const f32 v = params.v.value_or(0.5f);
                // The eye the frame was rendered with, so a click during play ray-casts from
                // the game camera, not the parked fly-cam.
                const CameraView cam = renderCameraView(ctx.sceneEdit);
                const u32 width = viewportWidth(ctx.renderer);
                const u32 height = viewportHeight(ctx.renderer);
                const glm::vec2 mouse{ u * static_cast<f32>(width), v * static_cast<f32>(height) };

                // Billboards first (light/camera glyphs aren't in the mesh AABB set), then the
                // mesh ray-pick. The glyph hit rect mirrors the overlay's ~12px half-size.
                const Entity billboard = pickBillboard(ctx.sceneEdit, cam, width, height, mouse);
                if (billboard.handle != entt::null)
                {
                    setSelection(ctx.sceneEdit, billboard);
                    EntityRef ref = entityRefDto(activeScene(ctx.sceneEdit), billboard);
                    return PickResult{ true, ref.id, ref.name, PickKind::Billboard };
                }

                // pickEntity flips proj[1][1] to match the renderer's clip space, so it
                // expects y-down NDC: v=0 (viewport top) maps to ndc.y=-1.
                const Entity hit = pickEntity(activeScene(ctx.sceneEdit), ctx.assets, ctx.renderer, cam,
                                              glm::vec2{ u * 2.0f - 1.0f, v * 2.0f - 1.0f });
                if (hit.handle == entt::null)
                {
                    setSelection(ctx.sceneEdit, hit);
                    return PickResult{ false, std::nullopt, std::nullopt, std::nullopt };
                }
                // A model instance is a single subtree; a click anywhere in it selects the whole
                // model (its container root), not the bare mesh/bone node the ray hit.
                const Entity selected = modelRootOf(activeScene(ctx.sceneEdit), hit);
                setSelection(ctx.sceneEdit, selected);
                EntityRef ref = entityRefDto(activeScene(ctx.sceneEdit), selected);
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
                    if (row.has(activeScene(ctx.sceneEdit), *entity))
                    {
                        components[row.name] = row.serialize(activeScene(ctx.sceneEdit), *entity);
                    }
                }
                EntityRef ref = entityRefDto(activeScene(ctx.sceneEdit), *entity);
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
                if (!hasComponent<TransformComponent>(activeScene(ctx.sceneEdit), *entity))
                {
                    return Err(std::string{ "entity has no Transform" });
                }
                const glm::vec3 target = worldTranslation(activeScene(ctx.sceneEdit), *entity);
                ctx.sceneEdit.camera.position = target - sceneEditCameraForward(ctx.sceneEdit.camera) * 5.0f;
                return entityRefDto(activeScene(ctx.sceneEdit), *entity);
            });

        registerCommand<EntityParams, WorldTransformResult>(
            reg, "get-world-transform",
            "get-world-transform {entity} — the entity's composed world translation + scale",
            [](EngineContext& ctx, const EntityParams& params) -> Result<WorldTransformResult>
            {
                auto entity = resolveEntity(ctx, params.entity);
                if (!entity)
                {
                    return Err(entity.error());
                }
                Scene& scene = activeScene(ctx.sceneEdit);
                const glm::mat4 world = worldMatrix(scene, *entity);
                const glm::vec3 t{ world[3] };
                const glm::vec3 s{ glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                                   glm::length(glm::vec3(world[2])) };
                return WorldTransformResult{ Vec3{ t.x, t.y, t.z }, Vec3{ s.x, s.y, s.z } };
            });

        registerCommand<EmptyParams, EnvironmentDto>(
            reg, "get-environment", "get-environment — dump the scene sky/environment settings",
            [](EngineContext& ctx, const EmptyParams&) -> Result<EnvironmentDto>
            { return environmentDto(activeScene(ctx.sceneEdit)); });

        // Merges the provided fields over the current environment (same wire shape as the
        // scene file's "environment" block) so unspecified fields are preserved. Pass a typed
        // object via --json for numeric/bool fields; individual named flags also overlay.
        registerCommand<SetEnvironmentParams, EnvironmentDto>(
            reg, "set-environment",
            "set-environment {--json {...} | skyMode?:color|texture|procedural, clearColor?:{x,y,z}, "
            "skyTexture?:uuid, skyIntensity?, skyRotation?, exposure?, visible?:bool, useSkyForAmbient?:bool, "
            "ambientColor?:{x,y,z}, ambientIntensity?}",
            [](EngineContext& ctx, const SetEnvironmentParams& params) -> Result<EnvironmentDto>
            {
                json body = environmentToJson(activeScene(ctx.sceneEdit).environment);
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
                activeScene(ctx.sceneEdit).environment = environmentFromJson(body);
                ctx.sceneEdit.sceneVersion += 1;
                return environmentDto(activeScene(ctx.sceneEdit));
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
                json body = environmentToJson(activeScene(ctx.sceneEdit).environment);
                json atmos = body["atmosphere"];
                if (params.json && params.json->is_object())
                {
                    for (auto it = params.json->begin(); it != params.json->end(); ++it)
                    {
                        atmos[it.key()] = it.value();
                    }
                }
                if (params.enabled)
                {
                    atmos["enabled"] = *params.enabled;
                }
                if (params.planetRadius)
                {
                    atmos["planetRadius"] = *params.planetRadius;
                }
                if (params.atmosphereHeight)
                {
                    atmos["atmosphereHeight"] = *params.atmosphereHeight;
                }
                if (params.rayleighScattering)
                {
                    atmos["rayleighScattering"] = vec3Json(*params.rayleighScattering);
                }
                if (params.rayleighScaleHeight)
                {
                    atmos["rayleighScaleHeight"] = *params.rayleighScaleHeight;
                }
                if (params.mieScattering)
                {
                    atmos["mieScattering"] = *params.mieScattering;
                }
                if (params.mieScaleHeight)
                {
                    atmos["mieScaleHeight"] = *params.mieScaleHeight;
                }
                if (params.mieAnisotropy)
                {
                    atmos["mieAnisotropy"] = *params.mieAnisotropy;
                }
                if (params.ozoneAbsorption)
                {
                    atmos["ozoneAbsorption"] = vec3Json(*params.ozoneAbsorption);
                }
                if (params.sunDiskAngularRadius)
                {
                    atmos["sunDiskAngularRadius"] = *params.sunDiskAngularRadius;
                }
                if (params.sunDiskIntensity)
                {
                    atmos["sunDiskIntensity"] = *params.sunDiskIntensity;
                }
                body["atmosphere"] = atmos;
                activeScene(ctx.sceneEdit).environment = environmentFromJson(body);
                ctx.sceneEdit.sceneVersion += 1;
                return environmentDto(activeScene(ctx.sceneEdit));
            });

        registerCommand<EmptyParams, SelectionResult>(
            reg, "get-selection", "get-selection — the current editor selection + scene/selection version stamps",
            [](EngineContext& ctx, const EmptyParams&) -> Result<SelectionResult>
            {
                SelectionResult out{ static_cast<i32>(ctx.sceneEdit.selectionVersion),
                                     static_cast<i32>(ctx.sceneEdit.sceneVersion),
                                     std::nullopt,
                                     playStateName(ctx.sceneEdit.playState),
                                     static_cast<i32>(ctx.sceneEdit.playVersion),
                                     static_cast<i32>(ctx.sceneEdit.animationVersion) };
                const Entity sel = ctx.sceneEdit.selected;
                if (sel.handle != entt::null && valid(activeScene(ctx.sceneEdit), sel))
                {
                    out.entity = entityRefDto(activeScene(ctx.sceneEdit), sel);
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

        registerCommand<EmptyParams, PlayStateResult>(
            reg, "play", "play — enter play mode (Edit) or resume (Paused)",
            [](EngineContext& ctx, const EmptyParams&) -> Result<PlayStateResult>
            {
                if (previewing(ctx.sceneEdit))
                {
                    return Err("exit the asset preview first");
                }
                if (ctx.sceneEdit.playState == PlayState::Paused)
                {
                    auto resumed = resumePlay(ctx.sceneEdit);
                    if (!resumed)
                    {
                        return Err(resumed.error());
                    }
                }
                else
                {
                    auto entered = enterPlay(ctx.sceneEdit);
                    if (!entered)
                    {
                        return Err(entered.error());
                    }
                }
                return playStateResultDto(ctx.sceneEdit);
            });

        registerCommand<EmptyParams, PlayStateResult>(
            reg, "pause", "pause — freeze the running scene (Playing only)",
            [](EngineContext& ctx, const EmptyParams&) -> Result<PlayStateResult>
            {
                auto paused = pausePlay(ctx.sceneEdit);
                if (!paused)
                {
                    return Err(paused.error());
                }
                return playStateResultDto(ctx.sceneEdit);
            });

        registerCommand<StepParams, PlayStateResult>(
            reg, "step", "step {frames=1} — advance fixed ticks (Paused only)",
            [](EngineContext& ctx, const StepParams& params) -> Result<PlayStateResult>
            {
                auto stepped = stepPlay(ctx.sceneEdit, params.frames.value_or(1));
                if (!stepped)
                {
                    return Err(stepped.error());
                }
                return playStateResultDto(ctx.sceneEdit);
            });

        registerCommand<EmptyParams, PlayStateResult>(
            reg, "stop", "stop — discard the play scene and restore the authored one",
            [](EngineContext& ctx, const EmptyParams&) -> Result<PlayStateResult>
            {
                auto stopped = stopPlay(ctx.sceneEdit);
                if (!stopped)
                {
                    return Err(stopped.error());
                }
                return playStateResultDto(ctx.sceneEdit);
            });

        registerCommand<EmptyParams, PlayStateResult>(
            reg, "get-play-state", "get-play-state — the current play state + version",
            [](EngineContext& ctx, const EmptyParams&) -> Result<PlayStateResult>
            { return playStateResultDto(ctx.sceneEdit); });

        registerCommand<EmptyParams, ScriptStatusResult>(
            reg, "get-script-status", "get-script-status — play state, live script instances, error high-water",
            [](EngineContext& ctx, const EmptyParams&) -> Result<ScriptStatusResult>
            {
                return ScriptStatusResult{ playStateName(ctx.sceneEdit.playState), ctx.sceneEdit.scriptInstanceCount,
                                           ctx.sceneEdit.scriptErrorSeq };
            });

        registerCommand<SetScriptOverrideParams, SetScriptOverrideResult>(
            reg, "set-script-override",
            "set-script-override {entity, slot, name, value} — write one per-instance script field "
            "override (a null value clears it)",
            [](EngineContext& ctx, const SetScriptOverrideParams& params) -> Result<SetScriptOverrideResult>
            {
                auto entity = resolveEntity(ctx, params.entity);
                if (!entity)
                {
                    return Err(entity.error());
                }
                Scene& scene = activeScene(ctx.sceneEdit);
                if (!hasComponent<ScriptComponent>(scene, *entity))
                {
                    return Err(std::string{ "entity has no Script component" });
                }
                ScriptComponent& component = getComponent<ScriptComponent>(scene, *entity);
                if (params.slot < 0 || static_cast<std::size_t>(params.slot) >= component.scripts.size())
                {
                    return Err(std::format("slot {} out of range ({} slot(s))", params.slot, component.scripts.size()));
                }
                ScriptSlot& slot = component.scripts[static_cast<std::size_t>(params.slot)];
                if (!slot.overrides.is_object())
                {
                    slot.overrides = nlohmann::json::object();
                }
                if (params.value.is_null())
                {
                    slot.overrides.erase(params.name);
                }
                else
                {
                    slot.overrides[params.name] = params.value;
                }
                ctx.sceneEdit.sceneVersion += 1;
                return SetScriptOverrideResult{ slot.scriptPath, slot.overrides };
            });

        registerCommand<DrainScriptErrorsParams, DrainScriptErrorsResult>(
            reg, "drain-script-errors", "drain-script-errors {since} — script errors with seq > since (non-blocking)",
            [](EngineContext& ctx, const DrainScriptErrorsParams& params) -> Result<DrainScriptErrorsResult>
            {
                const i64 since = params.since.value_or(0);
                DrainScriptErrorsResult out;
                out.highWaterSeq = ctx.sceneEdit.scriptErrorSeq;
                out.oldestSeq = 0;
                if (!ctx.sceneEdit.scriptErrors.empty())
                {
                    out.oldestSeq = ctx.sceneEdit.scriptErrors.front().seq;
                }
                // The ring drops its oldest entries; a cursor older than what survives
                // means the caller missed events.
                out.overflowed = out.oldestSeq > 0 && since + 1 < out.oldestSeq;
                for (const ScriptError& event : ctx.sceneEdit.scriptErrors)
                {
                    if (event.seq <= since)
                    {
                        continue;
                    }
                    out.events.push_back(ScriptErrorDto{ event.seq, WireUuid{ event.entityUuid }, event.script,
                                                         event.message, event.tick });
                }
                return out;
            });

        registerCommand<DrainScriptLogsParams, DrainScriptLogsResult>(
            reg, "drain-script-logs", "drain-script-logs {since} — sa.log lines with seq > since (non-blocking)",
            [](EngineContext& ctx, const DrainScriptLogsParams& params) -> Result<DrainScriptLogsResult>
            {
                const i64 since = params.since.value_or(0);
                DrainScriptLogsResult out;
                out.highWaterSeq = ctx.sceneEdit.scriptLogSeq;
                out.oldestSeq = 0;
                if (!ctx.sceneEdit.scriptLogs.empty())
                {
                    out.oldestSeq = ctx.sceneEdit.scriptLogs.front().seq;
                }
                out.overflowed = out.oldestSeq > 0 && since + 1 < out.oldestSeq;
                for (const ScriptLog& event : ctx.sceneEdit.scriptLogs)
                {
                    if (event.seq <= since)
                    {
                        continue;
                    }
                    out.events.push_back(ScriptLogDto{ event.seq, WireUuid{ event.entityUuid }, event.message,
                                                       event.epochMs, event.tick });
                }
                return out;
            });

        registerCommand<AddEntityParams, EntityRef>(
            reg, "add-entity",
            "add-entity {preset=empty|cube|model|point-light|spot-light|directional-light|camera|reflection-probe}",
            [](EngineContext& ctx, const AddEntityParams& params) -> Result<EntityRef>
            {
                Scene& scene = activeScene(ctx.sceneEdit);
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
                    // The built-in cube is a model asset like any other: ensure its .smodel exists, then
                    // instantiate it into the scene.
                    auto cubeId = ensureBuiltinModelAsset(ctx.assets, assetPath("models/cube.gltf"));
                    if (!cubeId)
                    {
                        return Err(cubeId.error());
                    }
                    auto root = instantiateModel(scene, ctx.assets, *cubeId, "Cube");
                    if (!root)
                    {
                        return Err(root.error());
                    }
                    e = *root;
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
                Scene& scene = activeScene(ctx.sceneEdit);
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
                getComponent<NameComponent>(activeScene(ctx.sceneEdit), *entity).name = params.name;
                ctx.sceneEdit.sceneVersion += 1;
                return entityRefDto(activeScene(ctx.sceneEdit), *entity);
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
                if (!row->has(activeScene(ctx.sceneEdit), *entity))
                {
                    row->addDefault(activeScene(ctx.sceneEdit), *entity);
                }
                json body = row->serialize(activeScene(ctx.sceneEdit), *entity);
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
                if (params.index.has_value())
                {
                    // Address one element of an array field: an object value merges its keys into
                    // body[field][index] (a partial edit), any other value replaces the element.
                    json& array = body[params.field];
                    if (!array.is_array() || *params.index < 0 ||
                        static_cast<std::size_t>(*params.index) >= array.size())
                    {
                        return Err(std::format("'{}.{}' has no array index {}", params.component, params.field,
                                               *params.index));
                    }
                    json& element = array[static_cast<std::size_t>(*params.index)];
                    if (value.is_object())
                    {
                        for (const auto& [key, sub] : value.items())
                        {
                            element[key] = sub;
                        }
                    }
                    else
                    {
                        element = value;
                    }
                }
                else
                {
                    body[params.field] = value;
                }
                auto result = row->deserialize(activeScene(ctx.sceneEdit), *entity, body);
                if (!result)
                {
                    return Err(result.error());
                }
                // A raw Relationship write changes the durable parent uuid; relink so the
                // caches follow (a cyclic parent is cut back to root with a warning).
                if (row->name == "Relationship")
                {
                    relinkHierarchy(activeScene(ctx.sceneEdit));
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
            reg, "set-gizmo", "set-gizmo {op?:translate|rotate|scale, space?:world|local, preserveChildren?:0|1}",
            [](EngineContext& ctx, const SetGizmoParams& params) -> Result<GizmoState>
            {
                if (ctx.sceneEdit.playState != PlayState::Edit)
                {
                    return Err("gizmo is hidden during play");
                }
                if (params.op)
                {
                    ctx.sceneEdit.gizmoOp = gizmoOpFromDto(*params.op);
                }
                if (params.space)
                {
                    ctx.sceneEdit.gizmoSpace = gizmoSpaceFromDto(*params.space);
                }
                if (params.preserveChildren)
                {
                    ctx.sceneEdit.preserveChildren = *params.preserveChildren;
                }
                return gizmoStateDto(ctx.sceneEdit);
            });

        registerCommand<EmptyParams, DebugOverlaysResult>(
            reg, "get-debug-overlays", "get-debug-overlays — the viewport debug-overlay toggles",
            [](EngineContext& ctx, const EmptyParams&) -> Result<DebugOverlaysResult>
            { return debugOverlaysState(ctx.sceneEdit.debugOverlays); });

        registerCommand<DebugOverlaysParams, DebugOverlaysResult>(
            reg, "set-debug-overlays",
            "set-debug-overlays {bounds?, sceneAabb?, lightVolumes?, grid?, colliders?} — toggle viewport debug "
            "overlays",
            [](EngineContext& ctx, const DebugOverlaysParams& params) -> Result<DebugOverlaysResult>
            {
                DebugOverlayOptions& opts = ctx.sceneEdit.debugOverlays;
                if (params.bounds)
                {
                    opts.bounds = *params.bounds;
                }
                if (params.sceneAabb)
                {
                    opts.sceneAabb = *params.sceneAabb;
                }
                if (params.lightVolumes)
                {
                    opts.lightVolumes = *params.lightVolumes;
                }
                if (params.grid)
                {
                    opts.grid = *params.grid;
                }
                if (params.colliders)
                {
                    opts.colliders = *params.colliders;
                }
                return debugOverlaysState(opts);
            });

        registerCommand<GizmoPointerParams, GizmoPointerResult>(
            reg, "gizmo-pointer",
            "gizmo-pointer {phase:hover|begin|drag|end, x, y} — drive the overlay gizmo (x,y are NDC [-1,1])",
            [](EngineContext& ctx, const GizmoPointerParams& params) -> Result<GizmoPointerResult>
            {
                if (ctx.sceneEdit.playState != PlayState::Edit)
                {
                    return Err("gizmo is hidden during play");
                }
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
                        hasComponent<TransformComponent>(activeScene(ctx.sceneEdit), ctx.sceneEdit.selected))
                    {
                        gizmo.active = gizmo.hovered;
                        gizmo.dragging = true;
                        gizmo.startMouse = mouse;
                        gizmo.dragTarget = mouse;
                        gizmo.dragSmoothed = mouse;
                        gizmo.dragPending = false;
                        gizmo.target = ctx.sceneEdit.selected;
                        snapshotNativeGizmoStart(ctx.sceneEdit, ctx.sceneEdit.selected);
                    }
                }
                else if (phase == GizmoPointerPhase::Drag)
                {
                    // Record the sample only; stepNativeGizmoDrag smooths toward it every
                    // rendered frame, so ~60Hz pointer samples don't staircase on screen.
                    gizmo.dragTarget = mouse;
                    gizmo.dragPending = true;
                }
                else if (phase == GizmoPointerPhase::End)
                {
                    // Land exactly on the release position regardless of smoothing lag.
                    if (gizmo.dragging)
                    {
                        applyNativeGizmoDrag(ctx.sceneEdit, cam, width, height, mouse);
                    }
                    gizmo.dragging = false;
                    gizmo.dragPending = false;
                    gizmo.active = NativeGizmoHandle::None;
                    gizmo.target = Entity{ entt::null };
                }

                NativeGizmoHandle h = gizmo.hovered;
                if (gizmo.dragging)
                {
                    h = gizmo.active;
                }
                const char* handleName = nativeGizmoHandleName(h);
                return GizmoPointerResult{ handleName, gizmo.dragging };
            });

        registerCommand<FlyInputParams, FlyInputResult>(
            reg, "fly-input",
            "fly-input {active, lookDx, lookDy, forward, back, left, right, up, down} — stream editor "
            "fly-cam input (look deltas in pixels accumulate until the next frame)",
            [](EngineContext& ctx, const FlyInputParams& params) -> Result<FlyInputResult>
            {
                SceneEditCameraInput& fly = ctx.sceneEdit.flyInput;
                fly.active = params.active.value_or(false);
                fly.lookDelta += glm::vec2{ params.lookDx.value_or(0.0f), params.lookDy.value_or(0.0f) };
                fly.forward = params.forward.value_or(false);
                fly.back = params.back.value_or(false);
                fly.left = params.left.value_or(false);
                fly.right = params.right.value_or(false);
                fly.up = params.up.value_or(false);
                fly.down = params.down.value_or(false);
                if (!fly.active)
                {
                    fly.lookDelta = glm::vec2{ 0.0f };
                }
                return FlyInputResult{ fly.active };
            });

        registerCommand<ScriptInputParams, ScriptInputResult>(
            reg, "script-input",
            "script-input {keys, mouseButtons?, mouseX?, mouseY?, scroll?} — forward gameplay input to Lua",
            [](EngineContext& ctx, const ScriptInputParams& params) -> Result<ScriptInputResult>
            {
                ScriptInputState& input = ctx.sceneEdit.scriptInput;
                input.held.clear();
                for (const std::string& key : params.keys)
                {
                    const std::string normalized = normalizeScriptKey(key);
                    if (!normalized.empty())
                    {
                        input.held.insert(normalized);
                    }
                }
                if (params.mouseButtons)
                {
                    input.mouseButtons.clear();
                    for (const std::string& button : *params.mouseButtons)
                    {
                        const std::string normalized = normalizeScriptKey(button);
                        if (!normalized.empty())
                        {
                            input.mouseButtons.insert(normalized);
                        }
                    }
                }
                if (params.mouseX)
                {
                    input.mouseX = *params.mouseX;
                }
                if (params.mouseY)
                {
                    input.mouseY = *params.mouseY;
                }
                if (params.scroll)
                {
                    input.scroll = *params.scroll;
                }
                std::vector<std::string> keys(input.held.begin(), input.held.end());
                std::ranges::sort(keys);
                return ScriptInputResult{ std::move(keys) };
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
                forEach<ReflectionProbeComponent>(activeScene(ctx.sceneEdit),
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
