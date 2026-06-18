# Phase 8 — Kinematic bone bodies following the animated pose

**Status:** COMPLETED
**Depends on:** 05-physics-jolt-bridge:phase-7, 04-animation (the animated pose / `SkinnedMeshComponent` bone handles)

## Goal

Implement kinematic bone-following: for each rig with an enabled `KinematicBonesComponent`, create one
Kinematic capsule body per driven joint, sized from `BonePhysics.shapeHalfExtents`, that follows the
animated pose each fixed step via `MoveKinematic` — so a moving animated character shoves the dynamic
world (binding mode b, animation→physics, no pose write-back). Independent colliders, no constraints
(that is the ragdoll, phase 9).

## Why this shape (NO LEGACY)

`buildBoneBodies` (`physics.cpp:844`) walks `KinematicBonesComponent`, picks the driven joints (empty
`driven` = all), and creates a Kinematic capsule per joint keyed by the joint entity (so it tears down
with the world). The fixed-step loop's `MoveKinematic` branch (`physics.cpp:979`) drives every Kinematic
body toward its entity's current `worldPose` using the same `PhysicsFixedStep` that feeds `Update`, so
the swept motion imparts contact velocity to the dynamics it hits (never a teleport, which gives zero
contact velocity — a load-bearing subtlety, `physics.cpp:976`). `worldPose` composes the world matrix
fresh (`composeWorldMatrix`, `physics.cpp:174`) rather than reading the one-frame-stale
`WorldTransformComponent` — the most likely source of a follow-lag bug, so the Rust port composes fresh
too. These bodies share `World.bodies` with everything else (creation order preserved). The
`set-kinematic-bones` toggle (control command) flips `enabled`; the bodies are rebuilt on the next play
edge.

## Grounding (real files/symbols)

- `engine-old/source/saffron/physics/physics.cpp:844-922` — `buildBoneBodies`: skip disabled / no
  `SkinnedMeshComponent`; the `driven` predicate (empty = all); per-joint capsule from
  `phys->bones[index].shapeHalfExtents` (radius `.x`, half-height `.y`, 0.03 default); Kinematic, Moving
  layer; `CreateAndAddBody`; `indexByBodyId` + `bodies` push with `MotionType::Kinematic`.
- `engine-old/source/saffron/physics/physics.cpp:979-987` — the `MoveKinematic` branch (valid-entity
  check, fresh `worldPose`, `MoveKinematic(id, pos, rot, PhysicsFixedStep)`).
- `engine-old/source/saffron/physics/physics.cpp:174-184` — `worldPose` (compose-fresh, scale divided
  out, `quat_cast`).
- `engine-old/source/saffron/scene/scene.cppm:235-239` — `KinematicBonesComponent` (enabled, driven
  indices).
- `engine-old/source/saffron/control/control_commands_physics.cpp:190` — the `set-kinematic-bones`
  command (resolves to the rig entity, toggles `enabled`).

## Work

- `saffron-physics`: `World::build_bone_bodies(&mut self, scene)` — the `KinematicBonesComponent` walk,
  the `driven` predicate, per-joint Kinematic capsule creation, recorded in `bodies` as
  `MotionType::Kinematic`.
- The `MoveKinematic` branch in `World::step` (already stubbed in phase 3) becomes live: drive every
  Kinematic body toward its entity's fresh `world_pose` each substep. `world_pose(scene, entity)` ported
  (compose-fresh via the scene's `compose_world_matrix`, scale divided out — area 03 provides it).
- Confirm the bone bodies tear down with the world (they live in `World.bodies`, dropped with the
  `UniquePtr`).

## Acceptance gate

- `cargo build -p saffron-physics` succeeds.
- A `#[test]` `kinematic_bone_shoves_dynamics` builds a one-bone kinematic body, animates it toward a
  resting dynamic box, steps, and asserts the box gained velocity in the sweep direction (contact
  velocity imparted, not a teleport).
- A `#[test]` `driven_subset` asserts a non-empty `driven` list creates exactly the listed joints'
  bodies; an empty list creates one per bone.
- The determinism trace (phase 5) extended with a kinematic-bones rig produces a stable cross-run hash.
