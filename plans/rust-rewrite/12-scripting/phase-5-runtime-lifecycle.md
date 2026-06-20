# Phase 5 — The runtime lifecycle: start / tick / stop, instance build, field inject

**Status:** COMPLETED

**Depends on:** 12-scripting:phase-4-component-bridge

## Goal

Port the `ScriptHost` lifecycle: `start_scripts` (create VM, register bindings, instantiate every
`ScriptComponent` slot in deterministic order, run `on_create`), `tick_scripts` (run `on_update(dt)` in
instance order with pause-on-error), `stop_scripts` (run `on_destroy`, drop everything, drop the VM),
the class-table cache, the instance build with field injection + slot overrides, and the deferred
destroy + relink. After this phase the engine can run scripted gameplay end-to-end (minus the scheduler,
messages, and the physics bridge, which follow).

## Why this shape (NO LEGACY)

- **One VM for the whole play session, instances in creation order.** The `ScriptHost` holds one
  `mlua::Lua`, a `HashMap<String, RegistryKey>` class cache keyed by script path, and a
  `Vec<ScriptInstance>` whose order is load-bearing (instances run top-to-bottom). `start_scripts` walks
  `forEach<ScriptComponent>` and, per slot, loads the class (cached), builds the instance, and pushes it
  — exactly the C++ order (`startScripts`, `script_runtime.cpp:1060`–1394).
- **A class table must return a table with `on_update`.** `load_class` loads+runs the file, requires the
  returned value be a table carrying an `on_update` function, caches its `RegistryKey` per path (the C++
  `luaL_ref` int → `mlua::RegistryKey`), else a typed `Err` (`loadClass`, `script_runtime.cpp:700`–735).
  A slot that fails to load is a logged skip, not a fatal — the session continues
  (`script_runtime.cpp:1360`).
- **Instance build = `self` table with `entity` + injected fields + metatable `__index = Class`.**
  `make_instance` builds `setmetatable({ entity = <handle>, <fields> }, { __index = Class })`
  (`makeInstance`, `script_runtime.cpp:846`–863). `inject_fields` sets each declared `properties` key:
  the slot's override when present (JSON→Lua), else the declared default; a `sa.Vec3` field injects a
  **fresh per-instance** Vec3 (value copy, never aliased across instances); table defaults are
  shallow-copied; unknown override keys are silently dropped (a renamed/removed field's stale override)
  (`injectFields`, `script_runtime.cpp:791`–842). In Rust the per-instance-copy concern is natural —
  `Vec3` is `Copy` and a fresh table is built per instance — but the override-vs-default precedence and
  the drop-unknown-keys behavior port exactly.
- **Pause-on-error: the first failing instance halts the tick and is returned.** `tick_scripts` runs
  each instance's `on_update(dt)` in order; the first `Err` becomes a `ScriptRunError {entity_uuid,
  script, message}` and breaks the loop; the VM and all instances survive (`tickScripts`,
  `script_runtime.cpp:1396`–1422). `call_instance_method` calls `self:<name>(dt?)`; an absent method is
  a successful no-op (only `on_update` is required, enforced at load) — `mlua` makes this a `Table::get`
  + `Function::call`, with the budget reset per call (phase 1).
- **Deferred destroy + relink after the instance loop.** `entity:destroy()` queues a uuid
  (`pending_destroy`) and sets `hierarchy_dirty`; the handle stays valid for the rest of the handler.
  `flush_structural_ops` runs after each loop: destroy each queued entity, then `relink_hierarchy` once
  if dirty (`flushStructuralOps`, `script_runtime.cpp:585`–601). Never mid-loop — the instance vector is
  iterated by reference (in Rust, iterate over a snapshot/index so a deferred op cannot invalidate it).
- **`stop_scripts` runs `on_destroy` with no scene bound, then drops the VM.** The play duplicate may
  already be gone, so `on_destroy` runs with the session guard inactive (entity access degrades to
  no-ops); then instances/cache/queues clear and the VM drops (`stopScripts`,
  `script_runtime.cpp:1485`–1510). In Rust this is the guard staying inactive + dropping `mlua::Lua`
  (Drop frees it) — no `lua_close` dance.

## Grounding (real files / symbols)

- `engine-old/source/saffron/script/script_runtime.cpp`: `startScripts` (1060–1394), `tickScripts`
  (1396–1422), `stopScripts` (1485–1510), `loadClass` (700–735), `makeInstance` (846–863),
  `injectFields` (791–842), `pushTableCopy` (772–785), `callInstanceMethod` (639–663),
  `flushStructuralOps` (585–601), `ScriptInstance` (`script.cppm:62`–69).
- `engine-old/source/saffron/scene/scene.cppm`: `ScriptComponent`/`ScriptSlot` (339–351), `forEach`,
  `findEntityByUuid`, `destroyEntity`, `relinkHierarchy`, `createEntity`.

## Acceptance gate

- `cargo build --workspace` succeeds; `#![deny(unsafe_code)]`; clippy + fmt clean.
- `#[test]`: a scene with two entities each carrying a `ScriptComponent` slot (a fixture `.luau`
  returning a class with `on_create`/`on_update` and a `properties` table) starts: `on_create` runs once
  per slot in instance order; `tick_scripts(dt)` runs `on_update`; instances persist across ticks.
- `#[test]`: field injection — a slot override sets `self.speed`; a missing override falls back to the
  declared default; a `sa.vec3` field is a fresh per-instance value (mutating one instance's field does
  not bleed to another); a stale override key for a removed field is dropped silently.
- `#[test]` (pause-on-error): the first instance whose `on_update` errors returns a `ScriptRunError` with
  its uuid + script path + a traceback, halts that tick, and the VM survives a subsequent tick.
- `#[test]` (deferred destroy): `entity:destroy()` in `on_update` keeps the handle valid for the rest of
  the handler, the entity is gone after the loop, and one `relink_hierarchy` ran.
- `#[test]`: `stop_scripts` runs `on_destroy` (logging on a now-detached scene) and drops the VM cleanly;
  a second `start_scripts` builds a fresh session.
