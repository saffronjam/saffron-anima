# Phase 3 — `saffron-physics`: the World, rigidbody core, and the fixed-step loop

**Status:** COMPLETED
**Depends on:** 05-physics-jolt-bridge:phase-2, ecs:world (saffron-scene components + `forEach`/world helpers), 02-math-and-geometry (glam, `Mesh`)

## Goal

Build the safe `saffron-physics` crate: the `World` struct, body creation from
`ColliderComponent`/`RigidbodyComponent` (Box shape only this phase, sized from `halfExtents`), the
fixed-step accumulator loop with dynamic-body transform write-back, and the read-only surface
(`stats`, `list_bodies`, `body_linear_velocity`) plus the impulse/force/velocity mutators. This is the
deterministic spine the gate runs on; the richer shapes/sensors/bones land after the gate.

## Why this shape (NO LEGACY)

`PhysicsWorldImpl` is split: the Jolt-owning half lives in the `-sys` `JoltWorld` (phase 2); the
Rust-side bookkeeping (`bodies`, `index_by_body_id`, `contact_ring`, counters, accumulator) lives in
`saffron-physics::World`. `World` owns the `UniquePtr<JoltWorld>` and its own `Drop` is just dropping
that handle — the Jolt teardown order is the shim's job (`~PhysicsWorldImpl`, `physics.cpp:571`), so the
Rust side never sequences Jolt destruction. `bodies` is a `Vec<BodyEntry>` in **creation order** (never
a map iteration) because that order is load-bearing for the deterministic sim (`physics.cpp:517`); the
`HashMap<BodyId, usize>` is only for hit→entity lookup. The C++ no-op-on-null mutators
(`applyBodyImpulse` et al. early-return when `impl == nullptr`) become `&mut World` methods — "no world"
is a type-level impossibility, the `Option<World>` lives in the host. `glm::eulerAngles` write-back
(`physics.cpp:1046`) ports to glam, but note: the write-back stores rotation as a quaternion in the Rust
`TransformComponent` (per area 03's component design) rather than re-deriving Euler — the C++ stored
Euler only because its `TransformComponent.rotation` was Euler; one code path, the component's actual
shape wins.

## Grounding (real files/symbols)

- `engine-old/source/saffron/physics/physics.cpp:631-644` — `createPhysicsWorld` (the Init params,
  gravity, listener wiring — now in the shim, phase 2; this phase wraps it).
- `engine-old/source/saffron/physics/physics.cpp:518-525` — `BodyEntry { entity, uuid, id, motion,
  sensor }`.
- `engine-old/source/saffron/physics/physics.cpp:766-842` — `populatePhysicsWorld`: skip
  `CharacterControllerComponent` entities; collider-without-rigidbody → Static; `resolveObjectLayer`
  (`:283`), `allowedDOFs` (`:310`), `toJoltMotion` (`:266`); `BodyCreationSettings` (sensor flag,
  friction/restitution, damping/gravityFactor/mass/DOFs for Dynamic); `worldTranslation`/`worldRotation`
  (scale-free placement); `CreateAndAddBody`; the `indexByBodyId` + `bodies` push; `dynamicBodyCount`.
- `engine-old/source/saffron/physics/physics.cpp:960-1058` — `stepPhysics`: the accumulator loop
  (`PhysicsFixedStep` `1/60`, `maxSubsteps = 8`), `MoveKinematic` per kinematic body (phase 8 populates
  them), `system.Update(PhysicsFixedStep, 1, …)`, the dynamic write-back
  (`GetPositionAndRotation` → `TransformComponent`).
- `engine-old/source/saffron/physics/physics.cpp:646-764` — `physicsWorldStats`, `listPhysicsBodies`
  (read-only `BodyInterface` getters), `dynamicBodyId` (`:684`), `applyBodyImpulse`/`addBodyForce`/
  `setBodyLinearVelocity` (activate + apply, Dynamic-only with a warn on miss), `bodyLinearVelocity`.
- `engine-old/source/saffron/physics/physics_types.cppm:43` — `PhysicsFixedStep = 1/60` (matches
  SceneEdit `PlayFixedStep`).
- `engine-old/source/saffron/scene/scene.cppm:187-229` — `RigidbodyComponent` (motion, mass, damping,
  gravityFactor, lockPosition/Rotation, collisionLayer), `ColliderComponent` (shape, halfExtents,
  sourceMesh, offset, material, isSensor), `PhysicsMaterial`.

## Work

- Define `saffron-physics::Error` (thiserror) + `Result<T>`, and the POD result types `WorldStats`,
  `BodyInfo`, `RayHit` (port `PhysicsWorldStats`/`PhysicsBodyInfo`/`PhysicsRayHit`, glam types).
- `World::new() -> Result<World>` wrapping `jolt_world_new` + `jolt_world_init`; `Drop` drops the
  `UniquePtr`. `World::new` installs the process-global Jolt state lazily (idempotent `sys::init`).
- `shutdown_physics()` — the global-teardown pair (`sys::shutdown`: `UnregisterTypes` + destroy the
  `Factory`), exported from the safe crate so the host can tear the globals down **after** the last
  world drops (08-host-and-viewport:phase-6). Added during that phase to fill a substrate gap — the
  safe crate previously had no public `shutdownPhysics`.
- `World::populate(&mut self, scene: &mut Scene, cook: &mut dyn FnMut(Uuid) -> Result<Mesh>)` — the
  body-creation walk; this phase builds only the Box shape (`createShape(BoxShapeSettings)` via the
  shim, sized from `halfExtents`, convex-radius clamp); other shapes return a typed error / skip until
  phase 6. `resolveObjectLayer`, `allowedDOFs`, motion mapping ported as private fns.
- `World::step(&mut self, scene: &mut Scene, dt: f32)` — the fixed-step accumulator, `Update`, dynamic
  write-back. The kinematic `MoveKinematic` and character/contact branches are present but inert (no
  kinematic bodies, characters, or contacts exist yet — phases 4/7/8).
- `World::stats`, `World::list_bodies`, `World::apply_impulse`/`add_force`/`set_linear_velocity`/
  `body_linear_velocity` (Dynamic-only, `&mut self` for the mutators).

## Acceptance gate

- `cargo build -p saffron-physics` and `-p saffron-physics-sys` succeed; `#![deny(unsafe_code)]` holds
  in `saffron-physics`.
- A `#[test]` `box_falls_under_gravity` builds a world with one Dynamic box above a Static floor, steps
  N fixed substeps, and asserts the box's `TransformComponent.translation.y` decreased and it came to
  rest on the floor (a real test, the kind `runPhysicsSelfTest` gestured at).
- A `#[test]` `impulse_changes_velocity` applies an impulse to a Dynamic body and asserts
  `body_linear_velocity` reflects it; a non-Dynamic/unmapped target is a no-op (no panic).
- A `#[test]` `stats_and_list` asserts `stats().dynamic_count` and `list_bodies()` length/contents match
  the bodies created, in creation order.
