# 12 — Scripting (Luau via mlua)

Per-entity gameplay scripting. The engine embeds one VM for the whole play session, instantiates a
class table per `ScriptComponent` slot, runs `on_create`/`on_update`/`on_destroy` plus the contact and
message handlers, and exposes a small `sa.*` API (entity handles, transforms, input, physics bridges,
a coroutine scheduler). The C++ engine built this on **Lua 5.5 + LuaBridge3** (`Saffron.Script`, ~1.95k
LOC) and hand-wrote the LuaLS type stubs (`library/sa.lua`) with a regex drift tripwire keeping them in
sync. The Rust rewrite adopts **Luau via `mlua`** (the locked decision, pre-plan §0) and makes the
typed `sa.*` surface a **generated artifact** from the binding source — so the hand-written overlay and
its tripwire are deleted, replaced by a generated-defs-fresh check.

This area owns `saffron-script` (the VM + typed bindings + runtime port + the session guard) and the
area-12 half of the Luau typegen (the `sa.*` API emitter built on area 10's shared mapper).

## 1. Crate boundary and dependencies (locked)

`saffron-script` depends on `saffron-core` + `saffron-scene` **only** (the foundations contract crate
graph). It must NOT depend on `saffron-physics` or `saffron-animation` — exactly the C++ module-boundary
constraint (`Saffron.Script` imports only `Saffron.Core` + `Saffron.Scene`, `script.cppm:30`). The
physics reach (`sa.raycast`, `apply_impulse`, ragdoll control) and the editor script-log ring cross the
boundary as **host-installed callbacks over plain-POD signatures** (`ScriptRayHit`/`ScriptRagdollState`
+ `(u64, glm::vec3)` closures, `script.cppm:136`–148), which become a Rust **trait the host implements**
(§6). The crate is `#![deny(unsafe_code)]` — `mlua` confines all `lua_State` unsafety internally, which
is the dominant C++ hazard the port erases (feasibility 4.4).

## 2. Luau / mlua (the VM decision, confirmed)

`mlua` with the `luau` feature replaces `lua55` + LuaBridge3. What it gives over the C++ stack:

- **Gradual types.** Luau is the typed dialect the `todo.md` intent wanted; the generated `.luau` defs
  are the type surface gameplay scripts check against (§7).
- **Built-in sandbox.** `Lua::sandbox(true)` replaces the C++ `luaL_openselectedlibs(..., GLIBK |
  COLIBK | STRLIBK | MATHLIBK | TABLIBK | UTF8LIBK, 0)` hand-curated library set (`script.cppm:282`):
  Luau ships sandboxing as a first-class mode (frozen globals, no `io`/`os`/`debug`/`package`,
  read-only base tables). The C++ sandbox probe self-test (`assert(io == nil and os == nil …)`,
  `script.cppm:326`) becomes a `#[test]`.
- **Instruction budget.** `Lua::set_interrupt` (Luau's interrupt hook) + `Lua::set_memory_limit` give
  the budget/timeout the C++ VM lacked — a guard against a runaway gameplay loop hanging the host
  frame. Decision: install an interrupt that aborts a tick exceeding a per-tick instruction budget,
  surfaced as a `ScriptRunError` (the same pause-on-error path as a Lua error).
- **Determinism.** Luau is deterministic (no `os.clock` in the sandbox; the scheduler already times off
  accumulated `dt`, not wall-clock — `SchedulerPrelude`, `script_runtime.cpp:865`), so it composes with
  the lockstep premise the physics gate protects.
- **Safe callbacks.** `mlua` converts a Rust panic in a callback to a Lua error and guards the stack, so
  the entire LuaBridge3 raw-stack discipline (`lua_push*`/`lua_pcall`/`lua_pop` balance, the
  `tracebackHandler` + `luaL_traceback`, `popError`) collapses into `mlua`'s `Function::call` +
  `Error` with a Luau traceback. The contained-fault contract (a faulting instance/handler logs and the
  VM survives) is preserved by mapping `mlua::Error` to the per-instance `ScriptRunError`.

The VM is single-threaded and `!Send`: `mlua::Lua` is `!Send` under the default feature set, matching
the C++ single-VM-on-the-host-thread model. No `Arc<Mutex>` — the `ScriptHost` is owned by value by the
host's `HostLayer` (per 08-host-and-viewport's decision that host state is single-thread-owned).

## 3. The single-source binding layer (re-evaluating the rejected declarative registry)

The C++ plan rejected a declarative typed-descriptor registry for the bindings because LuaBridge3
registers functions by *deduced C++ type* and forced raw `lua_CFunction` thunks — a registry that both
registered and emitted types was "strictly worse" there. **`mlua` changes the calculus**: bindings are
registered via the `UserData` trait + `IntoLua`/`FromLua` (typed Rust signatures), so a function's
argument and return types are already first-class Rust types. The single-source layer is therefore
viable and is the locked shape:

- **One declarative table of `sa.*` binding descriptors** is the source of truth. Each entry names the
  binding (`name`, the `sa.` function or `Entity:` method), its Rust argument types, its return type,
  and a doc string — expressed as Rust data, not as a string parsed out of source. The registration
  step walks this table to wire the VM (`UserDataMethods::add_method` / `Lua::globals` `sa` table);
  area 10's xtask emitter walks the **same** table to emit the `sa.*` `---@class`/`fun(...)` Luau defs.
- **The type mapping is area 10's shared `Rust-type → Luau` mapper** (10-protocol-codegen phase-6, the
  `tsToLua` analogue): `f32/f64 → number`, `bool → boolean`, `String → string`, `WireUuid → string`,
  `Vec3 → {x:number,y:number,z:number}`, a DTO → `sa.<Name>`, `Vec<T> → T[]`. Area 12 adds the API
  surface; the mapper has one owner (area 10). This is the explicit reuse the area-10 README §8 hooks.
