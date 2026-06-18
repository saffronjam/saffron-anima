# Phase 4 — CharacterVirtual and a passive SwingTwist ragdoll (the two gate features)

**Status:** COMPLETED
**Depends on:** 05-physics-jolt-bridge:phase-3, 04-animation (the `SkinnedMeshComponent` bone handles + `JointPose`)

## Goal

Bind the two hardest Jolt features the determinism gate must exercise: a `CharacterVirtual` controller
(gravity + desired-velocity, stick-to-floor + WalkStairs via `ExtendedUpdate`) and a **passive**
SwingTwist ragdoll (built from `BonePhysicsComponent`, motors off, falling under gravity + joint
limits). No active motor drive, no weight blend, no pose write-back yet — those are phase 9. This phase
exists so the gate (phase 5) can assert bit-exactness on exactly the spike's pass condition: "one
SwingTwist-motor ragdoll and one `CharacterVirtual::ExtendedUpdate` working through `cxx`" + identical
traces.

## Why this shape (NO LEGACY)

The spike (feasibility §spike step 2) names these two as the features that decide the gate, so they land
*before* it; the richer ragdoll surface (drive/blend/writeback) lands *after* a green gate so a
determinism failure is caught before that work. `CharacterVirtual` is a sweep object, not a body — it is
held in `World.characters: Vec<(Entity, UniquePtr<JoltCharacter>)>` released before the world (mirrors
`PhysicsWorldImpl.characters`, `physics.cpp:564`). The ragdoll build ports `enableRagdoll`
(`physics.cpp:1216`) but stops at "created + added, motors off": the `RagdollSettings` skeleton +
parts + `buildJointConstraint` + `Stabilize` + `CalculateBodyIndexToConstraintIndex` + `CreateRagdoll`,
seeded at each bone's current world pose. The motor *settings* are attached (a SwingTwist carries
`boneMotorSettings`, `physics.cpp:200`) but the motor *state* stays `Off` until phase 9 sets it to
`Position`. The shim must expose `ExtendedUpdate` and the SwingTwist motor-state/target setters; the
glam quaternion (xyzw) feeds `SetTargetOrientationBS` directly with no swizzle.

## Grounding (real files/symbols)

- `engine-old/source/saffron/physics/physics.cpp:924-958` — `addCharacter`: capsule from the entity's
  `ColliderComponent` (radius `.x`, half-height `.y`) with defaults, `maxSlopeAngle` from
  `CharacterControllerComponent`, `CharacterVirtualSettings`, seeded at `worldPose`.
- `engine-old/source/saffron/physics/physics.cpp:990-1024` — the `ExtendedUpdate` branch in `stepPhysics`:
  ground-state check, vertical-velocity integration (`gravityFactor`), horizontal clamp to `maxSpeed`,
  `SetLinearVelocity`, `ExtendedUpdateSettings.mWalkStairsStepUp = maxStepHeight`, `ExtendedUpdate` with
  the Character-layer broad-phase + layer filters, `onGround` write-back; position write-back at `:1050`.
- `engine-old/source/saffron/physics/physics.cpp:1216-1316` — `enableRagdoll`: requires
  `SkinnedMeshComponent` + `BonePhysicsComponent` of matching length; builds `uuid→bone index`,
  `parentIndex` (from `RelationshipComponent.parent`), per-bone world pose; `RagdollSettings` skeleton
  (`AddJoint(to_string(i), parent)`), parts (capsule from `shapeHalfExtents`, Dynamic, Moving layer,
  mass), `mToParent = buildJointConstraint(...)` for non-root; `Stabilize`,
  `CalculateBodyIndexToConstraintIndex`, `CreateRagdoll(0, rigUuid, &system)`, `AddToPhysicsSystem`.
- `engine-old/source/saffron/physics/physics.cpp:200-264` — `boneMotorSettings` (frequency/damping/torque
  defaults), `buildJointConstraint` (Fixed/Hinge/Free/SwingTwist; swing/twist limits with the 0.7 rad
  default; the anchor + twist + plane axes).
- `engine-old/source/saffron/physics/physics.cpp:1175-1214` — `disableRagdoll`
  (`RemoveFromPhysicsSystem` + erase), `hasRagdoll`.
- `engine-old/source/saffron/scene/scene.cppm:159-181` — `BonePhysics` (shapeHalfExtents, mass, joint,
  swingTwistLimits, drive gains), `BonePhysicsComponent`.
- `engine-old/source/saffron/scene/scene.cppm:246-257` — `CharacterControllerComponent`.

## Work

- Shim: expose `JoltCharacter` (create from capsule + slope angle, `ExtendedUpdate`, `GetGroundState`,
  `GetPosition`, `SetLinearVelocity`) and ragdoll construction (`RagdollSettings` builder, per-bone
  capsule part, the four constraint kinds via `buildJointConstraint`, `CreateRagdoll`,
  `AddToPhysicsSystem`, `RemoveFromPhysicsSystem`, body world-transform + body-count getters,
  per-constraint subtype + SwingTwist motor setters).
- `saffron-physics`: `World::add_character(entity, scene) -> Result<()>`; the character branch in
  `step` (gravity integration, clamp, `ExtendedUpdate`, ground state, position write-back).
- `World::enable_ragdoll(scene, rig) -> Result<()>` (built passive, motors off) and
  `disable_ragdoll(rig)` / `has_ragdoll(rig)`. `RagdollEntry` struct (rig uuid, rig entity, the
  `UniquePtr` settings + ragdoll handles, `parent_index`, `weight_target`/`weight_current` defaulted to
  1.0, `weight_rate`, `motors_active = false`).
- The C++ `~PhysicsWorldImpl` ragdoll-detach-before-destroy (`physics.cpp:571`) is implemented in the
  shim's `JoltWorld` destructor (it owns the Jolt teardown order).

## Acceptance gate

- `cargo build -p saffron-physics` succeeds.
- A `#[test]` `character_walks_and_steps` drives a `CharacterVirtual` over a small ledge and asserts it
  stepped up (final y above the ledge) and `on_ground` is true on flat ground.
- A `#[test]` `passive_ragdoll_falls` builds a ragdoll on a simple 3-bone rig, steps it, and asserts the
  parts moved under gravity and stayed within joint limits (no NaN, bounded displacement).
- A `#[test]` `ragdoll_teardown_clean` enables then drops the world with a live ragdoll and asserts no
  panic / no leaked-body assertion (the detach-before-destroy order holds).
