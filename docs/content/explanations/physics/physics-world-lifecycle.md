+++
title = 'Physics world lifecycle'
weight = 1
+++

# Physics world lifecycle

The physics world is not a permanent fixture of the engine. It exists only while the game is
running. Entering play builds a fresh Jolt world; stopping play frees it. Nothing physical survives
between sessions, and nothing about the authored scene is changed to make a simulation run.

This mirrors how play mode already works. When the editor enters play it duplicates the scene
through the JSON serde into a throwaway copy and simulates *that*; stopping play discards the copy.
The authored scene is never written while playing, so "the discard is the restore." The physics
world is built against the same duplicate and dies with it — which is why there is no reset step to
get wrong.

## Built on the play edge

The play-state edge is the lifecycle seam every gameplay subsystem rides. The `saffron-host` layer
detects the `Edit → Playing` transition in `reconcile_play_edge` and keeps the live world in an
`Option<World>` that holds a `saffron_physics::World` exactly when play is active:

- on `Edit → Playing`, it lazily installs the Jolt globals once, then creates a `World`;
- on `→ Edit`, it drops the world, and `World`'s `Drop` frees every Jolt object it owns.

Because the world is keyed on the play state and built against the play duplicate, a quit mid-play,
a second play, or a stop all resolve through the same RAII edge — there is no manual teardown of
bodies and no leak across the boundary.

## Jolt globals vs the world

Jolt has process-global state — a default allocator, a `Factory`, and a table of registered types —
that must be installed before any world is built and torn down only after the last world is gone.
The crate keeps these as a balanced pair from the per-session world: `World::new` runs the
idempotent global init (`saffron_physics_sys::init`) and then allocates the world, and
`shutdown_physics` (over `saffron_physics_sys::shutdown`) tears the globals down. The host installs
the globals lazily on the first play and shuts them down at engine exit, after the world is already
gone, so the registered types always outlive every body.

The world steps on a fixed timestep for the same reason a deterministic simulation must advance in
fixed increments, decoupled from the render frame rate: `FIXED_STEP` is `1/60`, and `World::step`
runs a fixed-step accumulator over it.

## A cross-platform-deterministic, single-precision build

Jolt is a C++ library, vendored from source by `saffron-physics-sys` and compiled by its `build.rs`
with **`JPH_CROSS_PLATFORM_DETERMINISTIC` on**. This is a compile-time choice — it changes the
floating-point math Jolt emits so a simulation produces bit-identical results across machines, which
is the prerequisite for future lockstep networking and replay. The same `build.rs` confines the
determinism flags (`-ffp-model=precise`, `-ffp-contract=off`, a contained `-mavx2`, `JPH_USE_FMADD`
suppressed) to Jolt's own translation units, so the rest of the workspace is never recompiled with
arch flags that would change its float results. The build stays **single precision**
(`JPH_DOUBLE_PRECISION` off): cross-platform determinism is a float-determinism feature, not a
double-precision one. A `#error` guard in the C++ shim and the `cfg(jolt_deterministic)` the build
script sets prove the contract held at compile time.

## The unsafe Jolt boundary

The `unsafe` Jolt FFI lives entirely in `saffron-physics-sys` — the one crate that opts back into
`unsafe`; the safe `saffron-physics` crate holds `#![deny(unsafe_code)]` and speaks only safe Rust
plus a POD bridge. The FFI ABI is a `cxx` bridge: POD-only across the wire (scalars and the shared
`PendingContact` struct), with an opaque `JoltWorld` holding the `PhysicsSystem` plus the four
virtual shim classes `cxx` cannot synthesize (three layer filters and the `ContactListener`). So
depending on `saffron-physics` never pulls Jolt's heavy C++ headers into another crate, exactly as
the renderer keeps its Vulkan handles behind a thin wrapper. The opaque `World` handle the rest of
the engine names is a Rust struct holding a `cxx::UniquePtr<JoltWorld>`; everything else physics
exposes is a small vocabulary of plain enums and POD.

## Observing it

The world is summarised over the control plane by `physics-state`, which reports whether a world is
live and how many bodies it holds (zero until bodies are authored). It returns an inactive summary
in Edit rather than an error, so the editor can poll it unconditionally:

```sh
sa physics-state          # physics=inactive  bodies=0  dynamic=0   (in Edit)
sa play
sa physics-state          # physics=active    bodies=0  dynamic=0   (while playing)
```

## What | File | Symbols

| What | File | Symbols |
|---|---|---|
| World handle + globals + stats | `engine/crates/physics/src/world.rs` | `World`, `World::new`, `shutdown_physics`, `World::stats` |
| The Jolt-free POD vocabulary | `engine/crates/physics/src/types.rs` | `MotionType`, `ObjectLayer`, `WorldStats`, `FIXED_STEP` |
| The unsafe Jolt FFI seam | `engine/crates/physics-sys/src/lib.rs`, `src/bridge.rs` | `init`, `shutdown`, `world_new`, `JoltWorld`, the `cxx` bridge |
| Deterministic Jolt build | `engine/crates/physics-sys/build.rs`, `src/jolt_build_flags.rs` | `JoltBuildFlags::DETERMINISTIC`, `cfg(jolt_deterministic)` |
| Lifecycle wiring + lazy globals | `engine/crates/host/src/layer.rs` | `HostLayer::reconcile_play_edge`, the `physics` `Option<World>` |
| Control summary | `engine/crates/control/src/commands_physics.rs` | `physics-state` |