- **No proc-macro.** Like the command table and the component registry (area 10's decision), the binding
  set is an explicit ordered table, not `inventory`-collected: emit order is deterministic and the whole
  set is needed at once by the emitter. A small declarative `binding!` helper removes the per-entry
  boilerplate without hiding the set (the PP-7 macro discipline).
- **The drift tripwire is deleted with no behavioral replacement.** `tools/check-script-defs/check.ts`
  existed only because the bindings (imperative C++) and the defs (hand-written `SaLuaDefs`) were two
  copies that could drift. With one source, there is no second copy: the defs are generated. Its
  freshness role folds into the xtask generated-artifacts-fresh diff check (01-build phase-6 / area 10
  phase-5) — re-running the generator must produce a clean git diff. NO LEGACY: `library/sa.lua`'s
  `SaLuaDefs` overlay (`assets.cppm:1078`–1185) and `script_component_defs.generated.hpp` are both gone;
  the generator writes a single `.luau` defs file (area 10 phase-6 + this area's API half).

## 4. The runtime model (ported 1:1, idiomatic)

The C++ `ScriptHost` (`script.cppm:118`) is one VM + class-table cache + an ordered instance vector +
borrowed scene/registry pointers + deferred-op queues + the host-callback closures. The Rust `ScriptHost`
keeps the exact same shape with idiomatic types:

| C++ field | Rust |
|---|---|
| `ScriptVm vm` (move-only, `lua_close` in dtor) | `mlua::Lua` (owns the VM; Drop frees it) |
| `unordered_map<string,int> classRefByPath` | `HashMap<String, mlua::RegistryKey>` (or `Table` cache) |
| `vector<ScriptInstance> instances` | `Vec<ScriptInstance>` (creation order is load-bearing) |
| `Scene* currentScene` / `const ComponentRegistry* currentRegistry` | the **session guard** (§5), not a stored borrow |
| `const ScriptInputState* input` | passed into the tick / held by the guard |
| `vector<u64> pendingDestroy`, `bool hierarchyDirty` | `Vec<Uuid>`, `bool` (deferred structural ops) |
| `vector<ScriptMessage> messages`, `u64 currentSenderUuid` | `Vec<ScriptMessage>`, `Uuid` |
| `int selfRef` per instance, `int payloadRef` per message | `mlua::RegistryKey` (the registry-ref idiom) |
| the 11 `std::function` physics/log bridges | one `Box<dyn ScriptHostBridge>` trait object (§6) |

The lifecycle functions port directly: `start_scripts` (create VM, register bindings, install the
scheduler prelude, instantiate every `ScriptComponent` slot in `forEach` order, run `on_create`),
`tick_scripts` (run every instance's `on_update(dt)` in order, pause-on-error, then flush structural ops
→ dispatch messages → advance scheduler), `dispatch_contact` (sensor enter/exit, solid `on_contact` with
point+normal), `stop_scripts` (run `on_destroy` with no scene bound, drop everything, drop the VM).
`read_script_schema` runs a throwaway sandboxed VM, reads the `properties` table, infers each field type,
returns fields sorted by name.

**Deferred-structural-op ordering is exact** (the silent-correctness contract): `entity:destroy()`
queues a uuid and the handle stays valid for the rest of the handler; `flush_structural_ops` runs after
each instance loop and calls `relink_hierarchy` once if dirty; messages dispatch after the loop;
`set_parent`/`spawn` run inline (safe — they touch components, not the instance vector). The instance
vector is never structurally mutated mid-loop.

## 5. The scoped session guard (the part Rust adds)

The C++ invariant: `host->currentScene`/`currentRegistry` are raw pointers, **non-null only while a
start/tick/stop/contact call is on the stack**; a script entity handle kept past its session degrades to
logged no-ops, never a dangling deref (`script_runtime.cpp:175`–199, the `transformScene`/`registryScene`
guards). The handle caches a raw `ScriptHost*`, and every accessor checks `currentScene != nullptr` then
`valid(scene, entity)`.

Rust cannot put a `&Scene` lifetime into the `'static` userdata an entity handle becomes (feasibility
4.4). The locked re-encoding is a **scoped session guard**: a tick/start/stop sets the borrowed scene +
registry + input into the host for the duration of the call and clears them on scope exit (RAII guard or
explicit set/clear around the instance loop, mirroring `host.currentScene = &scene; … ;
host.currentScene = nullptr`). The entity-handle userdata holds the entity id + a way to reach the host
(a `Weak`/raw token resolved through the guard), and each accessor goes through the guard:
"session-active? entity valid? component present?" — identical to the C++ three-check pattern, returning
a logged no-op / `false` / `nil` otherwise. The borrow checker is satisfied because the `&mut Scene` is
re-supplied per call and never escapes into the `'static` VM. This is the one subsystem-specific Rust
*addition* the foundations subtractions-ledger §6 flags ("the script session guard re-encoding the
`currentScene` raw-pointer invariant").

## 6. The host-callback POD bridge → a trait

The 11 `std::function` bridges (`raycast`, `sphereCast`, `applyImpulse`, `addForce`, `setVelocity`,
`getVelocity`, `setRagdollEnabled`, `setRagdollBlend`, `ragdollState`, plus `logSink`) keep
`Saffron.Script` free of a physics/sceneedit edge — the binding only ever sees POD (`ScriptRayHit`,
`ScriptRagdollState`, `(u64, vec3)`). In Rust this is a `trait ScriptHostBridge` with one method per
bridge over POD args (`Vec3`/`Uuid`/the two POD structs), implemented by the host (`saffron-host`, which
*does* depend on physics + sceneedit). The `ScriptHost` holds a `Box<dyn ScriptHostBridge>` (or a
`Default` no-op impl when unset, matching "unset = a safe no-op"). The `sa.raycast` binding calls
`bridge.raycast(...)` and shapes the `ScriptRayHit` POD into the `{hit, distance, point, normal, entity}`
Lua table; `sa.log` calls `bridge.log_sink(sender, msg)` after the engine log. `move_character` is a
**pure Scene write** (writes `CharacterControllerComponent`), so it needs no bridge — kept on the scene
edge (`script_runtime.cpp:505`).

## 7. The generated `.luau` type surface (area 12's typegen half)

Area 10 phase-6 delivers the shared mapper + the **component-snapshot** emitter (the
`script_component_defs.generated.hpp` replacement: `---@class sa.<Comp>` + `:get_component` overloads,
the transitive reach walk, the two synthetic shapes). Area 12 adds the **`sa.*` API surface** emitter on
the same mapper, from the §3 binding-descriptor table: the `sa.Vec3` value class + operators, the
`sa.Entity` method set, the `sa.RayHit`/`sa.RagdollState`/`sa.ScriptSelf` classes, the `sa.*`
free-function/global table, and the `sa.ComponentName` alias (the registered-name union). Together the
two emitters produce one `.luau` defs file equivalent to today's `SaLuaDefs` + `SaComponentDefs`
concatenation (`assets.cppm:1211`), but generated from the single binding source — no `---@meta`
string-view blob, no `#pragma once` header, no runtime append. The `.luarc.json` project settings
(`assets.cppm:1189`, LuaLS pointed at `library/`, `sa` global, sandboxed-out libs disabled) stay as a
write-when-absent scaffold (owned by 07-assets project I/O), retargeted to the `.luau` defs.

## 8. Ref sites and ownership (this area)

Per the refPolicy, recorded for this area: there are **no `Arc<Mutex>` sites** in `saffron-script` — the
VM and all runtime state are single-thread-owned by value. `mlua::RegistryKey` (not `Rc<RefCell>`) is
the idiom for the class/instance/payload registry refs (the C++ `luaL_ref` ints). The `ScriptHostBridge`
is a `Box<dyn …>` trait object (the open host-implements-it set). The session guard re-supplies `&mut
Scene` per call rather than storing a shared-mutable handle.

## 9. Grounding table

| What | File | Symbols |
|---|---|---|
| VM type + create + sandbox + self-test | `engine-old/.../script/script.cppm` | `ScriptVm`, `newScriptVm`, `luaL_openselectedlibs`, `runScriptSelfTest` |
| Runtime host + lifecycle + handlers | `engine-old/.../script/script_runtime.cpp` | `ScriptHost`, `startScripts`, `tickScripts`, `dispatchContact`, `stopScripts`, `flushStructuralOps`, `dispatchMessages`, `advanceScheduler` |
| Session-guard invariant | `engine-old/.../script/script_runtime.cpp` | `ScriptEntity::transformScene`/`registryScene`, `host->currentScene` set/clear |
| `sa.*` bindings (the descriptor source) | `engine-old/.../script/script_runtime.cpp` | the `beginClass<ScriptEntity>("Entity")` + `beginNamespace("sa")` `.addFunction(...)` chain, `registerScriptValueTypes` |
| Scheduler prelude | `engine-old/.../script/script_runtime.cpp` | `SchedulerPrelude`, `advanceScheduler` |
| Script-declared fields + overrides + Inspector | `engine-old/.../script/script_runtime.cpp`, `.../script/script.cppm` | `injectFields`, `inferField`, `readScriptSchema`, `ScriptField`, `ScriptFieldType` |
| Host-callback POD bridge | `engine-old/.../script/script.cppm`, `.../host/host.cppm` | `ScriptHost` `std::function` fields, `state->script.raycast = …` wiring |
| Contact ring + input edges | `engine-old/.../host/host.cppm`, `.../scene/scene.cppm` | `drainContacts`/`dispatchContact` loop, `ScriptInputState`, `deriveScriptInputEdges` |
| `ScriptComponent` data | `engine-old/.../scene/scene.cppm` | `ScriptComponent`, `ScriptSlot` |
| Hand-written defs (deleted) | `engine-old/.../assets/assets.cppm` | `SaLuaDefs`, `SaComponentDefs`, `ensureScriptLibrary`, `LuarcJson` |
| Drift tripwire (deleted) | `tools/check-script-defs/check.ts` | the whole file |
| get-script-schema command | `engine-old/.../host/host.cppm`, `.../control/control_dto.cppm` | `registerCommand<GetScriptSchemaParams, GetScriptSchemaResult>`, `ScriptFieldDto` |

## 10. Phase list

| Phase | What | Depends on |
|---|---|---|
| `phase-1-vm-sandbox-budget` | `saffron-script` crate, `mlua` Luau VM bring-up, sandbox, instruction/memory budget, traceback→`Error` | 00-foundations, 03-ecs-and-scene |
| `phase-2-value-types-and-binding-table` | the `sa.Vec3` value type + math/operators, the declarative `sa.*` binding-descriptor table (the single source) | phase-1, 02-math-and-geometry |
| `phase-3-session-guard-and-entity-handle` | the scoped session guard + the `sa.Entity` handle (transforms, name/uuid, valid) | phase-2 |
| `phase-4-component-bridge` | `get/set/add/remove/has_component` over the registry serde + the structural-component gate | phase-3, 03-ecs-and-scene component registry |
| `phase-5-runtime-lifecycle` | `start/tick/stop`, class load + instance build + field inject + overrides, pause-on-error, deferred destroy + relink | phase-4 |
| `phase-6-scheduler-messages-input` | the coroutine scheduler prelude, `entity:send`/`sa.broadcast` messages, input edges, hierarchy (`parent`/`children`/`set_parent`/`spawn`) | phase-5 |
| `phase-7-host-bridge-and-contacts` | the `ScriptHostBridge` trait (physics/log POD bridges), `dispatch_contact`, `move_character` | phase-5 |
| `phase-8-schema-and-inspector-contract` | `read_script_schema` + the `GetScriptSchemaResult` DTO contract for the editor Inspector | phase-2 |
| `phase-9-luau-api-typegen` | the `sa.*` API `.luau` emitter on area 10's shared mapper; delete `library/sa.lua` + the tripwire | phase-2, 10-protocol-codegen phase-6 |
