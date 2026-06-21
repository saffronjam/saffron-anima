+++
title = 'Rigidbody and collider'
weight = 2
+++

# Rigidbody and collider

A simulated object is described by two components, not one. `Collider` says *what shape* the object
is and *how its surface behaves*; `Rigidbody` says *how it moves*. This is the Unity split
(`Collider` / `Rigidbody`), and it keeps the common cases cheap: a floor or a wall is a single
`Collider`, and a falling crate is a collider plus a rigidbody.

## The split, and the collider-alone rule

- **`Collider`** — the `shape` (`Shape::Box` / `Sphere` / `Capsule` / `ConvexHull` / `Mesh`), its
  `half_extents` / `offset`, a `PhysicsMaterial` (`friction`, `restitution`), and an `is_sensor`
  flag. The shape **auto-fits** to the entity's mesh AABB when the component is added (the locked
  decision — editable after).
- **`Rigidbody`** — the motion type (`Motion::Static` / `Kinematic` / `Dynamic`), `mass`, linear and
  angular damping, and a `gravity_factor`.

The rule that ties them together: **a `Collider` with no `Rigidbody` is an implicit Static body.**
Floors and walls are one component. Add a `Rigidbody` and its motion type wins — `Dynamic` moves
under gravity and contacts, `Kinematic` is driven by script or animation, `Static` never moves. The
mapping is `MotionType::from_scene`, which folds the scene `Motion` enum onto the Jolt motion type.

Both components live in `saffron-scene` (so they serialize and reach the editor through the generic
component registry); `saffron-physics` only *consumes* them.

## Component → body → step → write-back

This is the load-bearing loop, and it runs entirely inside the existing play tick — no new render
pass:

1. **Build** — on the `Edit → Playing` edge, `World::populate` walks every `Collider`, builds a Jolt
   shape, reads the entity's current world transform for the body's initial pose, maps the
   (optional) rigidbody's motion type, mass, damping, and gravity factor onto a `BodyCreate`, and
   creates one Jolt body per entity. Bodies are tracked in **creation order** (a `Vec<BodyEntry>`,
   never a map iteration) because that order is load-bearing for the deterministic sim.
2. **Step** — inside the host's `sim_tick` seam (composed **physics-then-scripts**, so a script
   reading a body's transform sees this frame's settled physics), `World::step` advances the world
   with a **fixed-step accumulator**: it adds the frame's clamped `dt` to an accumulator and runs one
   Jolt update per whole `FIXED_STEP` elapsed, capped at `MAX_SUBSTEPS` so a runaway `dt` cannot
   spiral. Fixed substeps keep the simulation frame-rate independent and bit-exact under the
   cross-platform-deterministic build.
3. **Write back** — after stepping, each Dynamic body's world pose is written into its entity's
   **local** `Transform` (`translation`, and `rotation` as a `Quat`). The later
   `update_world_transforms` pass recomposes the cached world matrix from the written local, exactly
   as it does for any edited transform — so the mesh follows the same frame.

Physics writes the **local** `Transform`, never the cached `WorldTransform` (that is overwritten
every frame). Dynamic bodies are scoped to **root** entities (world == local); the parented-body
local rebase is a later refinement.

## Play is the lifetime; stop is the restore

Bodies are created against the play-scene duplicate and die with the world when play stops. The
authored scene is never written during play, so there is no authored-transform reset — stopping play
discards the duplicate and the authored values stand untouched. A second play repopulates a fresh
world.

## What | File | Symbols

| What | File | Symbols |
|---|---|---|
| The two components + material | `engine/crates/scene/src/component.rs` | `Rigidbody`, `Collider`, `Motion`, `PhysicsMaterial` |
| Body creation + step + write-back | `engine/crates/physics/src/world.rs` | `World::populate`, `World::step`, `BodyEntry` |
| The motion-type mapping | `engine/crates/physics/src/types.rs` | `MotionType`, `MotionType::from_scene`, `FIXED_STEP` |
| sim_tick composition + lifecycle | `engine/crates/host/src/layer.rs` | `HostLayer::reconcile_play_edge`, the `sim_tick` closure |
| Auto-fit on add | `engine/crates/physics/src/world.rs` | `fit_collider_to_mesh` |
| World summary | `engine/crates/control/src/commands_physics.rs` | `physics-state` |
