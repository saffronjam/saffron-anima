module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

export module Saffron.Physics;

export import :Types;

import Saffron.Core;
import Saffron.Geometry;
import Saffron.Scene;
import Saffron.Animation;

export namespace se
{
    struct PhysicsWorldImpl;  // defined in physics.cpp; owns the Jolt objects

    /// The per-play-session physics world: wraps Jolt's PhysicsSystem + BodyInterface +
    /// TempAllocator + JobSystem and (later phases) the entity<->BodyID maps and a fixed-step
    /// accumulator. Owned by HostState, built on Edit->Playing, dropped on ->Edit.
    /// Move-only RAII: the destructor removes all bodies and frees the Jolt objects.
    class PhysicsWorld
    {
      public:
        PhysicsWorld(PhysicsWorld&&) noexcept;
        PhysicsWorld& operator=(PhysicsWorld&&) noexcept;
        PhysicsWorld(const PhysicsWorld&) = delete;
        PhysicsWorld& operator=(const PhysicsWorld&) = delete;
        ~PhysicsWorld();

        /// Non-null while the world is alive; the impl is the only holder of Jolt types.
        auto impl() -> PhysicsWorldImpl*
        {
            return impl_.get();
        }
        auto impl() const -> const PhysicsWorldImpl*
        {
            return impl_.get();
        }

      private:
        PhysicsWorld() noexcept;  // only createPhysicsWorld constructs one
        std::unique_ptr<PhysicsWorldImpl> impl_;
        friend auto createPhysicsWorld() -> Result<PhysicsWorld>;
    };

    /// One-time Jolt global init (idempotent): RegisterDefaultAllocator, install the
    /// Factory, RegisterTypes. Called once before the first world is built.
    auto initPhysics() -> Result<void>;

    /// Tear down the Jolt globals: UnregisterTypes + delete the Factory. Pairs with initPhysics.
    void shutdownPhysics();

    /// Allocate an empty Jolt world (PhysicsSystem::Init with v1 limits, the two-layer
    /// filters, single-precision, cross-platform deterministic). No bodies yet.
    auto createPhysicsWorld() -> Result<PhysicsWorld>;

    /// A Jolt-free summary the control plane echoes.
    auto physicsWorldStats(const PhysicsWorld& world) -> PhysicsWorldStats;

    /// Snapshot every live body (owner entity, motion, active state, world position) via read-only
    /// BodyInterface getters — never perturbs the deterministic sim. Empty when the world is null.
    auto listPhysicsBodies(const PhysicsWorld& world) -> std::vector<PhysicsBodyInfo>;

    /// Push a Dynamic body (by entity uuid): an instantaneous impulse, a continuous force (until the next
    /// step), or an absolute linear velocity — activating the body. A non-Dynamic / unmapped body is a
    /// no-op. These map uuid -> BodyID through the existing entity<->body map (the same raycastWorld uses)
    /// and must run BETWEEN steps (a command or on_update), never from a contact handler mid-solve.
    void applyBodyImpulse(PhysicsWorld& world, u64 entity, glm::vec3 impulse);
    void addBodyForce(PhysicsWorld& world, u64 entity, glm::vec3 force);
    void setBodyLinearVelocity(PhysicsWorld& world, u64 entity, glm::vec3 velocity);
    auto bodyLinearVelocity(const PhysicsWorld& world, u64 entity) -> glm::vec3;

    /// Decodes a baked mesh to CPU vertices for convex-hull / mesh-shape cooking. The Host binds it
    /// to loadMeshCpuAsset, keeping <Jolt/...> out of Saffron.Assets and the asset reader out of the
    /// Jolt TU. Jolt-free `Mesh` (Saffron.Geometry) crosses the seam.
    using MeshCookSource = std::function<Result<Mesh>(Uuid)>;

    /// Walk `scene` for every ColliderComponent (+ optional RigidbodyComponent), create a Jolt body
    /// per entity, and record the entity<->BodyID mapping. A collider without a rigidbody becomes a
    /// Static body; with one, its Motion maps to EMotionType {Static,Kinematic,Dynamic}. Analytic
    /// shapes size from the component; ConvexHull/Mesh cook from `cook`. Called on the Edit->Playing
    /// edge, against the play-scene duplicate.
    void populatePhysicsWorld(PhysicsWorld& world, Scene& scene, const MeshCookSource& cook);

    /// For each rig with an enabled KinematicBonesComponent, create one Kinematic capsule body per
    /// driven joint (sized from BonePhysics.shapeHalfExtents), keyed by the joint entity so it tears
    /// down with the world on stop. The bodies follow the animated pose each step (stepPhysics drives
    /// every Kinematic body) — independent colliders, no constraints (that is the ragdoll).
    void buildBoneBodies(PhysicsWorld& world, Scene& scene);

    /// Create a Jolt CharacterVirtual for `entity` from its capsule ColliderComponent + the
    /// CharacterControllerComponent's params. Stepped each fixed substep (gravity + the
    /// move-character desired velocity, with stick-to-floor + WalkStairs) inside stepPhysics, which
    /// writes the resolved position back into the entity-root TransformComponent (binding mode a).
    auto addCharacter(PhysicsWorld& world, Entity entity, Scene& scene) -> Result<void>;

    /// Step the world by `dt` using a fixed-step accumulator (PhysicsFixedStep), then write each
    /// Dynamic body's world transform back into its entity TransformComponent (root entities this
    /// phase: world == local). Called once per frame inside the simTick seam, before renderScene.
    /// Also drains the contact listener's buffered pairs into the seq-stamped event ring.
    void stepPhysics(PhysicsWorld& world, Scene& scene, f32 dt);

