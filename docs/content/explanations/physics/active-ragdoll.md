+++
title = 'Active ragdoll'
weight = 9
+++

# Active ragdoll

The passive ragdoll lets a body drive a bone and collapse under gravity. The active ragdoll closes
the loop the other way: constraint **motors** pull the bodies *back* toward the animated pose, and a
per-bone weight mixes physics against animation through the **same** `PoseOverride` blend layer foot
IK and the passive ragdoll already use. The result is the spectrum a hit reaction needs — limp, fully
driven, or anything between, per bone — with no new pose path.

## The spectrum: passive, active, partial

A ragdoll is one Jolt `Ragdoll` whose parts mirror `SkinnedMesh.bones` 1:1. Two independent dials
decide how it behaves, and both live per bone:

- **Motors (`active`)** — whether the joint's motor is driven toward the animation each step. Off is a
  passive limp (gravity + limits only); on is a body that tracks the clip.
- **Weight (`body_weight` / per-bone `weight`)** — how much the *bone* follows physics vs. animation,
  `0` = pure animation, `1` = pure physics. The world eases the live weight toward this target (at
  `RAGDOLL_WEIGHT_RATE`) so a limb blends in and out without a pop.

Passive full-weight is the collapse of the previous page. Active full-weight is a body that holds the
animated pose against gravity. **Partial** — upper-body weight `1` while the legs stay at `0` — is a
character that takes an impact in the chest while still running, which is the headline UE Physical
Animation result this mirrors.

## Motors read the authored PD gains

`BonePhysics` has carried `drive_stiffness` / `drive_damping` / `drive_max_force` per bone the whole
time, authored-but-inert. They ride the `BonePart` into the shim's `add_ragdoll`, which bakes them
into each `SwingTwistConstraint`'s swing + twist `MotorSettings` at build — so a freshly imported,
auto-fit rig already has sane motors. Each fixed step, before the Jolt update,
`World::drive_ragdolls_to_pose` walks every **active** ragdoll and, for each `SwingTwist` joint, sets
the motor state to drive and the target orientation to the bone's local rotation from this frame's
animation target (Jolt's `SetTargetOrientationBS`; glam and Jolt share quaternion order, so no
swizzle). A `Free` (or `Hinge`/`Fixed`) joint carries no swing motor and stays limp under drive — the
desired "this limb is dead, the rest is driven" behaviour.

The animation target is the rig's `AnimationRuntime.last_pose` — the post-IK local pose the evaluator
produced this frame, in bone order. The host builds one `PoseTarget` per animated rig and hands it to
`drive_ragdolls_to_pose`.

> [!NOTE]
> Motors restore a joint's *relative* orientation, not the unconstrained root's world position. A
> free ragdoll whose root has fallen is re-*posed* by the motors but not stood back up — recovering a
> character to a standing pose needs a kinematic root anchor (the character controller's job),
> deferred. Recovering the *bone* to the animated pose is the weight blend below, which always works.

## The weight blend is the recover

After the step, `World::write_ragdoll_poses` converts each part's world transform to the bone's local
TRS and writes it into `PoseOverride` — but mixed by the bone's eased weight. At/above
`PURE_PHYSICS_WEIGHT` it overwrites; below it it `mix`/`slerp`s physics over the animation pose the
evaluator wrote into the override earlier this frame. `World::advance_ragdoll_blend` eases the live
weight toward the target each step, so ramping `body_weight` from `1` back to `0` slides the bone from
the collapsed physics pose back onto the clip — **a hit blows a limb to physics and it recovers to the
animation**. Because the evaluator rewrites the override from the clip every frame, the mix always
starts from a fresh animation pose, never a stale physics one — there is no drift to accumulate.

The host's `sim_tick` seam composes the per-frame order: `drive_ragdolls_to_pose` →
`advance_ragdoll_blend` → `World::step` → `write_ragdoll_poses`.

## Authoring: auto-fit on import, then hand-tune

A `BonePhysicsComponent` is auto-fit on skinned import — a capsule per bone sized from the rest
joint-to-child distance, `SwingTwist` joints, unit mass — so a rig is ragdoll-ready the instant it
loads (the locked auto-fit decision, mirroring `Collider`). Hand-edit a single bone's field
afterwards with `set-component-field {entity, "BonePhysics", field, index, value}`: the `index`
addresses one element of the `bones` array, so `{field: "joint", index: 0, value: "Free"}` makes the
pelvis limp without disturbing the rest.

## Driving it: `set-ragdoll` / `get-ragdoll`

`set-ragdoll {entity, active?, body_weight?, bone?, weight?}` drives the blend through
`World::set_ragdoll_blend`: it auto-creates the ragdoll on first call (so a hit "just works" with no
separate `enable-ragdoll`), flips the motors with `active`, sets a uniform target with `body_weight`,
or targets one limb with `bone`+`weight`. A hit reaction is `set-ragdoll {bone, weight: 1}` on the
struck limb, left to ease back down to its region's authored target. `get-ragdoll {entity}` reports
the `RagdollState`: presence, the active flag, the mean weight, and the bone count. Both are
scriptable from `sa` and bump `animation_version` so the editor reconciles.

## What | File | Symbols

| What | File | Symbols |
|---|---|---|
| Motor drive + blend + state | `engine/crates/physics/src/world.rs` | `World::drive_ragdolls_to_pose`, `World::advance_ragdoll_blend`, `World::write_ragdoll_poses`, `World::set_ragdoll_blend`, `World::ragdoll_state` |
| The motor + target FFI (C++ shim) | `engine/crates/physics-sys/src/lib.rs`, `shim/jolt_bridge.cpp` | `add_ragdoll`, `ragdoll_set_swing_twist_motor`, `BonePart` |
| The animation target | `engine/crates/physics/src/types.rs`, `engine/crates/animation/src/runtime.rs` | `PoseTarget`, `RagdollState`, `AnimationRuntime` (the `last_pose` snapshot) |
| Per-frame composition | `engine/crates/host/src/layer.rs` | the `sim_tick` seam (drive → advance → step → write) |
| Drive commands | `engine/crates/control/src/commands_physics.rs` | `set-ragdoll`, `get-ragdoll` |
| Per-bone field edit | `engine/crates/control/src/commands_scene.rs` | `set-component-field` |
