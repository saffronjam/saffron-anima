+++
title = 'Scene queries'
weight = 7
+++

# Scene queries

Stepping the world is not enough for gameplay — a script needs to *ask* it things: what is under the
player's feet, what does this shot hit, is there ground ahead. Scene queries answer that by casting a
ray or sweeping a shape against Jolt's narrow phase and reporting the closest hit, mapped back to the
entity that owns the body.

These are the gameplay counterpart to the editor's `pick` command. `pick` tests render AABBs and
billboards for *editor selection* in Edit; a query tests *physics shapes at their simulated
transforms* for gameplay in Play. They serve different surfaces and both stay — this is not a
duplicate path.

## Ray and sphere sweep

Two casts share one result type (`PhysicsRayHit`: hit flag, owner entity uuid, world point, world
normal, distance):

- **`raycastWorld(origin, dir, maxDist)`** — `NarrowPhaseQuery::CastRay` for the single closest hit.
  `dir` need not be normalized; the cast scales it by `maxDist`, so a caller can pass a velocity
  vector and read `distance` back in those units. The hit point comes from the ray fraction, and the
  surface normal from the hit body (under a body lock).
- **`sphereCastWorld(origin, dir, radius, maxDist)`** — a sphere sweep (`CastShape`) for a thicker
  probe that tolerates an edge a zero-radius ray slips past — a ground check that doesn't fall through
  a crack, say.

Both map the hit `BodyID` back to its entity uuid through the world's body→entity index; a body with
no entity reports `0`.

## Read-only, and off the step

A query touches no body state, so it never perturbs the deterministic step (the cross-platform
deterministic build stays intact). It must run when no `PhysicsSystem::Update` is in flight: control
commands run on the main thread between frames, and the Lua binding runs inside `on_update` (the
`simTick` seam, after the step completes) — both clear of the step's job graph. A query is never run
from a contact callback (that fires *during* the step).

## Three surfaces, one entry point

- **`raycast` / `shapecast` control commands** read the world through the non-owning `EngineContext`
  handle the Host installs. The world exists only while Playing/Paused, so — unlike `pick`, which
  works in Edit — these refuse in Edit with a stable `"no physics world — enter play first"` error.
- **`sa raycast` / `sa shapecast`** print the structured hit from the shell.
- **`sa.raycast` Lua binding** — gameplay scripts call it inside `on_update`:
  `local hit = sa.raycast(px,py,pz, 0,-1,0, 2); if hit.hit and hit.entity then … end`. Because the
  DAG forbids `Saffron.Script` importing `Saffron.Physics`, the Host bridges it: it binds a callback
  on the `ScriptHost` that calls `raycastWorld`, and the Lua binding (in `Saffron.Script`) only ever
  sees a plain hit struct + a resolved `sa.Entity`.

v1 returns the single closest hit; an all-hits collector, query-time layer masks, and overlap/point
queries extend this same entry point later.

## What | File | Symbols

| What | File | Symbols |
|---|---|---|
| The query entry points | `engine/source/saffron/physics/physics.cpp` | `raycastWorld`, `sphereCastWorld`, `PhysicsRayHit` |
| Control commands | `engine/source/saffron/control/control_commands_physics.cpp` | `raycast`, `shapecast` |
| Lua binding + bridge | `engine/source/saffron/script/script_runtime.cpp`, `host.cppm` | `sa.raycast`, `ScriptHost::raycast` |
