# Phase 7 — The `ScriptHostBridge` trait, physics bindings, and contact dispatch

**Status:** COMPLETED

**Depends on:** 12-scripting:phase-5-runtime-lifecycle

## Goal

Define the `ScriptHostBridge` trait (the POD seam that keeps `saffron-script` free of a physics/sceneedit
dependency), bind the physics-reaching `sa.*`/`sa.Entity` functions over it (`raycast`, `spherecast`,
`apply_impulse`, `add_force`, `set_velocity`, `get_velocity`, ragdoll control, `sa.log`'s log-sink), bind
the pure-Scene `move_character`, and port `dispatch_contact` (the contact-event-ring → script handlers).

## Why this shape (NO LEGACY)

- **The 11 `std::function` bridges become one trait the host implements.** C++ kept `Saffron.Script` off
  a physics/sceneedit edge by routing every physics reach through `std::function` POD closures the host
  installed (`ScriptHost::raycast`/`sphereCast`/`applyImpulse`/`addForce`/`setVelocity`/`getVelocity`/
  `setRagdollEnabled`/`setRagdollBlend`/`ragdollState`/`logSink`, `script.cppm:136`–148). In Rust this is
  a `trait ScriptHostBridge` with one method per bridge over POD args (`Vec3`, `Uuid`, the
  `ScriptRayHit`/`ScriptRagdollState` POD structs); `ScriptHost` holds a `Box<dyn ScriptHostBridge>`
  defaulting to a no-op impl ("unset = a safe no-op", `script_runtime.cpp:525`). The host
  (`saffron-host`, which depends on physics + sceneedit) implements it, calling `raycast_world`/
  `apply_body_impulse`/`enable_ragdoll`/`push_script_log`/etc. — exactly the C++ wiring
  (`host.cppm:1200`–1308). The crate stays `saffron-core` + `saffron-scene` only (README §1).
- **`sa.raycast`/`spherecast` shape the POD hit into the result table.** The binding calls
  `bridge.raycast(...)` → `ScriptRayHit`, then builds `{hit, distance, point=sa.Vec3, normal=sa.Vec3,
  entity=<sa.Entity or nil>}`, resolving the hit entity uuid through the session's scene
  (`script_runtime.cpp:1288`–1333). The `ScriptRayHit`/`ScriptRagdollState` POD structs live in
  `saffron-script` (Jolt-free), filled by the host impl.
- **`move_character` is a pure Scene write, no bridge.** It writes `CharacterControllerComponent`
  (`desired_velocity`, `vertical_velocity`) which `saffron-scene` owns, consumed by the next physics
  step — so it goes through the session guard, not the bridge (`script_runtime.cpp:505`–519).
- **`dispatch_contact` maps a contact transition to handlers on both entities.** A sensor Begin →
  `on_trigger_enter(self, other)`, sensor End → `on_trigger_exit`, solid Begin → `on_contact(self,
  other, point, normal)` (world space, passed as `sa.Vec3` since the interface stays glm/glam-POD);
  solid End has no handler. The first failing handler is returned (pause-on-error); a missing handler is
  a silent skip; both directions are dispatched (`dispatchContact`/`callContactHandler`,
  `script_runtime.cpp:1424`–1483,667–696). The host drives this by draining the contact-event ring
  before `on_update` each tick (`host.cppm:1161`–1183, the `drainContacts` + cursor loop) — this area
  provides `dispatch_contact`; 08-host-and-viewport drives the drain.

## Grounding (real files / symbols)

- `engine-old/source/saffron/script/script.cppm`: the `std::function` bridge fields + `ScriptRayHit`/
  `ScriptRagdollState` POD structs (90–148), `dispatchContact` decl (169–170).
- `engine-old/source/saffron/script/script_runtime.cpp`: `applyImpulse`/`addForce`/`setVelocity`/
  `getVelocity`/ragdoll (523–579), `moveCharacter` (505–519), `sa.raycast`/`spherecast` (1288–1333),
  `sa.log` log-sink override (1273–1285), `dispatchContact` (1424–1483), `callContactHandler` (667–696).
- `engine-old/source/saffron/host/host.cppm`: the bridge wiring (`state->script.raycast = …`,
  1200–1308), the contact-ring drain (`drainContacts` + `dispatchContact` loop, 1161–1183).
- 05-physics-jolt-bridge: `raycast_world`/`sphere_cast_world`/`apply_body_impulse`/`enable_ragdoll`/
  `ragdoll_state` (the host calls these inside its `ScriptHostBridge` impl, not this crate).

## Acceptance gate

- `cargo build --workspace` succeeds; `#![deny(unsafe_code)]`; clippy + fmt clean; `saffron-script`
  still depends on `saffron-core` + `saffron-scene` only (a `#[test]`/Cargo check confirms no physics
  edge).
- `#[test]`: with a stub `ScriptHostBridge` returning a fixed `ScriptRayHit`, `sa.raycast(...)` builds
  the `{hit, distance, point, normal, entity}` table with `point`/`normal` as `sa.Vec3` and `entity`
  resolved (or nil when no hit); the default no-op bridge yields `{hit=false}`.
- `#[test]`: `apply_impulse`/`add_force`/`set_velocity`/`get_velocity`/`enable_ragdoll`/
  `set_ragdoll_blend`/`ragdoll_state` route to the bridge with the entity's uuid; `sa.log` calls
  `log_sink(sender, msg)` after the engine log.
- `#[test]`: `e:move_character(v, jump)` writes `CharacterControllerComponent` (no bridge call).
- `#[test]` (contacts): `dispatch_contact` for a sensor Begin invokes `on_trigger_enter` on both
  entities' scripts; a solid Begin invokes `on_contact(self, other, point, normal)`; a missing handler
  is skipped; a failing handler returns a `ScriptRunError`.
