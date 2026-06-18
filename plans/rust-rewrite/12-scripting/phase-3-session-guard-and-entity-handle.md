# Phase 3 — The scoped session guard and the `sa.Entity` handle

**Status:** COMPLETED

**Depends on:** 12-scripting:phase-2-value-types-and-binding-table

## Goal

Re-encode the C++ borrowed-pointer invariant as a scoped session guard, and build the `sa.Entity`
handle's scene-only surface on top of it: `valid`, `name`, `uuid`, the local + world transform
getters/setters. This is the keystone of the whole area — the answer to "a `&Scene` cannot live in
`'static` userdata" — so it lands before any binding that touches the scene.

## Why this shape (NO LEGACY)

- **The session guard is the Rust addition (foundations ledger §6).** C++ kept `host->currentScene` /
  `currentRegistry` as raw pointers, non-null only while a start/tick/stop/contact call is on the stack;
  every accessor checked `currentScene != nullptr` then `valid(scene, entity)` then the component
  presence, degrading to a logged no-op otherwise (`ScriptEntity::transformScene`/`registryScene`,
  `script_runtime.cpp:180`–298). Rust cannot hold that `&mut Scene` in the `'static` VM userdata.
  Decision: a **scoped guard** supplies the borrowed `&mut Scene` + `&ComponentRegistry` + input for the
  duration of a scripted call and clears them on scope exit (RAII guard or explicit set/clear around the
  instance loop, mirroring `host.currentScene = &scene; …; host.currentScene = nullptr`). The borrow
  never escapes into the VM, so the borrow checker is satisfied; the invariant is the same.
- **The entity handle holds an id + a host token, not a borrow.** The `sa.Entity` userdata holds the
  `Entity`/`Uuid` and a token to reach the host (a `Weak` or a thread-local/host-supplied accessor),
  resolved through the guard each call — exactly the C++ `ScriptEntity { entity; host* }`
  (`script_runtime.cpp:175`). A handle kept past its session resolves to "no active session" → logged
  no-op, never a dangling deref.
- **The three-check accessor pattern ports verbatim.** Each accessor: session active? → entity valid? →
  (for transforms) `TransformComponent` present? Otherwise a `log_warn` no-op returning the C++ default
  (`vec3{0}` position/rotation, `vec3{1}` scale, `"0"` uuid, `""` name). `valid()` is session-active +
  `valid(scene, entity)` (`script_runtime.cpp:196`).
- **Transforms cross as `sa.Vec3`, rotation as euler radians.** `get_position/rotation/scale` read
  `TransformComponent`; `get_world_position` is `world_translation(scene, entity)`;
  `get_world_rotation` decomposes `world_rotation` to euler-ZYX so it round-trips through `set_rotation`
  (`script_runtime.cpp:203`–233). `uuid()` returns the `IdComponent` id as a decimal string (matching
  the wire), `name()` the `NameComponent` name.

## Grounding (real files / symbols)

- `engine-old/source/saffron/script/script_runtime.cpp`: `ScriptEntity` (175–199), `transformScene`
  (180–194), `registryScene` (285–298), `isValid` (196), `getPosition`/`getRotation`/`getScale` +
  world variants (203–233), `setPosition`/`setRotation`/`setScale` (235–257), `name` (259–271),
  `uuid` (273–281), `idValue` (492–500).
- The `sa.Entity` binding registration (`beginClass<ScriptEntity>("Entity") .addFunction("valid", …)`
  etc., `script_runtime.cpp:1076`–1107) — the descriptor entries this phase adds to the phase-2 table.
- 03-ecs-and-scene: `valid`, `has_component`/`get_component` (typed), `world_translation`,
  `world_rotation`, `quat_to_euler_zyx`, `TransformComponent`/`NameComponent`/`IdComponent`.

## Acceptance gate

- `cargo build --workspace` succeeds; `#![deny(unsafe_code)]`; clippy + fmt clean.
- `#[test]`: with a session guard active over a scene with one entity (Name+Transform+Id), a script
  reading `e:name()`, `e:uuid()`, `e:get_position()`, `e:set_position(sa.vec3(...))` then re-reading
  observes the write; `e:get_world_position()` matches `world_translation`; `e:get_world_rotation()`
  round-trips through `set_rotation`.
- `#[test]` (the invariant): an `sa.Entity` handle stashed in a Lua global and used **after** the guard
  scope ends returns the documented defaults (`"0"` uuid, `vec3{0}` position) and logs a warning, never
  panics or reads freed memory; the same handle inside the guard works.
- `#[test]`: a handle to a destroyed/invalid entity reports `e:valid() == false` and accessors no-op.
