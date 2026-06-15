module;

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <string>

module Saffron.Control;

import Saffron.Core;
import Saffron.Scene;
import Saffron.SceneEdit;
import Saffron.Physics;

namespace se
{
    namespace
    {
        auto colliderShapeName(ColliderComponent::Shape shape) -> const char*
        {
            switch (shape)
            {
            case ColliderComponent::Shape::Box:
                return "box";
            case ColliderComponent::Shape::Sphere:
                return "sphere";
            case ColliderComponent::Shape::Capsule:
                return "capsule";
            case ColliderComponent::Shape::ConvexHull:
                return "convexhull";
            case ColliderComponent::Shape::Mesh:
                return "mesh";
            }
            return "box";
        }

        // The rig's ragdoll state for a control reply: live presence/active/mean-weight from the
        // world, plus the authored BonePhysics bone count (informative even with no live ragdoll).
        auto ragdollResultFor(const PhysicsWorld& world, Scene& scene, Entity entity, u64 uuid) -> RagdollResult
        {
            const RagdollState state = ragdollState(world, uuid);
            const i32 bones = hasComponent<BonePhysicsComponent>(scene, entity)
                                  ? static_cast<i32>(getComponent<BonePhysicsComponent>(scene, entity).bones.size())
                                  : 0;
            return RagdollResult{
                .present = state.present, .active = state.active, .bodyWeight = state.bodyWeight, .bones = bones
            };
        }
    }

