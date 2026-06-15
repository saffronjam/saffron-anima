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

`SceneEditContext::onPlayStateChanged` is the lifecycle seam every gameplay subsystem rides. The
script VM already subscribes to it: it starts on `Edit → Playing` and tears down on `→ Edit`. The
physics world is a sibling — the Host subscribes the same way and keeps a `std::optional<PhysicsWorld>`
that holds a world exactly when play is active:

- on `Edit → Playing`, it lazily installs the Jolt globals once, then creates a world;
- on `→ Edit`, it drops the world, and the move-only `PhysicsWorld` destructor frees every Jolt
  object it owns.

Because the world is keyed on the play state and built against the play duplicate, a quit mid-play,
a second play, or a stop all resolve through the same RAII edge — there is no manual teardown of
bodies and no leak across the boundary.

## Jolt globals vs the world

Jolt has process-global state — a default allocator, a `Factory`, and a table of registered types —
that must be installed before any world is built and torn down only after the last world is gone.
The engine keeps these as a separate, balanced pair (`initPhysics` / `shutdownPhysics`) from the
per-session world (`createPhysicsWorld` / the `PhysicsWorld` destructor). The globals install lazily
on the first play and shut down at engine exit, after the world is already gone, so the registered
types always outlive every body.

## A cross-platform-deterministic, single-precision build

Jolt is vendored from source with **`CROSS_PLATFORM_DETERMINISTIC` on**. This is a compile-time
choice — it changes the floating-point math Jolt emits so a simulation produces bit-identical
results across machines, which is the prerequisite for future lockstep networking and replay. It
carries a modest performance cost, accepted up front rather than retrofitted, because flipping it
later would invalidate any recorded simulation. The build stays **single precision**
(`DOUBLE_PRECISION` off): cross-platform determinism is a float-determinism feature, not a
double-precision one.

The world steps on a fixed timestep for the same reason — a deterministic simulation must advance
in fixed increments, decoupled from the render frame rate.

## The Jolt-free boundary

Only one translation unit includes `<Jolt/…>`. The module's interface exposes an **opaque,
move-only `PhysicsWorld` handle** (a pimpl whose definition lives in the implementation unit) plus a
small `:Types` partition of plain enums and POD — never a Jolt type. So importing `Saffron.Physics`
never pulls Jolt's heavy headers into another module's compiled interface, exactly as the renderer
keeps `vk::` types behind its own pimpl. This is also why Jolt's architecture and thread compile
flags are scoped to that one source file: the rest of the engine, including the per-target
`import std` modules, never needs them.

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
| World handle + globals + stats | `engine/source/saffron/physics/physics.cppm` | `PhysicsWorld`, `initPhysics`, `shutdownPhysics`, `createPhysicsWorld`, `physicsWorldStats` |
| The Jolt vocabulary (Jolt-free POD) | `engine/source/saffron/physics/physics_types.cppm` | `MotionType`, `PhysicsLayer`, `PhysicsWorldStats` |
| The single Jolt TU + filter interfaces | `engine/source/saffron/physics/physics.cpp` | `PhysicsWorldImpl`, the broad-phase / object-layer filters |
| Lifecycle wiring + lazy globals | `engine/source/saffron/host/host.cppm` | `HostState::physics`, the `onPlayStateChanged` subscription |
| Control summary | `engine/source/saffron/control/control_commands_physics.cpp` | `physics-state` |
