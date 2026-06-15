module;

// Jolt's Core.h pulls <utility>/<cmath>/<algorithm>/<cstdint> but not <type_traits>
// (it relies on transitive includes libc++'s granular headers don't provide in a
// module TU), so its static_assert(std::is_trivial<…>) and friends need these first.
#include <cstddef>
#include <cstring>
#include <limits>
#include <type_traits>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/SubShapeIDPair.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Skeleton/Skeleton.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

module Saffron.Physics;

import Saffron.Core;
import Saffron.Geometry;
import Saffron.Scene;

namespace se
{
    namespace
    {
        // Broad-phase layers (Jolt's coarse AABB tier): everything non-moving in one,
        // everything that moves in the other. v1 keeps it to two — more is a perf
        // micro-opt not worth the bootstrap complexity.
        namespace BroadPhase
        {
            constexpr JPH::BroadPhaseLayer NonMoving{ 0 };
            constexpr JPH::BroadPhaseLayer Moving{ 1 };
            constexpr JPH::uint Count = 2;
        }

        // Only Static-layer bodies live in the non-moving broad phase; every other object layer
        // (Moving/Character/Debris/Sensor) is in the moving one, so a sensor over a static floor
        // still pairs through the moving tier.
        auto broadPhaseFor(ObjectLayer layer) -> JPH::BroadPhaseLayer
        {
            return layer == ObjectLayer::Static ? BroadPhase::NonMoving : BroadPhase::Moving;
        }

        /// Object layer -> broad-phase layer (the coarse tier Jolt buckets AABBs into).
        class BroadPhaseLayerImpl final : public JPH::BroadPhaseLayerInterface
        {
          public:
            auto GetNumBroadPhaseLayers() const -> JPH::uint override
            {
                return BroadPhase::Count;
            }

            auto GetBroadPhaseLayer(JPH::ObjectLayer layer) const -> JPH::BroadPhaseLayer override
            {
                return broadPhaseFor(static_cast<ObjectLayer>(layer));
            }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
            auto GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const -> const char* override
            {
                return layer == BroadPhase::NonMoving ? "NonMoving" : "Moving";
            }
#endif
        };

        /// "May an object in this layer collide with this broad-phase layer?" (the coarse cull).
        class ObjectVsBroadPhaseImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
        {
          public:
            auto ShouldCollide(JPH::ObjectLayer obj, JPH::BroadPhaseLayer bp) const -> bool override
            {
                // Static objects only need to test the moving broad phase (static-vs-static never
                // collides); every moving layer tests both tiers.
                return static_cast<ObjectLayer>(obj) != ObjectLayer::Static || bp == BroadPhase::Moving;
            }
        };

        /// "May these two object layers collide?" — defers to the v1 collision matrix.
        class ObjectLayerPairImpl final : public JPH::ObjectLayerPairFilter
        {
          public:
            auto ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const -> bool override
            {
                return layersCollide(static_cast<ObjectLayer>(a), static_cast<ObjectLayer>(b));
            }
        };

        void joltTrace(const char* format, ...)
        {
            std::array<char, 1024> buffer{};
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer.data(), buffer.size(), format, args);
            va_end(args);
            logInfo(std::string{ "[jolt] " } + buffer.data());
        }

#ifdef JPH_ENABLE_ASSERTS
        auto joltAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line) -> bool
        {
            logError(std::string{ "[jolt assert] " } + file + ":" + std::to_string(line) + ": " + expression +
                     (message != nullptr ? std::string{ " — " } + message : std::string{}));
            return true;  // breakpoint
        }