    /// One contact/overlap transition, seq-stamped, drained over a non-blocking cursor. Sensor
    /// overlaps and solid touches share one ring; `sensor` distinguishes them.
    struct ContactEvent
    {
        u64 seq = 0;
        enum class Kind : u8
        {
            Begin,  // OnContactAdded
            End     // OnContactRemoved
        } kind = Kind::Begin;
        u64 entityA = 0;           // IdComponent uuid of one body's entity (0 = none)
        u64 entityB = 0;           // the other entity's uuid
        bool sensor = false;       // either body is a sensor (a trigger overlap, not a solid touch)
        glm::vec3 point{ 0.0f };   // a representative world contact point; zero for an End event
        glm::vec3 normal{ 0.0f };  // world contact normal (A -> B); zero for an End event
        i64 tick = 0;              // the physics step the contact fired on
    };

    /// Snapshot of contact events with seq > since (non-blocking), plus cursor metadata that lets a
    /// stale cursor detect it missed evicted events (the drain-alarms / drain-script-errors shape).
    struct ContactDrain
    {
        std::vector<ContactEvent> events;
        u64 highWaterSeq = 0;
        u64 oldestSeq = 0;
        bool overflowed = false;
    };

    auto drainContacts(const PhysicsWorld& world, u64 since) -> ContactDrain;

    /// Cast a ray from `origin` along `dir` (need not be normalized) up to `maxDist`. Returns the
    /// closest hit, mapping the hit body back to its entity uuid. Read-only — does not perturb the
    /// deterministic step; must run between steps (a command between frames, or in on_update).
    auto raycastWorld(const PhysicsWorld& world, glm::vec3 origin, glm::vec3 dir, f32 maxDist) -> PhysicsRayHit;

    /// Sweep a sphere of `radius` from `origin` along `dir` up to `maxDist` (a thicker probe that
    /// tolerates edges a thin ray misses). Same hit mapping as raycastWorld.
    auto sphereCastWorld(const PhysicsWorld& world, glm::vec3 origin, glm::vec3 dir, f32 radius, f32 maxDist)
        -> PhysicsRayHit;

    /// Build a Jolt Ragdoll from the rig's BonePhysicsComponent (per-bone shapes + per-joint
    /// constraints) + SkinnedMeshComponent skeleton, seed each body at the rig's current world bone
    /// transform (spawns on the animated pose), and add it to the world. Idempotent per rig.
    auto enableRagdoll(PhysicsWorld& world, Scene& scene, Entity rig) -> Result<void>;

    /// Remove the rig's ragdoll from the world and forget it. Idempotent.
    void disableRagdoll(PhysicsWorld& world, u64 rig);

    /// After PhysicsSystem::Update: for each live ragdoll, read each part's world transform, convert
    /// to the bone's LOCAL TRS (the inverse of jointMatrices' composition), and write it into the
    /// bone's PoseOverrideComponent blended by the per-bone weight (1 = pure ragdoll). Runs before
    /// renderScene composes the skeleton.
    void writeRagdollPoses(PhysicsWorld& world, Scene& scene);

    /// Whether the rig has a live ragdoll instance this play session.
    auto hasRagdoll(const PhysicsWorld& world, u64 rig) -> bool;

    /// A rig's per-frame animation target: the post-IK local TRS pose the evaluator produced,
    /// indexed 1:1 with SkinnedMeshComponent.bones (the same order the ragdoll skeleton was built
    /// from). Keyed by the rig mesh entity uuid. Drives an active ragdoll's motors toward animation.
    struct PoseTarget
    {
        u64 rig = 0;
        std::vector<JointPose> local;
    };

    /// Drive every active ragdoll's SwingTwist motors toward its rig's animation target (set the
    /// motor state to Position + the per-joint target rotation). Passive ragdolls, untargeted rigs,
    /// and non-SwingTwist joints are left to swing freely. Call once per fixed step, before stepPhysics.
    void driveRagdollsToPose(PhysicsWorld& world, const std::vector<PoseTarget>& targets);

    /// Ease every ragdoll's per-bone physics weight toward its target at the entry's rate so the
    /// animation<->physics blend ramps without a pop. Call once per fixed step, before writeRagdollPoses.
    void advanceRagdollBlend(PhysicsWorld& world, f32 dt);

    /// Set a rig's active-ragdoll blend: motors on/off (`active`), a uniform per-bone target weight
    /// (`bodyWeight`, 0 = pure animation, 1 = pure physics), or one bone's target weight
    /// (`bone` >= 0 with `weight`). A hit reaction is `bone`+`weight` left to ease back down. Fails
    /// if the rig has no live ragdoll.
    auto setRagdollBlend(PhysicsWorld& world, u64 rig, std::optional<bool> active, std::optional<f32> bodyWeight,
                         std::optional<i32> bone, std::optional<f32> weight) -> Result<void>;

    /// A rig's live ragdoll state: presence, the motor-active flag, the mean target weight across
    /// bones, and the bone count. All-default (absent) when the rig has no ragdoll.
    struct RagdollState
    {
        bool present = false;
        bool active = false;
        f32 bodyWeight = 0.0f;
        i32 bones = 0;
    };
    auto ragdollState(const PhysicsWorld& world, u64 rig) -> RagdollState;

    /// Headless unit gate: initPhysics -> createPhysicsWorld -> drop -> shutdownPhysics,
    /// asserting each step's Result is ok. Run from the SAFFRON_SELFTEST block.
    auto runPhysicsSelfTest() -> Result<void>;
}
