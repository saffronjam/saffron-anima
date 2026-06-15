+++
title = 'Active ragdoll'
weight = 9
+++

# Active ragdoll

The passive ragdoll lets a body drive a bone and collapse under gravity. The active ragdoll closes
the loop the other way: constraint **motors** pull the bodies *back* toward the animated pose, and a
per-bone weight mixes physics against animation through the **same** `PoseBuffer.override_`/`weight`
blend layer foot IK and the passive ragdoll already use. The result is the spectrum a hit reaction
needs — limp, fully driven, or anything between, per bone — with no new pose path.

## The spectrum: passive, active, partial

A ragdoll is one Jolt `Ragdoll` whose parts mirror `SkinnedMeshComponent.bones` 1:1. Two independent
dials decide how it behaves, and both live per bone:

- **Motors (`active`)** — whether the joint's motor is driven toward the animation each step. Off is a
  passive limp (gravity + limits only); on is a body that tracks the clip.
- **Weight (`bodyWeight` / per-bone `weight`)** — how much the *bone* follows physics vs. animation,
  `0` = pure animation, `1` = pure physics. The evaluator eases the live weight toward this target so
  a limb blends in and out without a pop.

Passive full-weight is the collapse of the previous page. Active full-weight is a body that holds the
animated pose against gravity. **Partial** — upper-body weight `1` while the legs stay at `0` — is a
character that takes an impact in the chest while still running, which is the headline UE Physical
Animation result this mirrors.

## Motors read the authored PD gains

`BonePhysics` has carried `driveStiffness` / `driveDamping` / `driveMaxForce` per bone the whole time,
authored-but-inert. They are baked into each `SwingTwistConstraint`'s swing + twist
`MotorSettings` (frequency, damping, torque limit) at ragdoll build — so a freshly imported,
auto-fit rig already has sane motors. Each fixed step, before `PhysicsSystem::Update`,
`driveRagdollsToPose` walks every **active** ragdoll and, for each `SwingTwist` joint, sets the motor
state to `Position` and the target orientation to the bone's local rotation from this frame's
animation target (`SetTargetOrientationBS`). A `Free` (or `Hinge`/`Fixed`) joint carries no swing
motor and stays limp under drive — the desired "this limb is dead, the rest is driven" behaviour.

The animation target is the rig's `AnimationRuntime.lastPose` — the post-IK local pose the evaluator
produced this frame, in bone order. The Host builds one `PoseTarget` per animated rig and hands it to
`driveRagdollsToPose`; this is the reserved `lastPose` snapshot finally becoming load-bearing.

> [!NOTE]
> Motors restore a joint's *relative* orientation, not the unconstrained root's world position. A
> free ragdoll whose root has fallen is re-*posed* by the motors but not stood back up — recovering a
> character to a standing pose needs a kinematic root anchor (the character controller's job),
> deferred. Recovering the *bone* to the animated pose is the weight blend below, which always works.

## The weight blend is the recover

After the step, `writeRagdollPoses` converts each part's world transform to the bone's local TRS and
writes it into `PoseOverrideComponent` — but mixed by the bone's eased weight. At weight `1` it
overwrites; below `1` it `mix`/`slerp`s physics over the animation pose the evaluator wrote into the
override earlier this frame. `advanceRagdollBlend` eases the live weight toward the target each step at
`RagdollBlend.rate`, so ramping `bodyWeight` from `1` back to `0` slides the bone from the collapsed
physics pose back onto the clip — **a hit blows a limb to physics and it recovers to the animation**.
Because the evaluator rewrites the override from the clip every frame, the mix always starts from a
fresh animation pose, never a stale physics one — there is no drift to accumulate.

## Authoring: auto-fit on import, then hand-tune

A `BonePhysicsComponent` is auto-fit on skinned import — a capsule per bone sized from the rest
joint-to-child distance, `SwingTwist` joints, unit mass — so a rig is ragdoll-ready the instant it
loads (the locked auto-fit decision, mirroring `ColliderComponent`). Hand-edit a single bone's field
afterwards with `set-component-field {entity, "BonePhysics", field, index, value}`: the `index`
addresses one element of the `bones` array, so `{field: "joint", index: 0, value: "Free"}` makes the
pelvis limp without disturbing the rest.

## Driving it: `set-ragdoll` / `get-ragdoll`

`set-ragdoll {entity, active?, bodyWeight?, bone?, weight?}` drives the blend: it auto-creates the
ragdoll on first call (so a hit "just works" with no separate `enable-ragdoll`), flips the motors with
`active`, sets a uniform target with `bodyWeight`, or targets one limb with `bone`+`weight`. A hit
reaction is `set-ragdoll {bone, weight: 1}` on the struck limb, left to ease back down to its region's
authored target. `get-ragdoll {entity}` reports presence, the active flag, the mean weight, and the
bone count. Both are scriptable from `sa` and bump `animationVersion` so the editor reconciles.

## What | File | Symbols

| What | File | Symbols |
|---|---|---|
| Motor drive + blend + state | `engine/source/saffron/physics/physics.cpp` | `driveRagdollsToPose`, `advanceRagdollBlend`, `writeRagdollPoses`, `setRagdollBlend`, `ragdollState`, `boneMotorSettings` |
| Per-frame composition | `engine/source/saffron/host/host.cppm` | the `simTick` seam (`driveRagdollsToPose` -> `advanceRagdollBlend` -> step -> `writeRagdollPoses`) |
| Drive commands | `engine/source/saffron/control/control_commands_physics.cpp` | `set-ragdoll`, `get-ragdoll`, `ragdollResultFor` |
| Per-bone field edit | `engine/source/saffron/control/control_commands_scene.cpp` | `set-component-field` (the `index` element merge) |
| Import auto-fit | `engine/source/saffron/assets/assets.cppm` | `spawnSkinnedModel` (the `BonePhysicsComponent` fit) |