    void registerPhysicsCommands(CommandRegistry& reg)
    {
        registerCommand<EmptyParams, PhysicsStateResult>(
            reg, "physics-state", "physics-state — summary of the live physics world (active, body + dynamic counts)",
            [](EngineContext& ctx, const EmptyParams&) -> Result<PhysicsStateResult>
            {
                // No world in Edit (it exists only while Playing/Paused) — report inactive,
                // never an error, so the editor can poll it unconditionally.
                if (ctx.physics == nullptr)
                {
                    return PhysicsStateResult{ .active = false, .bodyCount = 0, .dynamicCount = 0 };
                }
                const PhysicsWorldStats stats = physicsWorldStats(*ctx.physics);
                return PhysicsStateResult{ .active = stats.active,
                                           .bodyCount = stats.bodyCount,
                                           .dynamicCount = stats.dynamicCount };
            });

        registerCommand<EmptyParams, PhysicsBodiesResult>(
            reg, "physics-bodies",
            "physics-bodies — every live body's entity, motion, active state, and world position",
            [](EngineContext& ctx, const EmptyParams&) -> Result<PhysicsBodiesResult>
            {
                PhysicsBodiesResult result;
                if (ctx.physics == nullptr)
                {
                    return result;  // Edit: no world, an empty list (the editor polls unconditionally)
                }
                auto motionName = [](MotionType m) -> const char*
                {
                    switch (m)
                    {
                    case MotionType::Static:
                        return "static";
                    case MotionType::Kinematic:
                        return "kinematic";
                    case MotionType::Dynamic:
                        return "dynamic";
                    }
                    return "static";
                };
                for (const PhysicsBodyInfo& body : listPhysicsBodies(*ctx.physics))
                {
                    result.bodies.push_back(PhysicsBodyDto{
                        .entity = WireUuid{ body.entity },
                        .motion = motionName(body.motion),
                        .active = body.active,
                        .position = Vec3{ .x = body.position.x, .y = body.position.y, .z = body.position.z } });
                }
                return result;
            });

        registerCommand<ApplyImpulseParams, ApplyImpulseResult>(
            reg, "apply-impulse",
            "apply-impulse {entity, impulse:{x,y,z}} — push a Dynamic rigidbody (returns its new velocity)",
            [](EngineContext& ctx, const ApplyImpulseParams& params) -> Result<ApplyImpulseResult>
            {
                if (ctx.physics == nullptr)
                {
                    return Err(std::string{ "no physics world — enter play first" });
                }
                auto entity = resolveEntity(ctx, nlohmann::json{ { "entity", params.entity.value } });
                if (!entity)
                {
                    return Err(entity.error());
                }
                Scene& scene = activeScene(ctx.sceneEdit);
                const u64 uuid =
                    hasComponent<IdComponent>(scene, *entity) ? getComponent<IdComponent>(scene, *entity).id.value : 0;
                applyBodyImpulse(*ctx.physics, uuid, glm::vec3(params.impulse.x, params.impulse.y, params.impulse.z));
                const glm::vec3 velocity = bodyLinearVelocity(*ctx.physics, uuid);
                return ApplyImpulseResult{ .velocity = Vec3{ .x = velocity.x, .y = velocity.y, .z = velocity.z } };
            });

        registerCommand<FitColliderParams, FitColliderResult>(
            reg, "fit-collider", "fit-collider {entity} — re-fit a Collider's shape to the entity's mesh AABB",
            [](EngineContext& ctx, const FitColliderParams& params) -> Result<FitColliderResult>
            {
                auto entity = resolveEntity(ctx, nlohmann::json{ { "entity", params.entity.value } });
                if (!entity)
                {
                    return Err(entity.error());
                }
                Scene& scene = activeScene(ctx.sceneEdit);
                if (!hasComponent<ColliderComponent>(scene, *entity))
                {
                    return Err(std::string{ "entity has no Collider" });
                }
                if (!fitColliderToMesh(ctx, *entity))
                {
                    return Err(std::string{ "no resolvable mesh to fit the collider to" });
                }
                ctx.sceneEdit.sceneVersion += 1;
                const ColliderComponent& c = getComponent<ColliderComponent>(scene, *entity);
                u64 uuid = 0;
                if (hasComponent<IdComponent>(scene, *entity))
                {
                    uuid = getComponent<IdComponent>(scene, *entity).id.value;
                }
                return FitColliderResult{ .entity = WireUuid{ uuid },
                                          .shape = colliderShapeName(c.shape),
                                          .halfExtents =
                                              Vec3{ .x = c.halfExtents.x, .y = c.halfExtents.y, .z = c.halfExtents.z },
                                          .offset = Vec3{ .x = c.offset.x, .y = c.offset.y, .z = c.offset.z } };
            });

        registerCommand<DrainContactsParams, DrainContactsResult>(
            reg, "drain-contacts", "drain-contacts {since} — contact/trigger events with seq > since (non-blocking)",
            [](EngineContext& ctx, const DrainContactsParams& params) -> Result<DrainContactsResult>
            {
                DrainContactsResult result;
                result.highWaterSeq = 0;
                result.oldestSeq = 0;
                result.overflowed = false;
                if (ctx.physics == nullptr)
                {
                    return result;  // Edit: no world, an empty drain (the editor polls unconditionally)
                }
                const u64 since = params.since.has_value() ? static_cast<u64>(*params.since) : 0;
                const ContactDrain drain = drainContacts(*ctx.physics, since);
                result.highWaterSeq = static_cast<i64>(drain.highWaterSeq);
                result.oldestSeq = static_cast<i64>(drain.oldestSeq);
                result.overflowed = drain.overflowed;
                for (const ContactEvent& event : drain.events)
                {
                    ContactEventDto dto;
                    dto.seq = static_cast<i64>(event.seq);
                    dto.kind = event.kind == ContactEvent::Kind::Begin ? "begin" : "end";
                    dto.entityA = WireUuid{ event.entityA };
                    dto.entityB = WireUuid{ event.entityB };
                    dto.sensor = event.sensor;
                    dto.point = Vec3{ .x = event.point.x, .y = event.point.y, .z = event.point.z };
                    dto.normal = Vec3{ .x = event.normal.x, .y = event.normal.y, .z = event.normal.z };
                    dto.tick = event.tick;
                    result.events.push_back(dto);
                }
                return result;
            });

        registerCommand<SetKinematicBonesParams, KinematicBonesResult>(
            reg, "set-kinematic-bones",
            "set-kinematic-bones {entity, enabled?} — toggle a rig's kinematic-bone physics",
            [](EngineContext& ctx, const SetKinematicBonesParams& params) -> Result<KinematicBonesResult>
            {
                auto entity = resolveEntity(ctx, nlohmann::json{ { "entity", params.entity.value } });
                if (!entity)
                {
                    return Err(entity.error());
                }
                Scene& scene = activeScene(ctx.sceneEdit);
                // The editor selects a model by its container root; the rig (SkinnedMesh + bones) lives
                // on a descendant — resolve to it so the kinematic bones bind the right entity.
                const Entity rig = animatableDescendant(scene, *entity);
                if (!hasComponent<KinematicBonesComponent>(scene, rig))
                {
                    addComponent<KinematicBonesComponent>(scene, rig);
                }
                auto& bones = getComponent<KinematicBonesComponent>(scene, rig);
                if (params.enabled.has_value())
                {
                    bones.enabled = *params.enabled;
                }
                ctx.sceneEdit.sceneVersion += 1;
                i32 boneCount = 0;
                if (hasComponent<SkinnedMeshComponent>(scene, rig))
                {
                    boneCount = static_cast<i32>(getComponent<SkinnedMeshComponent>(scene, rig).bones.size());
                }
                u64 uuid = 0;
                if (hasComponent<IdComponent>(scene, rig))
                {
                    uuid = getComponent<IdComponent>(scene, rig).id.value;
                }
                return KinematicBonesResult{ .entity = WireUuid{ uuid },
                                             .enabled = bones.enabled,
                                             .boneCount = boneCount };
            });

        registerCommand<MoveCharacterParams, MoveCharacterResult>(
            reg, "move-character",
            "move-character {entity, velocity:{x,y,z}, jump?} — set a character's desired walk velocity",
            [](EngineContext& ctx, const MoveCharacterParams& params) -> Result<MoveCharacterResult>
            {
                auto entity = resolveEntity(ctx, nlohmann::json{ { "entity", params.entity.value } });
                if (!entity)
                {
                    return Err(entity.error());
                }
                Scene& scene = activeScene(ctx.sceneEdit);
                if (!hasComponent<CharacterControllerComponent>(scene, *entity))
                {
                    return Err(std::string{ "entity has no CharacterController" });
                }
                auto& controller = getComponent<CharacterControllerComponent>(scene, *entity);
                // The sweep consumes these on the next physics step (the inert seam); y is gravity's.
                controller.desiredVelocity = glm::vec3(params.velocity.x, 0.0f, params.velocity.z);
                if (params.jump.value_or(false))
                {
                    controller.verticalVelocity = 5.0f;  // a fixed jump impulse
                }
                const glm::vec3 position = worldTranslation(scene, *entity);
                return MoveCharacterResult{ .position = Vec3{ .x = position.x, .y = position.y, .z = position.z },
                                            .onGround = controller.onGround };
            });

        registerCommand<RaycastParams, RaycastResult>(
            reg, "raycast",
            "raycast {origin:{x,y,z}, dir:{x,y,z}, maxDist=1000} — closest physics hit "
            "(entity/point/normal/distance)",
            [](EngineContext& ctx, const RaycastParams& params) -> Result<RaycastResult>
            {
                if (ctx.physics == nullptr)
                {
                    return Err(std::string{ "no physics world — enter play first" });
                }
                const PhysicsRayHit hit =
                    raycastWorld(*ctx.physics, glm::vec3(params.origin.x, params.origin.y, params.origin.z),
                                 glm::vec3(params.dir.x, params.dir.y, params.dir.z), params.maxDist.value_or(1000.0f));
                return RaycastResult{ .hit = hit.hit,
                                      .entity = WireUuid{ hit.entity },
                                      .point = Vec3{ .x = hit.point.x, .y = hit.point.y, .z = hit.point.z },
                                      .normal = Vec3{ .x = hit.normal.x, .y = hit.normal.y, .z = hit.normal.z },
                                      .distance = hit.distance };
            });

        registerCommand<ShapecastParams, RaycastResult>(
            reg, "shapecast",
            "shapecast {origin:{x,y,z}, dir:{x,y,z}, radius, maxDist=1000} — closest sphere-sweep hit",
            [](EngineContext& ctx, const ShapecastParams& params) -> Result<RaycastResult>
            {
                if (ctx.physics == nullptr)
                {
                    return Err(std::string{ "no physics world — enter play first" });
                }
                const PhysicsRayHit hit =
                    sphereCastWorld(*ctx.physics, glm::vec3(params.origin.x, params.origin.y, params.origin.z),
                                    glm::vec3(params.dir.x, params.dir.y, params.dir.z), params.radius,
                                    params.maxDist.value_or(1000.0f));
                return RaycastResult{ .hit = hit.hit,
                                      .entity = WireUuid{ hit.entity },
                                      .point = Vec3{ .x = hit.point.x, .y = hit.point.y, .z = hit.point.z },
                                      .normal = Vec3{ .x = hit.normal.x, .y = hit.normal.y, .z = hit.normal.z },
                                      .distance = hit.distance };
            });

        registerCommand<EnableRagdollParams, RagdollResult>(
            reg, "enable-ragdoll",
            "enable-ragdoll {entity, enabled?} — go limp (true) or restore animation (false) on a rig",
            [](EngineContext& ctx, const EnableRagdollParams& params) -> Result<RagdollResult>
            {
                if (ctx.physics == nullptr)
                {
                    return Err(std::string{ "no physics world — enter play first" });
                }
                auto entity = resolveEntity(ctx, nlohmann::json{ { "entity", params.entity.value } });
                if (!entity)
                {
                    return Err(entity.error());
                }
                Scene& scene = activeScene(ctx.sceneEdit);
                // Selecting the model container root resolves to its rig descendant (SkinnedMesh +
                // BonePhysics), so enable-ragdoll acts on the whole-model selection the editor sends.
                const Entity rig = animatableDescendant(scene, *entity);
                const u64 uuid =
                    hasComponent<IdComponent>(scene, rig) ? getComponent<IdComponent>(scene, rig).id.value : 0;
                if (params.enabled.value_or(true))
                {
                    if (auto enabled = enableRagdoll(*ctx.physics, scene, rig); !enabled)
                    {
                        return Err(enabled.error());
                    }
                }
                else
                {
                    disableRagdoll(*ctx.physics, uuid);
                }
                return ragdollResultFor(*ctx.physics, scene, rig, uuid);
            });

        registerCommand<SetRagdollParams, RagdollResult>(
            reg, "set-ragdoll",
            "set-ragdoll {entity, active?, bodyWeight?, bone?, weight?} — drive a rig's active-ragdoll blend",
            [](EngineContext& ctx, const SetRagdollParams& params) -> Result<RagdollResult>
            {
                if (ctx.physics == nullptr)
                {
                    return Err(std::string{ "no physics world — enter play first" });
                }
                auto entity = resolveEntity(ctx, nlohmann::json{ { "entity", params.entity.value } });
                if (!entity)
                {
                    return Err(entity.error());
                }
                Scene& scene = activeScene(ctx.sceneEdit);
                // Resolve the model root to its rig descendant (the SkinnedMesh + BonePhysics carrier).
                const Entity rig = animatableDescendant(scene, *entity);
                const u64 uuid =
                    hasComponent<IdComponent>(scene, rig) ? getComponent<IdComponent>(scene, rig).id.value : 0;
                // Auto-create the ragdoll on first drive so a hit reaction "just works" without a
                // separate enable-ragdoll round-trip.
                if (!hasRagdoll(*ctx.physics, uuid))
                {
                    if (auto enabled = enableRagdoll(*ctx.physics, scene, rig); !enabled)
                    {
                        return Err(enabled.error());
                    }
                }
                if (auto set = setRagdollBlend(*ctx.physics, uuid, params.active, params.bodyWeight, params.bone,
                                               params.weight);
                    !set)
                {
                    return Err(set.error());
                }
                ctx.sceneEdit.animationVersion += 1;
                return ragdollResultFor(*ctx.physics, scene, rig, uuid);
            });

        registerCommand<GetRagdollParams, RagdollResult>(
            reg, "get-ragdoll", "get-ragdoll {entity} — the rig's ragdoll presence, active flag, and blend weight",
            [](EngineContext& ctx, const GetRagdollParams& params) -> Result<RagdollResult>
            {
                if (ctx.physics == nullptr)
                {
                    return Err(std::string{ "no physics world — enter play first" });
                }
                auto entity = resolveEntity(ctx, nlohmann::json{ { "entity", params.entity.value } });
                if (!entity)
                {
                    return Err(entity.error());
                }
                Scene& scene = activeScene(ctx.sceneEdit);
                const Entity rig = animatableDescendant(scene, *entity);
                const u64 uuid =
                    hasComponent<IdComponent>(scene, rig) ? getComponent<IdComponent>(scene, rig).id.value : 0;
                return ragdollResultFor(*ctx.physics, scene, rig, uuid);
            });
    }
}
