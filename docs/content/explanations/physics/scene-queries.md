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

Two casts share one result type (`RayHit`: `hit` flag, owner `entity` uuid, world `point`, world
`normal`, `distance`):

- **`World::raycast(origin, dir, max_dist)`** — Jolt's `NarrowPhaseQuery::CastRay` for the single
  closest hit. `dir` need not be normalized; the cast scales it by `max_dist`, so a caller can pass a
  velocity vector and read `distance` back in those units. The hit point comes from the ray fraction,
  and the surface normal from the hit body.
- **`World::sphere_cast(origin, dir, radius, max_dist)`** — a sphere sweep (`CastShape`) for a
  thicker probe that tolerates an edge a zero-radius ray slips past — a ground check that doesn't
  fall through a crack, say.

Both map the hit `BodyID` back to its entity uuid through the world's body→entity index; a body with
no entity reports `Uuid(0)`.

## Read-only, and off the step

Both methods take `&self`, so a query touches no body state and never perturbs the deterministic step
(the cross-platform-deterministic build stays intact). It must run when no Jolt update is in flight:
control commands run on the main thread between frames, and the Lua binding runs inside `on_update`
(the `sim_tick` seam, after the step completes) — both clear of the step's job graph. A query is
never run from a contact callback (that fires *during* the step).

## Three surfaces, one entry point

- **`raycast` / `shapecast` control commands** read the world through the host's `Option<World>`.
  The world exists only while Playing/Paused, so — unlike `pick`, which works in Edit — these refuse
  in Edit with a "no physics world — enter play first" error.
- **`sa raycast` / `sa shapecast`** print the structured hit from the shell.
- **`sa.raycast` / `sa.spherecast` Lua bindings** — gameplay scripts call them inside `on_update`:
  `local hit = sa.raycast(px,py,pz, 0,-1,0, 2); if hit.hit and hit.entity then … end`. Because the
  crate DAG forbids `saffron-script` depending on `saffron-physics`, the host bridges it:
  `saffron-script` declares a `ScriptHostBridge` trait with `raycast` / `sphere_cast`, and
  `HostScriptBridge` implements it over the live `Option<World>`, calling these two methods and
  flattening `RayHit` into the script-side `ScriptRayHit` (a plain field copy). The Lua binding only
  ever sees a plain hit struct + a resolved entity.

v1 returns the single closest hit; an all-hits collector, query-time layer masks, and overlap/point
queries extend this same entry point later.

## What | File | Symbols

| What | File | Symbols |
|---|---|---|
| The query entry points | `engine/crates/physics/src/world.rs`, `src/types.rs` | `World::raycast`, `World::sphere_cast`, `RayHit` |
| The narrow-phase FFI | `engine/crates/physics-sys/src/lib.rs` | `raycast`, `sphere_cast` |
| Control commands | `engine/crates/control/src/commands_physics.rs` | `raycast`, `shapecast` |
| Lua bindings + host bridge | `engine/crates/script/src/bindings.rs`, `engine/crates/host/src/script_bridge.rs` | `sa.raycast`, `sa.spherecast`, `ScriptHostBridge`, `HostScriptBridge::raycast` |
