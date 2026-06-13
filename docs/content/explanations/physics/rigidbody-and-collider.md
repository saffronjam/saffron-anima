+++
title = 'Rigidbody and collider'
weight = 2
+++

# Rigidbody and collider

A simulated object is described by two components, not one. `ColliderComponent` says *what shape*
the object is and *how its surface behaves*; `RigidbodyComponent` says *how it moves*. This is the
Unity split (`Collider` / `Rigidbody`), and it keeps the common cases cheap: a floor or a wall is a
single `ColliderComponent`, and a falling crate is a collider plus a rigidbody.

## The split, and the collider-alone rule

- **`ColliderComponent`** — the shape (Box this phase; sphere/capsule/hull/mesh land with the shapes
  page), its `halfExtents`/`offset`, a `PhysicsMaterial` (`friction`, `restitution`), and an
  `isSensor` flag. The shape **auto-fits** to the entity's mesh AABB when the component is added
  (the locked decision — editable after).
- **`RigidbodyComponent`** — the motion type (`Static` / `Kinematic` / `Dynamic`), `mass`, linear
  and angular damping, a `gravityFactor`, per-axis position and rotation locks, and a collision
  layer index.

The rule that ties them together: **a `ColliderComponent` with no `RigidbodyComponent` is an
implicit Static body.** Floors and walls are one component. Add a `RigidbodyComponent` and its
motion type wins — `Dynamic` moves under gravity and contacts, `Kinematic` is driven by script or
animation, `Static` never moves.

Both components live in `Saffron.Scene` (so they serialize and reach the editor through the generic
component recipe); `Saffron.Physics` only *consumes* them.

## Component → body → step → write-back

This is the load-bearing loop, and it runs entirely inside the existing play tick — no new render
pass:

1. **Build** — on the `Edit → Playing` edge, `populatePhysicsWorld` walks every `ColliderComponent`,
   builds a Jolt shape, reads the entity's current world transform for the body's initial pose, maps
   the (optional) rigidbody's motion type, mass, damping, gravity factor, and locks onto the body
   creation settings, and creates one Jolt body per entity.
2. **Step** — inside the `simTick` seam (composed **physics-then-scripts**, so a script reading a
   body's transform sees this frame's settled physics), `stepPhysics` advances the world with a
   **fixed-step accumulator**: it adds the frame's clamped `dt` to an accumulator and runs
   `PhysicsSystem::Update(PhysicsFixedStep)` for each whole step elapsed. Fixed substeps keep the
   simulation frame-rate independent and bit-exact under the cross-platform-deterministic build.
3. **Write back** — after stepping, each Dynamic body's world pose is written into its entity's
   **local** `TransformComponent` (`translation`, and `rotation` as Euler angles via
   `glm::eulerAngles`, which round-trips through `transformMatrix`). The later
   `updateWorldTransforms` pass recomposes the cached world matrix from the written local, exactly as
   it does for any edited transform — so the mesh follows the same frame.

Physics writes the **local** `TransformComponent`, never the cached `WorldTransformComponent` (that
is overwritten every frame). This phase scopes dynamic bodies to **root** entities (world == local);
the parented-body local rebase is a later refinement.

## Play is the lifetime; stop is the restore

Bodies are created against the play-scene duplicate and die with the world when play stops. The
authored scene is never written during play, so there is no authored-transform reset — stopping play
discards the duplicate and the authored values stand untouched. A second play repopulates a fresh
world.

## What | File | Symbols

| What | File | Symbols |
|---|---|---|
| The two components + material | `engine/source/saffron/scene/scene.cppm` | `RigidbodyComponent`, `ColliderComponent`, `PhysicsMaterial` |
| Body creation + step + write-back | `engine/source/saffron/physics/physics.cpp` | `populatePhysicsWorld`, `stepPhysics`, `buildBoxShape` |
| simTick composition + lifecycle | `engine/source/saffron/host/host.cppm` | the `onPlayStateChanged` subscription, the `simTick` lambda |
| Auto-fit on add | `engine/source/saffron/control/control_commands_scene.cpp` | `fitColliderToMesh`, `add-component` |
| World summary | `engine/source/saffron/control/control_commands_physics.cpp` | `physics-state` |
