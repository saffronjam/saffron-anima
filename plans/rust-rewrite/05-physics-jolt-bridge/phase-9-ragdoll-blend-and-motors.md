# Phase 9 — The motor-driven ragdoll blend layer (active/partial via PoseBuffer override/weight)

**Status:** COMPLETED
**Depends on:** 05-physics-jolt-bridge:phase-8, 04-animation (`JointPose`, the evaluator's per-rig animated pose)

## Goal

Complete the ragdoll system from the passive base (phase 4) to the full animation↔physics blend: drive
each active ragdoll's SwingTwist motors toward the rig's per-frame animated pose, ease the per-bone
physics weight, write each ragdoll part's resolved world transform back into the bone's
`PoseOverrideComponent` blended by that weight, and expose `set_ragdoll_blend` (motors on/off, uniform
or per-bone target weight) + `ragdoll_state`. This is the passive/active/partial ragdoll the engine
ships.

## Why this shape (NO LEGACY)

This is the heart of the ragdoll's value and the trickiest math, so it lands after the gate confirms the
base sim is deterministic. Four functions compose per fixed step (the order is fixed by the host's
`simTick`, `host.cppm:1146`): `drive_ragdolls_to_pose` (before the step, so motors are read during the
solve), `advance_ragdoll_blend` (ease weights), `step` (solve), `write_ragdoll_poses` (after the step).

- **Drive** (`driveRagdollsToPose`, `physics.cpp:1384`): for an active ragdoll with an animation target,
  per bone, find the parent constraint (`GetConstraintIndexForBodyIndex`), and if it is a SwingTwist, set
  both motor states to `Position` and `SetTargetOrientationBS(target.local[i].rotation)`. A Free/Hinge
  bone stays limp; a passive ragdoll or untargeted rig swings freely. The glam quaternion feeds
  `SetTargetOrientationBS` directly (no swizzle).
- **Ease** (`advanceRagdollBlend`, `physics.cpp:1431`): move each `weight_current` toward `weight_target`
  by `weight_rate * dt` (clamped step), so the blend ramps without a pop.
- **Write-back** (`writeRagdollPoses`, `physics.cpp:1318`): read each part's world transform, compute
  bone-local = `inverse(parent_world) * part_world` (parent = the parent part's world, or the rig world
  for the root), decompose to TRS, and write into the bone's `PoseOverrideComponent` — at weight ≥ 0.999
  overwrite; else `mix`/`slerp` over the animation pose the evaluator wrote earlier this frame. glam:
  `Mat4::inverse`, `to_scale_rotation_translation`, `Quat::slerp`, `Vec3::lerp`.
- **Blend control** (`setRagdollBlend`, `physics.cpp:1451`): set `motors_active`; uniform `body_weight`
  fills `weight_target`; a `(bone, weight)` pair sets one bone (a hit reaction left to ease back); going
  passive releases every SwingTwist motor to `Off`.

`PoseTarget` (`physics.cppm:171`) is the per-rig animation target (rig uuid + `Vec<JointPose>` in
bones-order); the host builds it from the evaluator's `lastPose` (`host.cppm:1149`). One code path; the
C++ logic ports field-for-field with glam replacing GLM.

## Grounding (real files/symbols)

- `engine-old/source/saffron/physics/physics.cpp:1384-1429` — `driveRagdollsToPose`
  (`GetConstraintIndexForBodyIndex`, SwingTwist subtype check, `SetSwingMotorState`/`SetTwistMotorState`
  = Position, `SetTargetOrientationBS`).
- `engine-old/source/saffron/physics/physics.cpp:1431-1449` — `advanceRagdollBlend` (the eased
  `weight_current` toward `weight_target` by `weight_rate * dt`).
- `engine-old/source/saffron/physics/physics.cpp:1318-1382` — `writeRagdollPoses` (read part worlds,
  `inverse(parentWorld) * partWorld`, decompose, weight-based overwrite vs `mix`/`slerp` into
  `PoseOverrideComponent`).
- `engine-old/source/saffron/physics/physics.cpp:1451-1503` — `setRagdollBlend` (active flag,
  body/bone weight targets, passive → motors Off); `ragdollState` (`:1505`).
- `engine-old/source/saffron/physics/physics.cppm:171-202` — `PoseTarget`, `RagdollState`,
  `driveRagdollsToPose`/`advanceRagdollBlend`/`setRagdollBlend`/`ragdollState`/`writeRagdollPoses`
  signatures.
- `engine-old/source/saffron/physics/physics.cpp:530-541` — `RagdollEntry` (`weightTarget`,
  `weightCurrent`, `weightRate = 6.0`, `motorsActive`, `parentIndex`).
- `engine-old/source/saffron/scene/scene.cppm:128-133` — `PoseOverrideComponent` (translation/rotation/
  scale).
- `engine-old/source/saffron/host/host.cppm:1146-1160` — the per-frame compose order (drive → advance →
  step → write).

## Work

- Shim: SwingTwist motor-state + target-orientation setters, `GetConstraintIndexForBodyIndex`,
  per-constraint subtype query, ragdoll part world-transform + body-count getters (most added in phase 4
  for the bare ragdoll; this phase exercises the motor setters).
- `saffron-physics`: `PoseTarget` (rig uuid + `Vec<JointPose>`), `RagdollState`.
  `World::drive_ragdolls_to_pose(&mut self, targets: &[PoseTarget])`,
  `advance_ragdoll_blend(&mut self, dt)`, `write_ragdoll_poses(&mut self, scene)`,
  `set_ragdoll_blend(&mut self, rig, active: Option<bool>, body_weight: Option<f32>, bone: Option<i32>,
  weight: Option<f32>) -> Result<()>` (typed `Error::NoRagdoll`, `Error::BoneOutOfRange`),
  `ragdoll_state(&self, rig) -> RagdollState`.
- The world→local decompose via glam; the weight-based overwrite-vs-blend write into
  `PoseOverrideComponent` (add the component if absent, mirroring `addComponent<PoseOverrideComponent>`).

## Acceptance gate

- `cargo build -p saffron-physics` succeeds.
- A `#[test]` `active_ragdoll_tracks_pose` enables a ragdoll, sets it active with a fixed `PoseTarget`,
  steps, and asserts the motored bones converge toward the target rotation (error decreases over steps).
- A `#[test]` `partial_blend` sets one bone's weight to 0.5 and asserts that bone's
  `PoseOverrideComponent` is between the animation pose and the physics pose, while a weight-1 bone is
  pure physics.
- A `#[test]` `passive_release` sets active=false and asserts the SwingTwist motors are Off (the body
  falls freely); `ragdoll_state` reports the mean weight and bone count.
- A `#[test]` `set_ragdoll_blend_errors` asserts a missing rig → `Error::NoRagdoll` and an out-of-range
  bone → `Error::BoneOutOfRange`.
