# Phase 2 — The `cxx` bridge surface and the C++ filter/listener shims

**Status:** COMPLETED
**Depends on:** 05-physics-jolt-bridge:phase-1

## Goal

Author the `cxx` bridge that `saffron-physics` will speak through: an opaque `JoltWorld` handle, the POD
value types crossing the wire, and the four C++ shim classes `cxx` cannot synthesize — the three layer
filters (implemented directly in C++ from the fixed v1 policy) and the `ContactListener` (buffering POD
contact pairs for a Rust-side drain). After this phase the FFI ABI is fixed and auditable; phases 3+ are
pure safe-Rust orchestration on top of it.

## Why this shape (NO LEGACY)

Jolt requires virtual subclasses for `BroadPhaseLayerInterface`, `ObjectVsBroadPhaseLayerFilter`,
`ObjectLayerPairFilter`, and `ContactListener`. `cxx` cannot generate a C++ subclass from a Rust trait,
so these must be C++ classes living in the `-sys` shim (feasibility §4.3: "cxx can't synthesize virtual
subclasses"). The three layer filters encode *fixed* v1 policy — the two-broad-phase split and the
symmetric collision matrix — with no per-project state, so routing them back to Rust per-call buys
nothing and would put Rust callbacks on Jolt's hot cull path; they are implemented in C++, porting
`broadPhaseFor`/`layersCollide` verbatim. The `ContactListener` *does* need to reach Rust, but Jolt fires
it from job threads, so the shim buffers POD pairs under a C++ mutex and exposes a `drain()` the sim
thread calls — exactly `ContactListenerImpl::drain` (`physics.cpp:501`). This is the one place a Rust
`FnMut` on a job thread would fight `!Send`; the POD-buffer-then-drain seam sidesteps it entirely and is
the same design the C++ engine already chose. The GLM↔Jolt quaternion swizzle (`physics.cpp:158`) is
**not** ported: the bridge speaks plain `[f32; 3]` / `[f32; 4]` (xyzw), and glam's `Quat` is xyzw, so
the swizzle is deleted.

## Grounding (real files/symbols)

- `engine-old/source/saffron/physics/physics.cpp:75-132` — `BroadPhase::{NonMoving,Moving,Count}`,
  `broadPhaseFor` (`:85`), `BroadPhaseLayerImpl` (`:91`), `ObjectVsBroadPhaseImpl` (`:113`),
  `ObjectLayerPairImpl` (`:125`).
- `engine-old/source/saffron/physics/physics_types.cppm:26-38` — `ObjectLayer` enum (Static, Moving,
  Character, Debris, Sensor), `ObjectLayerCount`, `layersCollide` declaration.
- `engine-old/source/saffron/physics/physics.cpp:591-606` — `layersCollide` body (the v1 matrix:
  sensor-overlaps-all-but-sensor, static-vs-static off, debris-vs-debris off, else on).
- `engine-old/source/saffron/physics/physics.cpp:459-512` — `ContactRingCap`, `PendingContact`,
  `ContactListenerImpl` (`OnContactAdded` records `GetWorldSpaceContactPointOn1(0)` +
  `mWorldSpaceNormal`; `OnContactRemoved` records the `SubShapeIDPair` body ids; `OnContactPersisted`
  ignored — Begin/End only), `drain()`.
- `engine-old/source/saffron/physics/physics.cpp:546-580` — `PhysicsWorldImpl` field order (filters and
  `contactListener` declared before `system` so they outlive it); `createPhysicsWorld` (`:631`):
  `TempAllocatorImpl(10 MiB)`, `JobSystemThreadPool(cMaxPhysicsJobs, cMaxPhysicsBarriers, -1)`,
  `system.Init(1024, 0, 1024, 1024, …)`, gravity `(0,-9.81,0)`, `SetContactListener`.

## Work

- Define the `cxx` bridge module: an opaque `JoltWorld` (the shim's owning struct, holding the
  `PhysicsSystem` + `TempAllocator` + `JobSystem` + the four shim instances in the correct teardown
  order) created by `jolt_world_new() -> UniquePtr<JoltWorld>`; the world's `Drop` is the C++ destructor
  (it owns the Jolt teardown order — ragdoll detach before body destroy, phase 9 fills this in).
- Author the C++ shim TU in `-sys` (compiled with the same determinism flags as Jolt): the three filter
  classes (ported from `BroadPhaseLayerImpl`/`ObjectVsBroadPhaseImpl`/`ObjectLayerPairImpl` +
  `broadPhaseFor`/`layersCollide`), and the `ContactListener` subclass that pushes `PendingContact` POD
  records (`{a: u64, b: u64, point: [f32;3], normal: [f32;3], begin: bool}` — `BodyID` as its raw `u32`
  widened, or a shared POD) into a `std::mutex`-guarded buffer.
- Expose `cxx` functions: `jolt_world_new`, `jolt_world_init(&mut JoltWorld)` (wires the filters +
  listener into `system.Init` + `SetContactListener`), and `jolt_drain_contacts(&mut JoltWorld) ->
  Vec<PendingContact>` (the mutex-guarded swap-and-clear).
- Define the shared POD types in the `cxx` bridge: `Vec3`/`Quat` as `[f32;3]`/`[f32;4]` (xyzw, no
  swizzle), `BodyId(u32)`, `MotionTypeRaw`/`ObjectLayerRaw` as `u8` mirroring the C++ enums
  (`physics_types.cppm:16`, `:26`).
- Keep `init`/`shutdown` (phase 1) reachable through the bridge.

## Acceptance gate

- `cargo build -p saffron-physics-sys` compiles the `cxx` bridge + the C++ shim TU under the determinism
  flags.
- A `#[test]` `create_empty_world` builds a `JoltWorld` (init globals → `jolt_world_new` →
  `jolt_world_init`) and asserts body count is 0 and the world is non-null, then drops it cleanly (the
  `createPhysicsWorld` smoke from `physics.cpp:1546`, as a real test).
- A `#[test]` `layer_matrix_matches` calls a `cxx`-exposed `layers_collide(a, b)` for every layer pair
  and asserts the truth table equals `layersCollide` (`physics.cpp:591`) — the matrix is load-bearing
  and must not drift.
- A `#[test]` `drain_empty` confirms `jolt_drain_contacts` returns an empty `Vec` on a stepped-zero
  world.
