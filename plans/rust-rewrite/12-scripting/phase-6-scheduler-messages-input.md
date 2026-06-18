# Phase 6 — The coroutine scheduler, inter-script messages, input, and hierarchy

**Status:** COMPLETED

**Depends on:** 12-scripting:phase-5-runtime-lifecycle

## Goal

Add the remaining scene-side runtime surface: the Roblox-task-style coroutine scheduler
(`sa.wait`/`spawn_task`/`delay` + the per-tick `_sa_advance`), inter-script messages
(`entity:send`/`sa.broadcast` drained after the loop), the input bindings (held/edge keys + mouse), and
the hierarchy/query bindings (`parent`/`children`/`set_parent`/`spawn`/`get_entity_by_name`/
`find_all_by_name`/`find_by_uuid`/`primary_camera`).

## Why this shape (NO LEGACY)

- **The `SchedulerPrelude` is verbatim Luau, installed onto the `sa` table.** The C++ scheduler is pure
  Lua over the coroutine lib (`SchedulerPrelude`, `script_runtime.cpp:865`–904): `sa.spawn_task` creates
  + resumes a coroutine, `sa.wait` yields, `sa.delay` is wait+call, and a global `_sa_advance(dt)`
  resumes ready coroutines timed off accumulated `dt` (deterministic, never `os.clock`). It is installed
  with `rawset(sa, "name", …)` because the `sa` namespace table has a read-only `__newindex`. In Rust
  the prelude is the **same Luau source string** run after the bindings are bound; whether the `sa` table
  needs a read-only metatable depends on how the `sa` global is built — if `sa` is a plain table the
  `rawset` is a plain assignment, but the prelude string is kept as-is for fidelity. `advance_scheduler`
  calls `_sa_advance(dt)` under the traceback/budget guard each tick after the message dispatch; a
  faulting coroutine logs, never crashes the VM (`advanceScheduler`, `script_runtime.cpp:908`–925).
- **Messages dispatch after the instance loop, payloads as registry refs.** `entity:send(handler,
  payload)` queues a `ScriptMessage {target, sender, handler, payload_ref}`; `sa.broadcast` queues with
  `target = 0`. `dispatch_messages` drains the queue after the loop, calling `self:<handler>(sender,
  payload)` on each matching instance, then releases each payload ref (`send`/`broadcast`/
  `dispatchMessages`/`callMessageHandler`, `script_runtime.cpp:468`–488,1254–1270,929–986). The payload
  ref is a `mlua::RegistryKey` (the C++ `luaL_ref` int); `current_sender_uuid` is the instance whose
  handler is running. Never mid-loop (the instance vector is iterated by snapshot).
- **Input is held + derived edges, read through the borrowed `ScriptInputState`.** The bindings
  `is_key_down`/`is_key_pressed`/`is_key_up`, `mouse_position`/`mouse_delta`/`mouse_scroll`,
  `is_mouse_down`/`pressed`/`up` read the session-bound `ScriptInputState` (held vs derived edge sets,
  normalized to lowercase) (`script_runtime.cpp:1108`–1160). The edge derivation (`pressed`/`released`/
  `mouse_d*`) is `derive_script_input_edges`, called once per tick by the host **before** `tick_scripts`
  (`scene.cppm:1256`); this area binds the read side, the host (08) drives the per-tick derive + raw
  fill. The input is supplied through the session guard (phase 3), not stored.
- **Hierarchy + query bindings are scene reads/writes through the guard.** `parent()`/`children()`
  read `RelationshipComponent`; `set_parent` is the only reparent path (runs `set_parent` which guards
  self/cycle/dangling + relinks, safe mid-tick); `spawn` mints a Name+Transform+Relationship root in the
  play duplicate; `get_entity_by_name`/`find_all_by_name`/`find_by_uuid`/`primary_camera` are `forEach`
  scans (`script_runtime.cpp:415`–464,1163`–1252). All go through the session guard's `currentScene`.

## Grounding (real files / symbols)

- `engine-old/source/saffron/script/script_runtime.cpp`: `SchedulerPrelude` (865–904),
  `advanceScheduler` (908–925), `ScriptMessage` (`script.cppm:79`–88), `send` (468–488), `broadcast`
  (1254–1270), `dispatchMessages` (962–986), `callMessageHandler` (929–957), the input bindings
  (1108–1160), `normalizeInputKey` (630–635), `parent`/`children`/`setParent` (415–464),
  `get_entity_by_name`/`primary_camera`/`spawn`/`find_all_by_name`/`find_by_uuid` (1163–1252).
- `engine-old/source/saffron/scene/scene.cppm`: `ScriptInputState` (1235–1252),
  `deriveScriptInputEdges` (1256+).
- 03-ecs-and-scene: `setParent`, `relinkHierarchy`, `createEntity`, `RelationshipComponent`, `forEach`.

## Acceptance gate

- `cargo build --workspace` succeeds; `#![deny(unsafe_code)]`; clippy + fmt clean.
- `#[test]` (scheduler): a script `sa.spawn_task` + `sa.wait(0.1)` resumes after the accumulated `dt`
  crosses the threshold across several `tick_scripts(dt)` calls; a faulting coroutine logs and the VM
  survives; timing is dt-driven (deterministic), not wall-clock.
- `#[test]` (messages): `entity:send("ping", payload)` invokes the target's `ping(self, sender,
  payload)` after the loop with the right sender; `sa.broadcast` reaches every instance; the payload ref
  is released (no leak across ticks).
- `#[test]` (input): with a fixture `ScriptInputState`, `sa.is_key_down("w")` reflects held;
  `is_key_pressed`/`is_key_up` reflect the one-tick edges after `derive_script_input_edges`; mouse
  position/delta/scroll/buttons read through; keys normalize case.
- `#[test]` (hierarchy): `e:set_parent(p)` reparents (guarded), `e:parent()`/`e:children()` reflect it
  after relink; `sa.spawn("x")` creates a root; `sa.get_entity_by_name`/`find_by_uuid`/`primary_camera`
  resolve.
