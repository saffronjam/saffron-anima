# Phase 10 — Raycast / shapecast queries and the script host-callback seam

**Status:** COMPLETED
**Depends on:** 05-physics-jolt-bridge:phase-9

## Goal

Implement the read-only scene queries: `raycast_world` (closest ray hit, mapped back to the owner
entity) and `sphere_cast_world` (a thicker sphere sweep), plus the POD `RayHit` they return. Define the
host-callback seam these expose to scripting (`sa.raycast` / `sa.spherecast`) without `saffron-script`
depending on `saffron-physics` — the seam the host bridges (area 08/12). This completes the physics
public surface.

## Why this shape (NO LEGACY)

`raycastWorld` (`physics.cpp:1117`) casts an `RRayCast` through the narrow-phase query, reads the hit
fraction/point/normal (via a `BodyLockRead` for the surface normal), and maps the hit BodyID back to its
entity uuid (`bodyUuid`, `physics.cpp:1110`). `sphereCastWorld` (`physics.cpp:1144`) does the same with a
`ShapeCast` + a `ClosestHitCollisionCollector`. Both are read-only — they must not perturb the
deterministic step, so they run between steps (a command, or `on_update`), never mid-solve; the Rust
signatures take `&self` to enforce that. The script bridge is a host-owned callback (`host.cppm:1200`):
`Saffron.Script` exposes a `raycast`/`sphereCast` `std::function` field that the host binds to the live
world, converting `PhysicsRayHit` → the POD `ScriptRayHit`. In Rust this is a trait (e.g.
`trait RaycastProvider { fn raycast(...) -> RayHit; }`) the host implements over the `Option<World>`, so
`saffron-script` depends only on the trait, never on `saffron-physics` — preserving the module-boundary
constraint (the script area, 12, defines the trait; the host wires it).

## Grounding (real files/symbols)

- `engine-old/source/saffron/physics/physics.cpp:1107-1142` — `bodyUuid` (BodyID→uuid via
  `indexByBodyId`), `raycastWorld` (`RRayCast`, `CastRay`, `GetPointOnRay(fraction)`,
  `BodyLockRead` + `GetWorldSpaceSurfaceNormal`, `bodyUuid`).
- `engine-old/source/saffron/physics/physics.cpp:1144-1173` — `sphereCastWorld` (`SphereShapeSettings`,
  `RShapeCast::sFromWorldTransform`, `ClosestHitCollisionCollector<CastShapeCollector>`,
  `mContactPointOn2` + `mPenetrationAxis` → point/normal, `bodyUuid`).
- `engine-old/source/saffron/physics/physics_types.cppm:64-71` — `PhysicsRayHit` (hit, entity, point,
  normal, distance).
- `engine-old/source/saffron/host/host.cppm:1200-1236` — the `raycast`/`sphereCast` host callbacks
  bridging to `ScriptRayHit`.
- `engine-old/source/saffron/control/control_commands_physics.cpp:256` (`raycast`), `:276` (`shapecast`)
  — the control commands over the same query fns; DTOs `RaycastParams`/`ShapecastParams`/`RaycastResult`
  (`control_dto.cppm:541-563`).

## Work

- Shim: expose narrow-phase ray cast (`CastRay` + the body-lock surface-normal read) and shape cast
  (`CastShape` + closest-hit collector) returning POD hit fields + the hit `BodyId`.
- `saffron-physics`: `RayHit` (glam, port `PhysicsRayHit`); `World::raycast(&self, origin, dir, max_dist)
  -> RayHit` and `World::sphere_cast(&self, origin, dir, radius, max_dist) -> RayHit`; `body_uuid`
  lookup via `index_by_body_id`. Both `&self`.
- Define the host-callback seam for scripting: a `RaycastProvider`-style trait (owned by area 12) and
  the `RayHit` → script-POD mapping the host performs. This phase specifies the conversion; the host
  wiring lands in area 08, the trait + binding in area 12.

## Acceptance gate

- `cargo build -p saffron-physics` succeeds.
- A `#[test]` `ray_hits_box` casts a ray at a Static box and asserts hit=true, the entity uuid matches,
  the point is on the box face, and the distance ≈ expected; a ray into empty space returns hit=false.
- A `#[test]` `sphere_cast_thicker` asserts a sphere sweep catches an edge a thin ray of the same
  origin/dir misses.
- A `#[test]` `query_does_not_perturb` runs a deterministic scenario, takes the phase-5 trace hash,
  interleaves raycasts between steps, and asserts the trace hash is unchanged (queries are read-only).
