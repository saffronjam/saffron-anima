+++
title = 'Kinematic bodies and bone following'
weight = 5
+++

# Kinematic bodies and bone following

A **Kinematic** body is one the simulation does not push but *pushes back from*. It ignores gravity
and contacts — its motion is set explicitly each step — yet a dynamic body that hits it bounces off
correctly, because the kinematic body's swept motion imparts the right contact velocity. This is how
a moving platform carries a crate, and how a walking character's limbs shove the world around.

## The three binding modes

A skeleton and a physics body can be wired together three ways. Naming them keeps the modes straight:

- **(a) static** — the body *is* the character's collision proxy; animation plays independently on
  top (the character controller).
- **(b) animation → physics** — per-bone kinematic bodies *follow* the animated pose, so the world
  reacts to a moving character. **This is what this page covers.**
- **(c) physics → animation** — the body drives the bone (the ragdoll), writing back into the pose.

Mode (b) is strictly one-way: animation is authoritative for the skeleton, and physics only *reads*
it. The `PoseOverride` blend layer is left untouched here — that seam is mode (c)'s.

## MoveKinematic, never a teleport

The simulation moves a kinematic body with `saffron_physics_sys::move_kinematic` (Jolt's
`BodyInterface::MoveKinematic`), which derives the linear + angular velocity that carries the body to
the target over `FIXED_STEP` and integrates it as a swept motion. A teleport (`SetPosition`) would
leave the body's velocity at zero, so a dynamic body it overlaps is resolved as a static penetration
push with no momentum — the crate would *ooze* off the platform instead of getting *hit*. Deriving
velocity from `(target − current)/dt` is the whole point, so the same `FIXED_STEP` feeds both the
kinematic move and the Jolt update and the swept velocity matches the integration step.

Every kinematic body — a free `Rigidbody` whose motion is `Motion::Kinematic`, and every per-bone
body — is driven this way in `move_kinematic_bodies`, each fixed substep, toward its entity's current
world transform.

## Reading the pose: compose, don't trust the cache

The bone target is each joint's animated world transform. The subtle trap: the cached
`WorldTransform` is **one frame stale** during the simulation tick, because the pass that refreshes
it (`update_world_transforms`) runs *after* the update. So the follow step composes the world matrix
itself from the parent chain and the fresh `PoseOverride` the animation evaluator just wrote, rather
than reading the cache — that is what `fresh_world_pose` does. Getting this wrong is the single most
likely source of a one-frame follow lag.

The ordering that makes this work is already fixed: the host's `tick_animation` writes the pose
overrides *before* `tick_play` runs the simulation seam, so by the time the bone bodies read the
skeleton, this frame's pose is in hand.

## Per-bone bodies, auto-fit on add

A rig opts in with a `KinematicBones` component (`enabled` + an optional `driven` index list; empty
means every joint). Adding it auto-fits a capsule per bone into the reserved
`BonePhysics.shape_half_extents` (via `fit_bone_capsules`) — half-height from the joint-to-child rest
distance, radius a fraction of it, with a small default for leaf joints so Jolt never sees a
degenerate capsule. On play, `World::build_bone_bodies` creates one **Kinematic** capsule body per
driven joint, keyed by the joint entity so it tears down with the world on stop. The bodies are
**independent colliders** — no constraints link them; that joint graph is the ragdoll. A rig with
`KinematicBones` is simply a moving collision proxy, which is all "the world reacts to a walking
character" needs.

## What | File | Symbols

| What | File | Symbols |
|---|---|---|
| Kinematic drive (free + bone bodies) | `engine/crates/physics/src/world.rs` | `World::move_kinematic_bodies`, `fresh_world_pose` |
| Per-bone body creation | `engine/crates/physics/src/world.rs` | `World::build_bone_bodies` |
| The MoveKinematic FFI | `engine/crates/physics-sys/src/lib.rs` | `move_kinematic` |
| The opt-in component | `engine/crates/scene/src/component.rs` | `KinematicBones`, `BonePhysics` |
| Auto-fit + toggle | `engine/crates/physics/src/world.rs`, `engine/crates/control/src/commands_physics.rs` | `fit_bone_capsules`, `set-kinematic-bones` |