#endif

        // glm <-> Jolt at the boundary; glm::quat is (w,x,y,z), Jolt's Quat is (x,y,z,w).
        auto toJolt(glm::vec3 v) -> JPH::Vec3
        {
            return JPH::Vec3(v.x, v.y, v.z);
        }
        auto toJolt(const glm::quat& q) -> JPH::Quat
        {
            return JPH::Quat(q.x, q.y, q.z, q.w);
        }
        auto fromJolt(JPH::Vec3Arg v) -> glm::vec3
        {
            return glm::vec3(v.GetX(), v.GetY(), v.GetZ());
        }
        auto fromJolt(JPH::QuatArg q) -> glm::quat
        {
            return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
        }

        // An entity's fresh world position + rotation (scale divided out). Composes the matrix rather
        // than reading the cached WorldTransformComponent, which is one frame stale during simTick
        // (renderScene refreshes it later). The single most likely source of a one-frame follow lag.
        auto worldPose(Scene& scene, Entity entity) -> std::pair<glm::vec3, glm::quat>
        {
            const glm::mat4 world = composeWorldMatrix(scene, entity);
            const glm::vec3 position(world[3]);
            glm::vec3 scale{ glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])),
                             glm::length(glm::vec3(world[2])) };
            scale = glm::max(scale, glm::vec3(1e-8f));
            const glm::mat3 rotation{ glm::vec3(world[0]) / scale.x, glm::vec3(world[1]) / scale.y,
                                      glm::vec3(world[2]) / scale.z };
            return { position, glm::quat_cast(rotation) };
        }

        auto fromJoltMat(JPH::RMat44Arg matrix) -> glm::mat4
        {
            glm::mat4 out{ 1.0f };
            for (int column = 0; column < 4; column = column + 1)
            {
                const JPH::Vec4 col = matrix.GetColumn4(column);
                out[column] = glm::vec4(col.GetX(), col.GetY(), col.GetZ(), col.GetW());
            }
            return out;
        }

        // A position motor from the bone's PD gains: drive* feed a frequency/damping spring + torque
        // limit, with sensible defaults when authored ~0 (a fresh ragdoll motors gently, not rigidly).
        // Inert until DriveToPoseUsingMotors sets the motor state to Position (active ragdoll only).
        auto boneMotorSettings(const BonePhysics& bone) -> JPH::MotorSettings
        {
            JPH::MotorSettings motor;
            motor.mSpringSettings.mFrequency = bone.driveStiffness > 0.001f ? bone.driveStiffness : 8.0f;
            motor.mSpringSettings.mDamping = bone.driveDamping > 0.001f ? bone.driveDamping : 1.0f;
            motor.SetTorqueLimit(bone.driveMaxForce > 0.001f ? bone.driveMaxForce : 1000.0f);
            return motor;
        }

        // Build the constraint attaching a bone's part to its parent, from BonePhysics.joint. Anchored
        // (world space) at the child bone's joint origin, twist along the bone. A zero swing/twist limit
        // falls back to a sensible default so a freshly-imported ragdoll is floppy, not rigid. SwingTwist
        // carries the per-bone PD motors (driven only when the ragdoll is active).
        auto buildJointConstraint(const BonePhysics& bone, glm::vec3 childPos, glm::vec3 parentPos)
            -> JPH::Ref<JPH::TwoBodyConstraintSettings>
        {
            const JPH::Vec3 anchor = toJolt(childPos);
            const glm::vec3 along = childPos - parentPos;
            const JPH::Vec3 twist = glm::length(along) > 1e-4f ? toJolt(glm::normalize(along)) : JPH::Vec3::sAxisY();
            const JPH::Vec3 plane = twist.GetNormalizedPerpendicular();
            const float swingDefault = 0.7f;  // ~40°, used when the authored limit is ~0
            const float normalCone = bone.swingTwistLimits.x > 0.001f ? bone.swingTwistLimits.x : swingDefault;
            const float planeCone = bone.swingTwistLimits.y > 0.001f ? bone.swingTwistLimits.y : swingDefault;
            const float twistLimit = bone.swingTwistLimits.z > 0.001f ? bone.swingTwistLimits.z : swingDefault;
            switch (bone.joint)
            {
            case BonePhysics::Joint::Fixed:
            {
                auto* settings = new JPH::FixedConstraintSettings();
                settings->mAutoDetectPoint = true;
                return settings;  // Ref<TwoBodyConstraintSettings> wraps the upcast pointer
            }
            case BonePhysics::Joint::Hinge:
            {
                auto* settings = new JPH::HingeConstraintSettings();
                settings->mPoint1 = settings->mPoint2 = anchor;
                settings->mHingeAxis1 = settings->mHingeAxis2 = plane;
                settings->mNormalAxis1 = settings->mNormalAxis2 = twist;
                settings->mLimitsMin = -normalCone;
                settings->mLimitsMax = normalCone;
                return settings;
            }
            case BonePhysics::Joint::Free:
            {
                auto* settings = new JPH::PointConstraintSettings();
                settings->mPoint1 = settings->mPoint2 = anchor;
                return settings;
            }
            case BonePhysics::Joint::SwingTwist:
            default:
            {
                auto* settings = new JPH::SwingTwistConstraintSettings();
                settings->mPosition1 = settings->mPosition2 = anchor;
                settings->mTwistAxis1 = settings->mTwistAxis2 = twist;
                settings->mPlaneAxis1 = settings->mPlaneAxis2 = plane;
                settings->mNormalHalfConeAngle = normalCone;
                settings->mPlaneHalfConeAngle = planeCone;
                settings->mTwistMinAngle = -twistLimit;
                settings->mTwistMaxAngle = twistLimit;
                settings->mSwingMotorSettings = boneMotorSettings(bone);
                settings->mTwistMotorSettings = boneMotorSettings(bone);
                return settings;
            }
            }
        }

        auto toJoltMotion(RigidbodyComponent::Motion m) -> JPH::EMotionType
        {
            switch (m)
            {
            case RigidbodyComponent::Motion::Static:
                return JPH::EMotionType::Static;
            case RigidbodyComponent::Motion::Kinematic:
                return JPH::EMotionType::Kinematic;
            case RigidbodyComponent::Motion::Dynamic:
                return JPH::EMotionType::Dynamic;
            }
            return JPH::EMotionType::Static;
        }

        // Resolve a body's object layer. Precedence: isSensor > the moving-slot the Rigidbody's
        // collisionLayer selects (0 = Moving, 1 = Character, 2 = Debris) > implicit Static (a lone
        // collider or an explicit Static rigidbody).
        auto resolveObjectLayer(bool hasRigidbody, RigidbodyComponent::Motion motion, i32 collisionLayer, bool isSensor)
            -> JPH::ObjectLayer
        {
            ObjectLayer layer = ObjectLayer::Static;
            if (isSensor)
            {
                layer = ObjectLayer::Sensor;
            }
            else if (hasRigidbody && motion != RigidbodyComponent::Motion::Static)
            {
                switch (collisionLayer)
                {
                case 1:
                    layer = ObjectLayer::Character;
                    break;
                case 2:
                    layer = ObjectLayer::Debris;
                    break;
                default:
                    layer = ObjectLayer::Moving;  // 0 = default moving; unknown clamps to Moving
                    break;
                }
            }
            return static_cast<JPH::ObjectLayer>(layer);
        }

        // Per-axis position/rotation locks -> the body's allowed degrees of freedom.
        auto allowedDOFs(const RigidbodyComponent& rb) -> JPH::EAllowedDOFs
        {
            JPH::EAllowedDOFs dofs = JPH::EAllowedDOFs::All;
            if (rb.lockPosition.x)
            {
                dofs &= ~JPH::EAllowedDOFs::TranslationX;
            }
            if (rb.lockPosition.y)
            {
                dofs &= ~JPH::EAllowedDOFs::TranslationY;
            }
            if (rb.lockPosition.z)
            {
                dofs &= ~JPH::EAllowedDOFs::TranslationZ;
            }
            if (rb.lockRotation.x)
            {
                dofs &= ~JPH::EAllowedDOFs::RotationX;
            }
            if (rb.lockRotation.y)
            {
                dofs &= ~JPH::EAllowedDOFs::RotationY;
            }
            if (rb.lockRotation.z)
            {
                dofs &= ~JPH::EAllowedDOFs::RotationZ;
            }
            return dofs;
        }

        // Create a shape from any settings (virtual ShapeSettings::Create); null + a loud log on error.
        auto createShape(const JPH::ShapeSettings& settings, const char* what) -> JPH::ShapeRefC
        {
            const JPH::ShapeSettings::ShapeResult result = settings.Create();
            if (result.HasError())
            {
                logError(std::string{ "physics: " } + what + " shape create failed: " + result.GetError().c_str());
                return {};
            }
            return result.Get();
        }

        // Place a shape in the body's local frame when the collider has a non-zero offset.
        auto wrapOffset(JPH::ShapeRefC shape, glm::vec3 offset) -> JPH::ShapeRefC
        {
            if (shape == nullptr || offset == glm::vec3(0.0f))
            {
                return shape;
            }
            const JPH::RotatedTranslatedShapeSettings wrap(toJolt(offset), JPH::Quat::sIdentity(), shape);
            return createShape(wrap, "offset");
        }

        // Build the Jolt collision shape for a collider. Analytic shapes size from the (auto-fitted)
        // component fields; ConvexHull/Mesh cook from the entity's .smesh via `cook`. MeshShape is
        // rejected on a Dynamic body (Jolt restriction) with a loud failure — the caller skips it.
        // Cook inputs are fed in stable index order so the cooked shape is reproducible run-to-run.
        auto buildColliderShape(const ColliderComponent& c, RigidbodyComponent::Motion motion,
                                const MeshCookSource& cook) -> JPH::ShapeRefC
        {
            switch (c.shape)
            {
            case ColliderComponent::Shape::Box:
            {
                const glm::vec3 he = glm::max(c.halfExtents, glm::vec3(0.01f));  // Jolt rejects a degenerate box
                const float convexRadius = std::min(0.05f, std::min({ he.x, he.y, he.z }) * 0.5f);
                return wrapOffset(createShape(JPH::BoxShapeSettings(toJolt(he), convexRadius), "box"), c.offset);
            }
            case ColliderComponent::Shape::Sphere:
            {
                const float radius = std::max(c.halfExtents.x, 0.01f);  // radius packed in .x
                return wrapOffset(createShape(JPH::SphereShapeSettings(radius), "sphere"), c.offset);
            }
            case ColliderComponent::Shape::Capsule:
            {
                const float radius = std::max(c.halfExtents.x, 0.01f);      // radius in .x
                const float halfHeight = std::max(c.halfExtents.y, 0.01f);  // cylinder half-height in .y (Y-up)
                return wrapOffset(createShape(JPH::CapsuleShapeSettings(halfHeight, radius), "capsule"), c.offset);
            }
            case ColliderComponent::Shape::ConvexHull:
            {
                if (!cook)
                {
                    logError("physics: convex-hull collider has no mesh cook source");
                    return {};
                }
                const Result<Mesh> mesh = cook(c.sourceMesh);
                if (!mesh)
                {
                    logError(std::string{ "physics: convex-hull cook failed: " } + mesh.error());
                    return {};
                }
                JPH::Array<JPH::Vec3> points;
                points.reserve(mesh->vertices.size());
                for (const Vertex& v : mesh->vertices)  // index order — stable for determinism
                {
                    points.push_back(toJolt(v.position));
                }
                if (points.empty())
                {
                    logError("physics: convex-hull source mesh has no vertices");
                    return {};
                }
                return wrapOffset(createShape(JPH::ConvexHullShapeSettings(points), "convex hull"), c.offset);
            }
            case ColliderComponent::Shape::Mesh:
            {
                if (motion == RigidbodyComponent::Motion::Dynamic)
                {
                    logError("physics: Mesh collider on a Dynamic body is invalid — Jolt MeshShape is "
                             "Static/Kinematic only; use ConvexHull for a dynamic body, or make it "
                             "Static/Kinematic");
                    return {};
                }
                if (!cook)
                {
                    logError("physics: mesh collider has no mesh cook source");
                    return {};
                }
                const Result<Mesh> mesh = cook(c.sourceMesh);
                if (!mesh)
                {
                    logError(std::string{ "physics: mesh cook failed: " } + mesh.error());
                    return {};
                }
                JPH::VertexList vertices;
                vertices.reserve(mesh->vertices.size());
                for (const Vertex& v : mesh->vertices)
                {
                    vertices.push_back(JPH::Float3(v.position.x, v.position.y, v.position.z));
                }
                JPH::IndexedTriangleList triangles;
                triangles.reserve(mesh->indices.size() / 3);
                for (std::size_t i = 0; i + 2 < mesh->indices.size(); i = i + 3)
                {
                    triangles.push_back(
                        JPH::IndexedTriangle(mesh->indices[i], mesh->indices[i + 1], mesh->indices[i + 2], 0));
                }
                if (triangles.empty())
                {
                    logError("physics: mesh source has no triangles");
                    return {};
                }
                return wrapOffset(createShape(JPH::MeshShapeSettings(vertices, triangles), "mesh"), c.offset);
            }
            }
            return {};
        }

        inline constexpr std::size_t ContactRingCap = 256;

        // A raw contact transition captured on a Jolt job thread. Only BodyIDs + a representative
        // point/normal are recorded here; the BodyID -> entity mapping is done on the sim thread.
        struct PendingContact
        {
            JPH::BodyID a;
            JPH::BodyID b;
            glm::vec3 point{ 0.0f };
            glm::vec3 normal{ 0.0f };
            bool begin = true;
        };

        // Jolt invokes the contact callbacks from job threads during Update, so they must not touch
        // the entity maps or the seq-stamped ring directly. They buffer raw pairs under a mutex; the
        // sim thread drains them after Update (see stepPhysics). OnContactPersisted is ignored — v1
        // emits Begin/End transitions only (the trigger model), not a per-frame "still overlapping".
        class ContactListenerImpl final : public JPH::ContactListener
        {
          public:
            void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                                JPH::ContactSettings&) override
            {
                const JPH::RVec3 point = manifold.GetWorldSpaceContactPointOn1(0);
                const std::scoped_lock lock(mutex_);
                pending_.push_back(PendingContact{ .a = body1.GetID(),
                                                   .b = body2.GetID(),
                                                   .point = fromJolt(JPH::Vec3(point)),
                                                   .normal = fromJolt(manifold.mWorldSpaceNormal),
                                                   .begin = true });
            }

            void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
            {
                const std::scoped_lock lock(mutex_);
                pending_.push_back(PendingContact{ .a = pair.GetBody1ID(),
                                                   .b = pair.GetBody2ID(),
                                                   .point = glm::vec3(0.0f),
                                                   .normal = glm::vec3(0.0f),
                                                   .begin = false });
            }

            auto drain() -> std::vector<PendingContact>
            {
                const std::scoped_lock lock(mutex_);
                std::vector<PendingContact> out = std::move(pending_);
                pending_.clear();
                return out;
            }

          private:
            std::mutex mutex_;
            std::vector<PendingContact> pending_;
        };
    }

    // One body created from an entity's components — tracked for transform write-back and (later
    // phases) scene queries + contact events. Stored in creation order so the sim stays
    // reproducible run-to-run (the cross-platform-deterministic build).
    struct BodyEntry
    {
        Entity entity;
        u64 uuid = 0;
        JPH::BodyID id;
        MotionType motion = MotionType::Static;
        bool sensor = false;
    };

    // One live ragdoll: a Jolt Ragdoll whose parts mirror SkinnedMeshComponent.bones 1:1, plus the
    // per-bone blend weight (1 = pure physics, 0 = pure animation) and the parent-bone index used to
    // convert each part's world transform back to a bone-local PoseOverride.
    struct RagdollEntry
    {
        u64 rig = 0;                              // rig mesh entity uuid
        Entity rigEntity;                         // for the world->local conversion (parent of the root bone)
        JPH::Ref<JPH::RagdollSettings> settings;  // kept alive: the Ragdoll references it
        JPH::Ref<JPH::Ragdoll> ragdoll;
        std::vector<i32> parentIndex;    // bone i -> parent bone index (-1 = root)
        std::vector<f32> weightTarget;   // per bone: desired physics weight (Phase 9)
        std::vector<f32> weightCurrent;  // per bone: eased weight (1 in pure ragdoll)
        f32 weightRate = 6.0f;           // weight units/sec the eased weight approaches the target
        bool motorsActive = false;       // active (motor-driven) vs passive ragdoll
    };

    // The Jolt objects behind the opaque PhysicsWorld handle. The filter interfaces are
    // held by value and must outlive `system` (Jolt borrows them for the world's lifetime);
    // `system` is declared last so it is destroyed first, before the filters it references.
    struct PhysicsWorldImpl
    {
        BroadPhaseLayerImpl broadPhaseLayer;
        ObjectVsBroadPhaseImpl objectVsBroadPhase;
        ObjectLayerPairImpl objectLayerPair;
        ContactListenerImpl contactListener;  // declared before `system` so it outlives it
        std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
        std::unique_ptr<JPH::JobSystemThreadPool> jobSystem;
        JPH::PhysicsSystem system;
        std::vector<BodyEntry> bodies;  // every created body, in creation order (deterministic)
        std::unordered_map<JPH::BodyID, std::size_t> indexByBodyId;  // body -> index into `bodies`
        std::vector<ContactEvent> contactRing;                       // bounded ring, oldest evicted at ContactRingCap
        u64 contactSeq = 0;        // last assigned ContactEvent.seq (drain high-water)
        i32 dynamicBodyCount = 0;  // of `bodies`, how many are Dynamic
        i64 stepCount = 0;         // physics steps run (the ContactEvent.tick)
        f32 accumulator = 0.0f;    // fixed-step accumulator
        // CharacterVirtual sweep objects (not bodies) per character entity; declared last so they are
        // released before `system`, which they reference.
        std::vector<std::pair<Entity, JPH::Ref<JPH::CharacterVirtual>>> characters;
        std::vector<RagdollEntry> ragdolls;  // released before `system` (their bodies live in it)

        // Remove every live ragdoll from the system before its bodies are destroyed: ~Ragdoll
        // destroys its bodies but never removes their constraints, so a ragdoll left live at world
        // teardown (stop without disable) would strand dangling constraints and stall the drop. The
        // body runs before the members destruct, so the subsequent ~Ragdoll DestroyBodies is safe.
        ~PhysicsWorldImpl()
        {
            for (RagdollEntry& entry : ragdolls)
            {
                if (entry.ragdoll != nullptr)
                {
                    entry.ragdoll->RemoveFromPhysicsSystem();
                }
            }
        }
    };

    PhysicsWorld::PhysicsWorld() noexcept = default;
    PhysicsWorld::PhysicsWorld(PhysicsWorld&&) noexcept = default;
    PhysicsWorld& PhysicsWorld::operator=(PhysicsWorld&&) noexcept = default;
    PhysicsWorld::~PhysicsWorld() = default;

    // The v1 collision matrix (symmetric): a sensor overlaps every solid layer to generate triggers
    // but never another sensor; two static bodies never collide; debris collides with the world and
    // characters but not other debris; everything else collides.
    auto layersCollide(ObjectLayer a, ObjectLayer b) -> bool
    {
        if (a == ObjectLayer::Sensor || b == ObjectLayer::Sensor)
        {
            return !(a == ObjectLayer::Sensor && b == ObjectLayer::Sensor);
        }
        if (a == ObjectLayer::Static && b == ObjectLayer::Static)
        {
            return false;
        }
        if (a == ObjectLayer::Debris && b == ObjectLayer::Debris)
        {
            return false;
        }
        return true;
    }

    auto initPhysics() -> Result<void>
    {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = joltTrace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = joltAssertFailed;)
        if (JPH::Factory::sInstance == nullptr)
        {
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }
        return {};
    }

    void shutdownPhysics()
    {
        if (JPH::Factory::sInstance != nullptr)
        {
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }
    }

    auto createPhysicsWorld() -> Result<PhysicsWorld>
    {
        PhysicsWorld world;
        world.impl_ = std::make_unique<PhysicsWorldImpl>();
        PhysicsWorldImpl& impl = *world.impl_;
        // 10 MiB scratch for the solver; the canonical Jolt job-system bounds + auto thread count.
        impl.tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10 * 1024 * 1024);
        impl.jobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, -1);
        // v1 limits: 1024 bodies, default mutex count, 1024 body pairs / contact constraints.
        impl.system.Init(1024, 0, 1024, 1024, impl.broadPhaseLayer, impl.objectVsBroadPhase, impl.objectLayerPair);
        impl.system.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
        impl.system.SetContactListener(&impl.contactListener);
        return world;
    }

    auto physicsWorldStats(const PhysicsWorld& world) -> PhysicsWorldStats
    {
        PhysicsWorldStats stats;
        const PhysicsWorldImpl* impl = world.impl();
        stats.active = impl != nullptr;
        if (impl != nullptr)
        {
            stats.bodyCount = static_cast<i32>(impl->system.GetNumBodies());
            stats.dynamicCount = impl->dynamicBodyCount;
        }
        return stats;
    }

    auto listPhysicsBodies(const PhysicsWorld& world) -> std::vector<PhysicsBodyInfo>
    {
        std::vector<PhysicsBodyInfo> out;
        const PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return out;
        }
        const JPH::BodyInterface& bi = impl->system.GetBodyInterface();
        out.reserve(impl->bodies.size());
        for (const BodyEntry& entry : impl->bodies)
        {
            const JPH::RVec3 pos = bi.GetPosition(entry.id);
            out.push_back(PhysicsBodyInfo{ .entity = entry.uuid,
                                           .motion = entry.motion,
                                           .active = bi.IsActive(entry.id),
                                           .position = glm::vec3(pos.GetX(), pos.GetY(), pos.GetZ()) });
        }
        return out;
    }

    namespace
    {
        // The Dynamic body owned by `uuid`, or an invalid id (impulses/velocity only apply to Dynamic
        // bodies — a Static/Kinematic one would silently ignore them).
        auto dynamicBodyId(const PhysicsWorldImpl& impl, u64 uuid) -> JPH::BodyID
        {
            for (const BodyEntry& entry : impl.bodies)
            {
                if (entry.uuid == uuid && entry.motion == MotionType::Dynamic)
                {
                    return entry.id;
                }
            }
            return {};
        }
    }

    void applyBodyImpulse(PhysicsWorld& world, u64 entity, glm::vec3 impulse)
    {
        PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return;
        }
        const JPH::BodyID id = dynamicBodyId(*impl, entity);
        if (id.IsInvalid())
        {
            logWarn(std::format("physics: apply-impulse on a non-Dynamic / unmapped body ({})", entity));
            return;
        }
        JPH::BodyInterface& bi = impl->system.GetBodyInterface();
        bi.ActivateBody(id);
        bi.AddImpulse(id, toJolt(impulse));
    }

    void addBodyForce(PhysicsWorld& world, u64 entity, glm::vec3 force)
    {
        PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return;
        }
        const JPH::BodyID id = dynamicBodyId(*impl, entity);
        if (id.IsInvalid())
        {
            logWarn(std::format("physics: add-force on a non-Dynamic / unmapped body ({})", entity));
            return;
        }
        JPH::BodyInterface& bi = impl->system.GetBodyInterface();
        bi.ActivateBody(id);
        bi.AddForce(id, toJolt(force));
    }

    void setBodyLinearVelocity(PhysicsWorld& world, u64 entity, glm::vec3 velocity)
    {
        PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return;
        }
        const JPH::BodyID id = dynamicBodyId(*impl, entity);
        if (id.IsInvalid())
        {
            logWarn(std::format("physics: set-velocity on a non-Dynamic / unmapped body ({})", entity));
            return;
        }
        JPH::BodyInterface& bi = impl->system.GetBodyInterface();
        bi.ActivateBody(id);
        bi.SetLinearVelocity(id, toJolt(velocity));
    }

    auto bodyLinearVelocity(const PhysicsWorld& world, u64 entity) -> glm::vec3
    {
        const PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return glm::vec3{ 0.0f };
        }
        const JPH::BodyID id = dynamicBodyId(*impl, entity);
        if (id.IsInvalid())
        {
            return glm::vec3{ 0.0f };
        }
        return fromJolt(impl->system.GetBodyInterface().GetLinearVelocity(id));
    }

    void populatePhysicsWorld(PhysicsWorld& world, Scene& scene, const MeshCookSource& cook)
    {
        PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return;
        }
        JPH::BodyInterface& bi = impl->system.GetBodyInterface();
        forEach<ColliderComponent>(
            scene,
            [&](Entity entity, ColliderComponent& collider)
            {
                // A CharacterController owns its capsule via a CharacterVirtual, not a world body —
                // never make a static body for it (it would block the sweep).
                if (hasComponent<CharacterControllerComponent>(scene, entity))
                {
                    return;
                }
                // A collider with no Rigidbody is an implicit Static body; with one, its motion
                // wins.
                RigidbodyComponent::Motion motion = RigidbodyComponent::Motion::Static;
                const RigidbodyComponent* rb = nullptr;
                if (hasComponent<RigidbodyComponent>(scene, entity))
                {
                    rb = &getComponent<RigidbodyComponent>(scene, entity);
                    motion = rb->motion;
                }
                const JPH::ShapeRefC shape = buildColliderShape(collider, motion, cook);
                if (shape == nullptr)
                {
                    return;  // cook failed and logged — skip this body, don't abort the world
                }
                // The play scene's caches may be cold here; worldTranslation/Rotation compose on
                // a miss.
                const glm::vec3 pos = worldTranslation(scene, entity);
                const glm::quat rot = worldRotation(scene, entity);
                const JPH::ObjectLayer objectLayer = resolveObjectLayer(
                    rb != nullptr, motion, rb != nullptr ? rb->collisionLayer : 0, collider.isSensor);
                JPH::BodyCreationSettings settings(shape, toJolt(pos), toJolt(rot), toJoltMotion(motion), objectLayer);
                settings.mIsSensor = collider.isSensor;
                settings.mFriction = collider.material.friction;
                settings.mRestitution = collider.material.restitution;
                if (rb != nullptr && motion == RigidbodyComponent::Motion::Dynamic)
                {
                    settings.mLinearDamping = rb->linearDamping;
                    settings.mAngularDamping = rb->angularDamping;
                    settings.mGravityFactor = rb->gravityFactor;
                    settings.mAllowedDOFs = allowedDOFs(*rb);
                    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                    settings.mMassPropertiesOverride.mMass = rb->mass;
                }
                const JPH::EActivation activation = motion == RigidbodyComponent::Motion::Static
                                                        ? JPH::EActivation::DontActivate
                                                        : JPH::EActivation::Activate;
                const JPH::BodyID id = bi.CreateAndAddBody(settings, activation);
                if (id.IsInvalid())
                {
                    logError("physics: body create failed (body limit reached?)");
                    return;
                }
                u64 uuid = 0;
                if (hasComponent<IdComponent>(scene, entity))
                {
                    uuid = getComponent<IdComponent>(scene, entity).id.value;
                }
                impl->indexByBodyId[id] = impl->bodies.size();
                impl->bodies.push_back(BodyEntry{ .entity = entity,
                                                  .uuid = uuid,
                                                  .id = id,
                                                  .motion = static_cast<MotionType>(motion),
                                                  .sensor = collider.isSensor });
                if (motion == RigidbodyComponent::Motion::Dynamic)
                {
                    impl->dynamicBodyCount += 1;
                }
            });
    }

    void buildBoneBodies(PhysicsWorld& world, Scene& scene)
    {
        PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return;
        }
        JPH::BodyInterface& bi = impl->system.GetBodyInterface();
        forEach<KinematicBonesComponent>(
            scene,
            [&](Entity rig, KinematicBonesComponent& bones)
            {
                if (!bones.enabled || !hasComponent<SkinnedMeshComponent>(scene, rig))
                {
                    return;
                }
                const SkinnedMeshComponent& skin = getComponent<SkinnedMeshComponent>(scene, rig);
                const BonePhysicsComponent* phys = hasComponent<BonePhysicsComponent>(scene, rig)
                                                       ? &getComponent<BonePhysicsComponent>(scene, rig)
                                                       : nullptr;
                auto driven = [&](std::size_t index) -> bool
                {
                    if (bones.driven.empty())
                    {
                        return true;  // empty = every joint
                    }
                    for (const i32 wanted : bones.driven)
                    {
                        if (std::cmp_equal(wanted, index))
                        {
                            return true;
                        }
                    }
                    return false;
                };
                for (std::size_t index = 0; index < skin.boneHandles.size(); index = index + 1)
                {
                    if (!driven(index))
                    {
                        continue;
                    }
                    const Entity joint{ skin.boneHandles[index] };
                    if (!valid(scene, joint))
                    {
                        continue;
                    }
                    // Capsule from the per-bone shapeHalfExtents (radius .x, half-height .y), Y-up; a
                    // small default for a leaf/unfitted bone so Jolt never rejects a degenerate capsule.
                    const glm::vec3 extents = (phys != nullptr && index < phys->bones.size())
                                                  ? phys->bones[index].shapeHalfExtents
                                                  : glm::vec3(0.0f);
                    const float radius = std::max(extents.x, 0.03f);
                    const float halfHeight = std::max(extents.y, 0.03f);
                    const JPH::ShapeRefC shape =
                        createShape(JPH::CapsuleShapeSettings(halfHeight, radius), "bone capsule");
                    if (shape == nullptr)
                    {
                        continue;
                    }
                    const auto [position, rotation] = worldPose(scene, joint);
                    const JPH::BodyCreationSettings settings(shape, toJolt(position), toJolt(rotation),
                                                             JPH::EMotionType::Kinematic,
                                                             static_cast<JPH::ObjectLayer>(ObjectLayer::Moving));
                    const JPH::BodyID id = bi.CreateAndAddBody(settings, JPH::EActivation::Activate);
                    if (id.IsInvalid())
                    {
                        continue;
                    }
                    u64 uuid = 0;
                    if (hasComponent<IdComponent>(scene, joint))
                    {
                        uuid = getComponent<IdComponent>(scene, joint).id.value;
                    }
                    impl->indexByBodyId[id] = impl->bodies.size();
                    impl->bodies.push_back(BodyEntry{
                        .entity = joint, .uuid = uuid, .id = id, .motion = MotionType::Kinematic, .sensor = false });
                }
            });
    }

    auto addCharacter(PhysicsWorld& world, Entity entity, Scene& scene) -> Result<void>
    {
        PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return Err(std::string{ "no physics world" });
        }
        // The capsule is the entity's ColliderComponent (Shape::Capsule); fall back to a default.
        float radius = 0.3f;
        float halfHeight = 0.6f;
        if (hasComponent<ColliderComponent>(scene, entity))
        {
            const ColliderComponent& collider = getComponent<ColliderComponent>(scene, entity);
            radius = std::max(collider.halfExtents.x, 0.05f);
            halfHeight = std::max(collider.halfExtents.y, 0.05f);
        }
        float slopeAngle = 0.785398f;
        if (hasComponent<CharacterControllerComponent>(scene, entity))
        {
            slopeAngle = getComponent<CharacterControllerComponent>(scene, entity).maxSlopeAngle;
        }
        const JPH::ShapeRefC shape = createShape(JPH::CapsuleShapeSettings(halfHeight, radius), "character capsule");
        if (shape == nullptr)
        {
            return Err(std::string{ "character capsule create failed" });
        }
        JPH::CharacterVirtualSettings settings;
        settings.mShape = shape;
        settings.mMaxSlopeAngle = slopeAngle;
        const auto [position, rotation] = worldPose(scene, entity);
        JPH::Ref<JPH::CharacterVirtual> character =
            new JPH::CharacterVirtual(&settings, toJolt(position), JPH::Quat::sIdentity(), &impl->system);
        impl->characters.emplace_back(entity, std::move(character));
        return {};
    }

    void stepPhysics(PhysicsWorld& world, Scene& scene, f32 dt)
    {
        PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return;
        }
        // dt is already PlayMaxDelta-clamped by tickPlay; advance the sim in fixed substeps so the
        // simulation is frame-rate independent and stays bit-exact (cross-platform deterministic).
        impl->accumulator += dt;
        int substeps = 0;
        constexpr int maxSubsteps = 8;  // backstop against a runaway accumulator
        JPH::BodyInterface& bi = impl->system.GetBodyInterface();
        while (impl->accumulator >= PhysicsFixedStep && substeps < maxSubsteps)
        {
            // Drive every Kinematic body (free kinematic bodies + per-bone bodies) toward its entity's
            // current world transform via MoveKinematic, so the swept motion imparts contact velocity to
            // the dynamic bodies it hits (never a teleport, which gives zero contact velocity). The same
            // PhysicsFixedStep feeds MoveKinematic and Update so the derived velocity matches the step.
            for (const BodyEntry& entry : impl->bodies)
            {
                if (entry.motion != MotionType::Kinematic || !valid(scene, entry.entity))
                {
                    continue;
                }
                const auto [position, rotation] = worldPose(scene, entry.entity);
                bi.MoveKinematic(entry.id, toJolt(position), toJolt(rotation), PhysicsFixedStep);
            }
            static_cast<void>(
                impl->system.Update(PhysicsFixedStep, 1, impl->tempAllocator.get(), impl->jobSystem.get()));
            // Advance every CharacterVirtual against the just-settled world: gravity + the
            // move-character desired velocity, with stick-to-floor + WalkStairs (same fixed dt).
            const JPH::Vec3 gravity = impl->system.GetGravity();
            for (auto& [entity, character] : impl->characters)
            {
                if (!hasComponent<CharacterControllerComponent>(scene, entity))
                {
                    continue;
                }
                auto& controller = getComponent<CharacterControllerComponent>(scene, entity);
                const bool grounded = character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
                if (grounded && controller.verticalVelocity <= 0.0f)
                {
                    controller.verticalVelocity = 0.0f;  // rest on the floor
                }
                else
                {
                    controller.verticalVelocity += gravity.GetY() * controller.gravityFactor * PhysicsFixedStep;
                }
                glm::vec3 horizontal(controller.desiredVelocity.x, 0.0f, controller.desiredVelocity.z);
                const float speed = glm::length(horizontal);
                if (speed > controller.maxSpeed && speed > 1e-5f)
                {
                    horizontal = horizontal * (controller.maxSpeed / speed);
                }
                character->SetLinearVelocity(JPH::Vec3(horizontal.x, controller.verticalVelocity, horizontal.z));
                JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
                updateSettings.mWalkStairsStepUp = JPH::Vec3(0.0f, controller.maxStepHeight, 0.0f);
                const auto layer = static_cast<JPH::ObjectLayer>(ObjectLayer::Character);
                character->ExtendedUpdate(PhysicsFixedStep, gravity * controller.gravityFactor, updateSettings,
                                          impl->system.GetDefaultBroadPhaseLayerFilter(layer),
                                          impl->system.GetDefaultLayerFilter(layer), JPH::BodyFilter{},
                                          JPH::ShapeFilter{}, *impl->tempAllocator);
                controller.onGround = character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
            }
            impl->accumulator -= PhysicsFixedStep;
            impl->stepCount += 1;
            substeps += 1;
        }
        if (substeps == 0)
        {
            return;  // no fixed step elapsed this frame — transforms are unchanged
        }
        // Write each Dynamic body's world pose back into its entity's LOCAL TransformComponent. Phase 2
        // scopes bodies to root entities (world == local); the parented-body local rebase is later.
        for (const BodyEntry& entry : impl->bodies)
        {
            if (entry.motion != MotionType::Dynamic || !hasComponent<TransformComponent>(scene, entry.entity))
            {
                continue;
            }
            JPH::RVec3 position;
            JPH::Quat rotation;
            bi.GetPositionAndRotation(entry.id, position, rotation);
            auto& transform = getComponent<TransformComponent>(scene, entry.entity);
            transform.translation = fromJolt(position);
            transform.rotation = glm::eulerAngles(fromJolt(rotation));  // round-trips through transformMatrix
        }
        // Write each character's resolved world position back into its entity-root TransformComponent
        // (binding mode a: position only — rotation/animation are independent).
        for (auto& [entity, character] : impl->characters)
        {
            if (!hasComponent<TransformComponent>(scene, entity))
            {
                continue;
            }
            const JPH::RVec3 characterPosition = character->GetPosition();
            getComponent<TransformComponent>(scene, entity).translation = fromJolt(JPH::Vec3(characterPosition));
        }
        // Drain the contact transitions Jolt buffered on its job threads into the seq-stamped ring
        // (safe here: single-threaded, the body -> entity index is stable for the play session).
        const std::vector<PendingContact> pending = impl->contactListener.drain();
        for (const PendingContact& pc : pending)
        {
            const auto aIt = impl->indexByBodyId.find(pc.a);
            const auto bIt = impl->indexByBodyId.find(pc.b);
            const bool aKnown = aIt != impl->indexByBodyId.end();
            const bool bKnown = bIt != impl->indexByBodyId.end();
            ContactEvent event;
            event.seq = ++impl->contactSeq;
            event.kind = pc.begin ? ContactEvent::Kind::Begin : ContactEvent::Kind::End;
            event.entityA = aKnown ? impl->bodies[aIt->second].uuid : 0;
            event.entityB = bKnown ? impl->bodies[bIt->second].uuid : 0;
            event.sensor = (aKnown && impl->bodies[aIt->second].sensor) || (bKnown && impl->bodies[bIt->second].sensor);
            event.point = pc.point;
            event.normal = pc.normal;
            event.tick = impl->stepCount;
            if (impl->contactRing.size() >= ContactRingCap)
            {
                impl->contactRing.erase(impl->contactRing.begin());  // evict oldest at cap
            }
            impl->contactRing.push_back(event);
        }
    }

    auto drainContacts(const PhysicsWorld& world, u64 since) -> ContactDrain
    {
        ContactDrain drain;
        const PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return drain;  // no world in Edit — an empty drain, never an error
        }
        for (const ContactEvent& event : impl->contactRing)
        {
            if (event.seq > since)
            {
                drain.events.push_back(event);
            }
        }
        drain.highWaterSeq = impl->contactSeq;
        drain.oldestSeq = impl->contactRing.empty() ? 0 : impl->contactRing.front().seq;
        // A cursor older than the oldest retained event missed evictions — signal a resync.
        drain.overflowed = drain.oldestSeq > 0 && since + 1 < drain.oldestSeq;
        return drain;
    }

    namespace
    {
        // Map a hit BodyID back to its owner entity uuid (0 = no owning entity).
        auto bodyUuid(const PhysicsWorldImpl& impl, JPH::BodyID id) -> u64
        {
            const auto it = impl.indexByBodyId.find(id);
            return it != impl.indexByBodyId.end() ? impl.bodies[it->second].uuid : 0;
        }
    }

    auto raycastWorld(const PhysicsWorld& world, glm::vec3 origin, glm::vec3 dir, f32 maxDist) -> PhysicsRayHit
    {
        PhysicsRayHit out;
        const PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return out;
        }
        const JPH::RRayCast ray{ toJolt(origin), toJolt(dir) * maxDist };
        JPH::RayCastResult result;
        if (!impl->system.GetNarrowPhaseQuery().CastRay(ray, result))
        {
            return out;  // nothing along the ray
        }
        out.hit = true;
        const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
        out.point = fromJolt(JPH::Vec3(point));
        out.distance = result.mFraction * maxDist;
        const JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(), result.mBodyID);
        if (lock.Succeeded())
        {
            out.normal = fromJolt(lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
        }
        out.entity = bodyUuid(*impl, result.mBodyID);
        return out;
    }

    auto sphereCastWorld(const PhysicsWorld& world, glm::vec3 origin, glm::vec3 dir, f32 radius, f32 maxDist)
        -> PhysicsRayHit
    {
        PhysicsRayHit out;
        const PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return out;
        }
        const JPH::ShapeRefC sphere = createShape(JPH::SphereShapeSettings(std::max(radius, 0.001f)), "query sphere");
        if (sphere == nullptr)
        {
            return out;
        }
        const JPH::RShapeCast shapeCast = JPH::RShapeCast::sFromWorldTransform(
            sphere, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(toJolt(origin)), toJolt(dir) * maxDist);
        const JPH::ShapeCastSettings settings;
        JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
        impl->system.GetNarrowPhaseQuery().CastShape(shapeCast, settings, toJolt(origin), collector);
        if (!collector.HadHit())
        {
            return out;
        }
        out.hit = true;
        out.distance = collector.mHit.mFraction * maxDist;
        out.point = origin + fromJolt(collector.mHit.mContactPointOn2);  // relative to the base offset (origin)
        out.normal = fromJolt(-collector.mHit.mPenetrationAxis.Normalized());
        out.entity = bodyUuid(*impl, collector.mHit.mBodyID2);
        return out;
    }

    void disableRagdoll(PhysicsWorld& world, u64 rig)
    {
        PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return;
        }
        for (auto it = impl->ragdolls.begin(); it != impl->ragdolls.end();)
        {
            if (it->rig == rig)
            {
                if (it->ragdoll != nullptr)
                {
                    it->ragdoll->RemoveFromPhysicsSystem();
                }
                it = impl->ragdolls.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    auto hasRagdoll(const PhysicsWorld& world, u64 rig) -> bool
    {
        const PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return false;
        }
        for (const RagdollEntry& entry : impl->ragdolls)
        {
            if (entry.rig == rig)
            {
                return true;
            }
        }
        return false;
    }

    auto enableRagdoll(PhysicsWorld& world, Scene& scene, Entity rig) -> Result<void>
    {
        PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return Err(std::string{ "no physics world" });
        }
        if (!hasComponent<SkinnedMeshComponent>(scene, rig) || !hasComponent<BonePhysicsComponent>(scene, rig))
        {
            return Err(std::string{ "rig has no SkinnedMesh + BonePhysics" });
        }
        const SkinnedMeshComponent& skin = getComponent<SkinnedMeshComponent>(scene, rig);
        const BonePhysicsComponent& phys = getComponent<BonePhysicsComponent>(scene, rig);
        const std::size_t count = skin.boneHandles.size();
        if (count == 0 || phys.bones.size() != count)
        {
            return Err(std::string{ "BonePhysics array length does not match the skeleton" });
        }
        const u64 rigUuid = hasComponent<IdComponent>(scene, rig) ? getComponent<IdComponent>(scene, rig).id.value : 0;
        disableRagdoll(world, rigUuid);  // idempotent re-enable

        // uuid -> bone index, then parent-bone index + current world pose per bone.
        std::unordered_map<u64, i32> boneIndexByUuid;
        for (std::size_t i = 0; i < count; i = i + 1)
        {
            const Entity bone{ skin.boneHandles[i] };
            if (valid(scene, bone) && hasComponent<IdComponent>(scene, bone))
            {
                boneIndexByUuid[getComponent<IdComponent>(scene, bone).id.value] = static_cast<i32>(i);
            }
        }
        std::vector<i32> parentIndex(count, -1);
        std::vector<glm::vec3> worldPos(count, glm::vec3(0.0f));
        std::vector<glm::quat> worldRot(count, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        for (std::size_t i = 0; i < count; i = i + 1)
        {
            const Entity bone{ skin.boneHandles[i] };
            if (!valid(scene, bone))
            {
                continue;
            }
            const auto [pos, rot] = worldPose(scene, bone);
            worldPos[i] = pos;
            worldRot[i] = rot;
            if (hasComponent<RelationshipComponent>(scene, bone))
            {
                const auto it = boneIndexByUuid.find(getComponent<RelationshipComponent>(scene, bone).parent.value);
                if (it != boneIndexByUuid.end())
                {
                    parentIndex[i] = it->second;
                }
            }
        }

        // Skeleton + parts, 1:1 with the bones (a small default capsule for an unfitted bone).
        const JPH::Ref<JPH::RagdollSettings> settings = new JPH::RagdollSettings();
        settings->mSkeleton = new JPH::Skeleton();
        for (std::size_t i = 0; i < count; i = i + 1)
        {
            settings->mSkeleton->AddJoint(std::to_string(i), parentIndex[i]);
        }
        settings->mParts.resize(count);
        for (std::size_t i = 0; i < count; i = i + 1)
        {
            const float radius = std::max(phys.bones[i].shapeHalfExtents.x, 0.03f);
            const float halfHeight = std::max(phys.bones[i].shapeHalfExtents.y, 0.03f);
            const JPH::ShapeRefC shape = createShape(JPH::CapsuleShapeSettings(halfHeight, radius), "ragdoll capsule");
            JPH::RagdollSettings::Part& part = settings->mParts[i];
            part.SetShape(shape);
            part.mPosition = toJolt(worldPos[i]);
            part.mRotation = toJolt(worldRot[i]);
            part.mMotionType = JPH::EMotionType::Dynamic;
            part.mObjectLayer = static_cast<JPH::ObjectLayer>(ObjectLayer::Moving);
            part.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            part.mMassPropertiesOverride.mMass = std::max(phys.bones[i].mass, 0.01f);
            if (parentIndex[i] >= 0)
            {
                part.mToParent = buildJointConstraint(phys.bones[i], worldPos[i],
                                                      worldPos[static_cast<std::size_t>(parentIndex[i])]);
            }
        }
        settings->Stabilize();
        settings->CalculateBodyIndexToConstraintIndex();
        JPH::Ragdoll* created = settings->CreateRagdoll(0, rigUuid, &impl->system);
        if (created == nullptr)
        {
            return Err(std::string{ "CreateRagdoll failed" });
        }
        created->AddToPhysicsSystem(JPH::EActivation::Activate);

        RagdollEntry entry;
        entry.rig = rigUuid;
        entry.rigEntity = rig;
        entry.settings = settings;
        entry.ragdoll = created;
        entry.parentIndex = std::move(parentIndex);
        entry.weightTarget.assign(count, 1.0f);  // pure ragdoll: physics wins outright
        entry.weightCurrent.assign(count, 1.0f);
        impl->ragdolls.push_back(std::move(entry));
        return {};
    }

    void writeRagdollPoses(PhysicsWorld& world, Scene& scene)
    {
        PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return;
        }
        const JPH::BodyInterface& bi = impl->system.GetBodyInterface();
        for (RagdollEntry& entry : impl->ragdolls)
        {
            if (!valid(scene, entry.rigEntity) || !hasComponent<SkinnedMeshComponent>(scene, entry.rigEntity))
            {
                continue;
            }
            const SkinnedMeshComponent& skin = getComponent<SkinnedMeshComponent>(scene, entry.rigEntity);
            const std::size_t count = skin.boneHandles.size();
            const std::size_t parts = entry.ragdoll->GetBodyCount();
            // Read every part's world transform up front (a part is 1:1 with a bone index).
            std::vector<glm::mat4> partWorld(count, glm::mat4(1.0f));
            for (std::size_t i = 0; i < count && i < parts; i = i + 1)
            {
                partWorld[i] = fromJoltMat(bi.GetWorldTransform(entry.ragdoll->GetBodyID(static_cast<int>(i))));
            }
            const glm::mat4 rigWorld = composeWorldMatrix(scene, entry.rigEntity);
            for (std::size_t i = 0; i < count; i = i + 1)
            {
                const Entity bone{ skin.boneHandles[i] };
                if (!valid(scene, bone) || i >= parts)
                {
                    continue;
                }
                // Local = inverse(parent world) * world, the inverse of jointMatrices' composition.
                const i32 parent = entry.parentIndex[i];
                const glm::mat4 parentWorld = parent >= 0 ? partWorld[static_cast<std::size_t>(parent)] : rigWorld;
                const glm::mat4 local = glm::inverse(parentWorld) * partWorld[i];
                glm::vec3 translation;
                glm::vec3 scaleVec;
                glm::vec3 skew;
                glm::vec4 perspective;
                glm::quat rotation;
                if (!glm::decompose(local, scaleVec, rotation, translation, skew, perspective))
                {
                    continue;
                }
                rotation = glm::normalize(rotation);
                const f32 weight = entry.weightCurrent[i];
                auto& over = hasComponent<PoseOverrideComponent>(scene, bone)
                                 ? getComponent<PoseOverrideComponent>(scene, bone)
                                 : addComponent<PoseOverrideComponent>(scene, bone);
                if (weight >= 0.999f)
                {
                    over.translation = translation;
                    over.rotation = rotation;
                    over.scale = scaleVec;
                }
                else
                {
                    // Blend physics over the animation pose the evaluator wrote earlier this frame.
                    over.translation = glm::mix(over.translation, translation, weight);
                    over.rotation = glm::normalize(glm::slerp(over.rotation, rotation, weight));
                    over.scale = glm::mix(over.scale, scaleVec, weight);
                }
            }
        }
    }

    void driveRagdollsToPose(PhysicsWorld& world, const std::vector<PoseTarget>& targets)
    {
        PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return;
        }
        for (RagdollEntry& entry : impl->ragdolls)
        {
            if (!entry.motorsActive || entry.ragdoll == nullptr || entry.settings == nullptr)
            {
                continue;
            }
            const PoseTarget* target = nullptr;
            for (const PoseTarget& candidate : targets)
            {
                if (candidate.rig == entry.rig)
                {
                    target = &candidate;
                    break;
                }
            }
            if (target == nullptr)
            {
                continue;  // no animation target this frame: let the bodies swing freely
            }
            const std::size_t count = std::min(entry.parentIndex.size(), target->local.size());
            for (std::size_t i = 0; i < count; i = i + 1)
            {
                const int constraintIdx = entry.settings->GetConstraintIndexForBodyIndex(static_cast<int>(i));
                if (constraintIdx < 0)
                {
                    continue;  // the root bone has no parent constraint to motor
                }
                JPH::TwoBodyConstraint* constraint = entry.ragdoll->GetConstraint(constraintIdx);
                if (constraint == nullptr || constraint->GetSubType() != JPH::EConstraintSubType::SwingTwist)
                {
                    continue;  // only SwingTwist carries motors; a Free/Hinge bone stays limp under drive
                }
                auto* swingTwist = static_cast<JPH::SwingTwistConstraint*>(constraint);
                swingTwist->SetSwingMotorState(JPH::EMotorState::Position);
                swingTwist->SetTwistMotorState(JPH::EMotorState::Position);
                swingTwist->SetTargetOrientationBS(toJolt(target->local[i].rotation));
            }
        }
    }

    void advanceRagdollBlend(PhysicsWorld& world, f32 dt)
    {
        PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return;
        }
        for (RagdollEntry& entry : impl->ragdolls)
        {
            const f32 step = entry.weightRate * dt;
            const std::size_t count = std::min(entry.weightCurrent.size(), entry.weightTarget.size());
            for (std::size_t i = 0; i < count; i = i + 1)
            {
                const f32 delta = entry.weightTarget[i] - entry.weightCurrent[i];
                entry.weightCurrent[i] = std::abs(delta) <= step ? entry.weightTarget[i]
                                                                 : entry.weightCurrent[i] + std::copysign(step, delta);
            }
        }
    }

    auto setRagdollBlend(PhysicsWorld& world, u64 rig, std::optional<bool> active, std::optional<f32> bodyWeight,
                         std::optional<i32> bone, std::optional<f32> weight) -> Result<void>
    {
        PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return Err(std::string{ "no physics world" });
        }
        RagdollEntry* entry = nullptr;
        for (RagdollEntry& candidate : impl->ragdolls)
        {
            if (candidate.rig == rig)
            {
                entry = &candidate;
                break;
            }
        }
        if (entry == nullptr)
        {
            return Err(std::string{ "rig has no live ragdoll" });
        }
        if (bodyWeight.has_value())
        {
            std::fill(entry->weightTarget.begin(), entry->weightTarget.end(), std::clamp(*bodyWeight, 0.0f, 1.0f));
        }
        if (bone.has_value() && weight.has_value())
        {
            if (*bone < 0 || static_cast<std::size_t>(*bone) >= entry->weightTarget.size())
            {
                return Err(std::format("bone index {} out of range", *bone));
            }
            entry->weightTarget[static_cast<std::size_t>(*bone)] = std::clamp(*weight, 0.0f, 1.0f);
        }
        if (active.has_value())
        {
            entry->motorsActive = *active;
            if (!*active && entry->ragdoll != nullptr)
            {
                // Going passive: release the motors so the bodies fall under gravity + limits alone.
                for (std::size_t i = 0; i < entry->ragdoll->GetConstraintCount(); i = i + 1)
                {
                    JPH::TwoBodyConstraint* constraint = entry->ragdoll->GetConstraint(static_cast<int>(i));
                    if (constraint != nullptr && constraint->GetSubType() == JPH::EConstraintSubType::SwingTwist)
                    {
                        auto* swingTwist = static_cast<JPH::SwingTwistConstraint*>(constraint);
                        swingTwist->SetSwingMotorState(JPH::EMotorState::Off);
                        swingTwist->SetTwistMotorState(JPH::EMotorState::Off);
                    }
                }
            }
        }
        return {};
    }

    auto ragdollState(const PhysicsWorld& world, u64 rig) -> RagdollState
    {
        RagdollState state;
        const PhysicsWorldImpl* impl = world.impl();
        if (impl == nullptr)
        {
            return state;
        }
        for (const RagdollEntry& entry : impl->ragdolls)
        {
            if (entry.rig != rig)
            {
                continue;
            }
            state.present = true;
            state.active = entry.motorsActive;
            state.bones = static_cast<i32>(entry.weightTarget.size());
            f32 sum = 0.0f;
            for (const f32 w : entry.weightTarget)
            {
                sum += w;
            }
            state.bodyWeight = entry.weightTarget.empty() ? 0.0f : sum / static_cast<f32>(entry.weightTarget.size());
            break;
        }
        return state;
    }

    auto runPhysicsSelfTest() -> Result<void>
    {
        if (auto inited = initPhysics(); !inited)
        {
            return Err(std::string{ "initPhysics failed: " } + inited.error());
        }
        {
            auto world = createPhysicsWorld();
            if (!world)
            {
                shutdownPhysics();
                return Err(std::string{ "createPhysicsWorld failed: " } + world.error());
            }
            const PhysicsWorldStats stats = physicsWorldStats(*world);
            if (!stats.active)
            {
                shutdownPhysics();
                return Err("world is not active after create");
            }
            if (stats.bodyCount != 0)
            {
                shutdownPhysics();
                return Err("a freshly created world should hold no bodies");
            }
        }  // world dropped here — frees the Jolt objects before shutdown
        shutdownPhysics();
        return {};
    }
}
