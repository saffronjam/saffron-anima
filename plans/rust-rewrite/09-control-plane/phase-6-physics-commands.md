# Phase 6 — Physics command domain

**Status:** COMPLETED

**Depends on:** 09-control-plane:phase-1-socket-server-and-dispatch, 05-physics-jolt-bridge (the physics world, bodies, queries, ragdoll, character), 03-ecs-and-scene + 08-host-and-viewport (sceneEdit for collider/bone fit and ragdoll/kinematic config)

## Goal

Register the 12 physics-domain commands (`register_physics_commands`): world state + body listing,
impulse application, contact-event draining, kinematic bone following, character movement, raycast +
shapecast queries, and the ragdoll surface (enable/set/get). This is the domain that touches the
**nullable** `physics` field (the live play world, null in Edit) — every world-querying handler guards
the null and returns an inactive/empty result rather than an error, so the editor can poll
unconditionally.

## Why this shape (NO LEGACY)

- **`Option<&mut PhysicsWorld>` is the seam, and absence is not an error.** `ctx.physics` is null in
  Edit (the world exists only while Playing/Paused). `physics-state` returns
  `{active:false, bodyCount:0, dynamicCount:0}` when null; `physics-bodies` returns an empty list;
  `apply-impulse`/`move-character`/`raycast`/`shapecast` return a typed error ("no physics world —
  enter play first") because they are meaningless without a world. This precise split — query commands
  degrade gracefully, mutation/query-on-world commands error — is ported exactly (`physics-state`
  `control_commands_physics.cpp` returns inactive; `apply-impulse` errs). The Rust `match ctx.physics`
  makes the null-guard explicit at every site (no silent deref).
- **`fit-collider`/`set-kinematic-bones`/`enable-ragdoll`/`get-ragdoll` reach `sceneEdit`, not the
  world** — they configure components (the collider shape auto-fit, the bone-physics capsule fit, the
  ragdoll config) on the authored scene, so they work in Edit. `fit_collider_to_mesh` and
  `fit_bone_capsules` are the shared auto-fit helpers (`command.cppm:93`/`:98`), kept as one place,
  called both from `add-component` (phase 3) and here. `set-ragdoll` touches both the world (live
  drive) and `sceneEdit` (config).
- **`raycast`/`shapecast` share `RaycastResult`.** `RaycastParams` (origin/dir/maxDist) and
  `ShapecastParams` both return `RaycastResult`; `maxDist` is `Option<f32>` (default 1000), read
  leniently as a missing key. The query routes through the Jolt bridge (`05-physics-jolt-bridge`);
  this phase is the command wrapper. (The Lua `sa.raycast` host-callback bridge is the scripting
  crate's concern, not a control command.)
- **`drain-contacts` drains the contact-event ring** (`ContactEventDto` list since a cursor), the same
  cursor-drain shape as `drain-alarms`/`drain-script-*` — a since-id parameter, a typed event list
  back. Kept uniform.
- **Motion type spells as a kebab/lower string** (`"static"`/`"kinematic"`/`"dynamic"`) in
  `PhysicsBodyDto.motion` via the `motionName` helper — the wire spelling is frozen; the Rust
  `MotionType` enum's `Display`/serde is the one translation place.

## Grounding (real files/symbols)

- `engine-old/source/saffron/control/control_commands_physics.cpp`
  - `registerPhysicsCommands` (12 invocations).
  - `physics-state` null-guard returns inactive (`PhysicsStateResult{active:false,...}`);
    `physics-bodies` null-guard returns an empty list; `apply-impulse` null-guard errs ("no physics
    world — enter play first"). The `motionName` lambda → `"static"/"kinematic"/"dynamic"`.
  - Calls into the physics crate: `physicsWorldStats`, `listPhysicsBodies`, `PhysicsBodyInfo`,
    `MotionType`.
  - Auto-fit helpers `fitColliderToMesh`/`fitBoneCapsules` (`command.cppm:93`/`:98`).
- DTOs: `PhysicsStateResult`, `PhysicsBodyDto`/`PhysicsBodiesResult`, `FitColliderParams`/
  `FitColliderResult`, `ApplyImpulseParams`/`ApplyImpulseResult`, `ContactEventDto`/
  `DrainContactsParams`/`DrainContactsResult`, `SetKinematicBonesParams`/`KinematicBonesResult`,
  `MoveCharacterParams`/`MoveCharacterResult`, `RaycastParams`/`ShapecastParams`/`RaycastResult`,
  `EnableRagdollParams`/`SetRagdollParams`/`GetRagdollParams`/`RagdollResult` — all in
  `control_dto.cppm`. `RaycastParams.maxDist` is `std::optional<f32>` (`:RaycastParams`).
- `09-control-plane/catalog.md` — the physics-domain table (12 rows, nullable-reach noted) + fixtures.

## Acceptance gate

- `cargo build -p saffron-control` green with the physics handlers registered; clippy/fmt clean.
- `cargo test -p saffron-control` passes physics-domain unit tests:
  - **null-world degradation** — with `physics: None`, `physics-state` returns `active:false` and
    `physics-bodies` returns an empty list (never an error); `apply-impulse`/`raycast`/`move-character`
    return the typed "no physics world" error.
  - **with a stub world** — `physics-bodies` maps `MotionType` to `"static"/"kinematic"/"dynamic"`;
    `raycast` with a missing `maxDist` defaults to 1000 (lenient `Option`); `drain-contacts` drains
    the ring from a since-cursor.
  - `fit-collider`/`fit-bone-capsules` no-op (return `false`) on an entity with no collider/skinned
    mesh, mirroring the C++ best-effort.
- The wire-contract test validates the fixtured physics commands' live `result` against OpenRPC and
  `help` against the manifest (`empty`, `alarms-since-0`); the play/rig-dependent commands carry their
  manifest skip reason.
- All entity ids in physics results (e.g. `PhysicsBodyDto.entity`) stay decimal strings.
