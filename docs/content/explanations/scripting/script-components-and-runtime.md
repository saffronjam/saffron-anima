+++
title = 'Script components and the play runtime'
weight = 2
+++

# Script components and the play runtime

An entity runs gameplay logic by carrying a `Script` component: an ordered list of script slots,
each naming a `.lua` file under the project's `src/`. The component is plain data — a path and a
JSON overrides blob per slot — so it serializes like any other component and rides into the play
duplicate for free. All Luau execution lives in the `saffron-script` runtime (`ScriptHost`), which
exists only while play is active.

## How it works

A script file returns a class table with an `on_update(self, dt)` method (`on_create`,
`on_destroy`, and the physics/message callbacks below run if present). On Play, `start_scripts`
creates one VM for the session and, for every slot of every scripted entity, `build_instance`
instantiates `self = setmetatable({ entity = <handle> }, { __index = Class })` — classes are loaded
once per path and shared. Methods are authored colon-style (`function Class:on_update(dt)`, with `self`
implicit); the dot form (`function Class.on_update(self, dt)`) binds the identical field, so the two
are interchangeable and the scaffold uses colons. Within an entity, instances run in slot order
every tick; across entities the order is unspecified. `self.entity` is an opaque handle (the full
method set is in the [API reference](#api-reference) below); it reaches the scene only while a
script callback is on the stack, so a handle smuggled past its session degrades to a logged no-op.

Beyond `on_update`, a class may define `on_create()`, `on_destroy()`, the physics callbacks
`on_trigger_enter(other)` / `on_trigger_exit(other)` / `on_contact(other, point, normal)`, and any
number of message handlers it names itself (see [messaging](#messaging) below).

The lifecycle rides the existing play seams. The play edge duplicates the authored scene by a JSON
round-trip, so scripts always mutate the throwaway duplicate and Stop discards everything; the host
subscribes to `SceneEditContext::on_play_state_changed` to `start_scripts` on Edit→Playing and
`stop_scripts` on →Edit (pause keeps the VM), and installs a `sim_tick` closure that drives
`tick_scripts` with the clamped, fixed-step-aware dt.

Errors are contained per instance: every callback runs through `mlua` with the budget armed and the
Luau traceback captured. The first failing instance halts that tick, the error lands as a
`ScriptRunError` in a bounded ring on the edit context (`push_script_error`, drained over the
control plane), and play flips to Paused one frame later — never from inside the tick, and never by
crashing the host. A slot whose file is missing or fails to load is a logged skip. The VM survives
an error, so Resume retries with state intact.

```mermaid
flowchart LR
    A[Edit] -->|play edge: duplicate scene| B[Playing]
    B -->|on_play_state_changed| C[start_scripts: VM + instances]
    B -->|sim_tick -> tick_scripts| D["on_update(self, dt) per slot"]
    D -->|error: ring + pause| E[Paused]
    E -->|resume| B
    B -->|stop: discard duplicate| A
    A -->|on_play_state_changed| F[stop_scripts: VM destroyed]
```

## Value types

`sa.vec3(x, y, z)` builds an `sa.Vec3` — a real userdata value, not a `{x,y,z}` table. It carries
`.x`/`.y`/`.z`, the operators `+`, `-`, unary `-`, and `*` by a scalar (either order), and the
methods `:length()`, `:normalized()`, `:dot(o)`, `:cross(o)`, `:lerp(o, t)`. Every vector the API
returns (positions, velocities, mouse deltas) is an `sa.Vec3`, and every vector it accepts wants
one. This is the only vector form scripts use — a plain 3-number table is no longer accepted as a
`properties` default and is skipped as uninferable.

## API reference

Module functions, available as `sa.*` inside every script.

**Logging and vectors**

| Function | Returns | Notes |
|---|---|---|
| `sa.log(message)` | — | writes to the engine log under the `script` subsystem AND into the editor's [Script Logs panel](../../ui-and-editor/script-logs-panel/) (drained over `drain-script-logs`, tagged with the logging entity) |
| `sa.vec3(x, y, z)` | `sa.Vec3` | construct a vector value |
| `sa.lerp(a, b, t)` | `sa.Vec3` | linear blend of two vectors |
| `sa.look_at(eye, target, up)` | `sa.Vec3` | Euler XYZ radians aiming `eye` at `target` (default up `+Y`); feed straight to `:set_rotation` |

**Input** (edges are derived per tick from the editor-reported held set)

| Function | Returns | Notes |
|---|---|---|
| `sa.is_key_down(key)` | boolean | **held** — true every tick the normalized key is down, such as `"w"`, `"space"`, `"arrowup"` (the UE `IsKeyDown` sense) |
| `sa.is_key_pressed(key)` | boolean | **press edge** — true the one tick the key goes down, then false until it is released and pressed again (fire-once, for "on press") |
| `sa.is_key_up(key)` | boolean | **release edge** — true the one tick the key goes up |
| `sa.mouse_position()` | `sa.Vec3` | cursor in viewport pixels in `x`,`y` (`z` is 0) |
| `sa.mouse_delta()` | `sa.Vec3` | cursor movement since last tick in `x`,`y` |
| `sa.is_mouse_down(button)` | boolean | **held** — true while mouse `"left"`/`"right"`/`"middle"` is down |
| `sa.is_mouse_pressed(button)` | boolean | **press edge** — true the one tick the button goes down (a click) |
| `sa.is_mouse_up(button)` | boolean | **release edge** — true the one tick the button goes up |
| `sa.mouse_scroll()` | number | wheel delta this tick |

**Scene queries and spawning**

| Function | Returns | Notes |
|---|---|---|
| `sa.get_entity_by_name(name)` | entity | first match in iteration order — names are not unique; invalid when absent, so check `:valid()` |
| `sa.find_all_by_name(name)` | entity array | every match (1-indexed); the multi-match `get_entity_by_name` cannot give |
| `sa.find_by_uuid(uuid)` | entity | resolve a decimal-string uuid (matching `:uuid()`); invalid when absent |
| `sa.primary_camera()` | entity | the first primary `CameraComponent` entity; moving its transform is "move camera". Invalid when the scene has none (the viewport falls back to the fly-cam) |
| `sa.spawn(name)` | entity | create a new entity (a clean root with a `RelationshipComponent`); takes effect immediately |

**Physics queries** (no-ops returning a miss when no play-time physics world exists)

| Function | Returns | Notes |
|---|---|---|
| `sa.raycast(ox, oy, oz, dx, dy, dz, maxDist)` | `sa.RayHit` | closest ray hit: `.hit`, `.distance`, `.point`, `.normal`, `.entity` |
| `sa.spherecast(ox, oy, oz, dx, dy, dz, radius, maxDist)` | `sa.RayHit` | swept-sphere variant of the above |

**Messaging and timers** — see [messaging](#messaging) and [timers](#timers-and-coroutines).

Entity handle methods (`self.entity` and anything the queries above return):

**Identity and components**

| Method | Returns | Notes |
|---|---|---|
| `:valid()` | boolean | false for a missed lookup or a destroyed entity |
| `:name()` | string | the `NameComponent`; `""` when absent |
| `:uuid()` | string | the stable `IdComponent` id as a decimal string |
| `:get_component(name)` | table or nil | a read-only snapshot of any registered component in its serialized wire shape (vectors as `{x, y, z}` tables — **not** `sa.Vec3` — ids as decimal strings); nil when absent or the name is unknown. Mutating the returned table writes nothing back — use `:set_component` |
| `:set_component(name, table)` | boolean | write any registered component from a wire-shape table; true on success. Refused for structural components (transform-graph, mesh/skinning, physics-body) — those have dedicated methods or are editor-only |
| `:add_component(name)` | boolean | add a default-constructed component; same structural gate |
| `:remove_component(name)` | boolean | remove a component; same structural gate |
| `:has_component(name)` | boolean | whether the component is present |

`name` is a registered component name — `"Transform"`, `"Camera"`, `"DirectionalLight"`,
`"AnimationPlayer"`, `"Script"`, and so on. In `library/sa.lua` it is typed as the string-literal union
`sa.ComponentName`, so the editor autocompletes and validates the spelling; the runtime resolves it by
string and a typo is a logged miss (`nil` / `false`). `get_component`/`has_component` accept every
registered name; `set_component`/`add_component`/`remove_component` reject the structural ones.

`get_component` is further typed **per component** via a `---@overload` per name, so the editor knows the
*shape* of what comes back: `get_component("DirectionalLight")` is an `sa.DirectionalLight` with
`.color`/`.intensity`/`.ambient`, and `local m = get_component("Mesh"); m.` autocompletes the Mesh
fields. These `sa.<Component>` classes are **generated** from the same component wire-shape catalog the
TypeScript protocol uses, so they never drift from the serde — the whole `sa` def file is one generated
artifact, and the gate fails if re-running the generator changes it.

**Transform**

| Method | Returns | Notes |
|---|---|---|
| `:get_position()` | `sa.Vec3` | local `TransformComponent.translation` |
| `:get_rotation()` | `sa.Vec3` | local Euler XYZ, radians |
| `:get_scale()` | `sa.Vec3` | local scale |
| `:get_world_position()` | `sa.Vec3` | translation of the composed world matrix |
| `:get_world_rotation()` | `sa.Vec3` | Euler XYZ radians of the world matrix |
| `:set_position(v)` | — | write the local translation from an `sa.Vec3` |
| `:set_rotation(v)` | — | write local Euler XYZ radians |
| `:set_scale(v)` | — | write the local scale |

Transforms are local only: a snapshot read after a same-tick setter sees the written local value,
but world matrices refresh at render, after the tick. A setter on an entity without a transform —
or any access outside a script callback — is a logged no-op, never a crash.

**Hierarchy and lifecycle**

| Method | Returns | Notes |
|---|---|---|
| `:parent()` | entity | the `RelationshipComponent` parent; invalid for a root |
| `:children()` | entity array | the direct children (1-indexed) |
| `:set_parent(other)` | boolean | reparent under `other`; false (and unchanged) on a self-parent or a cycle. Takes effect immediately and relinks the hierarchy |
| `:destroy()` | — | mark for removal; the entity and its subtree are deleted after the tick, so the rest of this tick still sees it |

**Physics** (Dynamic-rigidbody methods are no-ops off a body; `move_character` drives a `CharacterVirtual`)

| Method | Returns | Notes |
|---|---|---|
| `:apply_impulse(v)` | — | add an instantaneous impulse to a Dynamic rigidbody |
| `:add_force(v)` | — | accumulate a force on a Dynamic rigidbody for the step |
| `:set_velocity(v)` | — | set the linear velocity of a Dynamic rigidbody |
| `:get_velocity()` | `sa.Vec3` | the linear velocity (zero off a body) |
| `:move_character(velocity, jump)` | — | request movement on a character controller; `jump` is an optional boolean |
| `:enable_ragdoll()` | boolean | start a motor-driven ragdoll blend on a skinned entity |
| `:disable_ragdoll()` | — | end the ragdoll blend |
| `:set_ragdoll_blend(active, weight)` | — | tune the active flag and body-follow weight of an ongoing blend |
| `:ragdoll_state()` | table | `{ present, active, bodyWeight, bones }` for inspection |

**Messaging** — `:send(handler, payload)` queues a call, see [below](#messaging).

### Messaging

`self.entity:send("on_hit", payload)` and `sa.broadcast("on_hit", payload)` queue a message to one
entity or to every scripted entity. Messages are **not** delivered inline — they are collected and
dispatched after the tick's `on_update` pass, so a handler never reenters the loop mid-iteration.
A delivered message invokes the named method on each matching instance as
`handler(self, sender, payload)`: `sender` is the entity that sent it (invalid for a broadcast from
outside a script), and `payload` is whatever value was passed (a table, a number, an `sa.Vec3`).
The handler name is the script's own — a class opts in simply by defining a method of that name.

### Timers and coroutines

The runtime installs a small coroutine scheduler so scripts can suspend across ticks:

- `sa.wait(seconds)` — yield the current coroutine for a duration, then resume. Outside a coroutine
  it logs and returns rather than erroring.
- `sa.delay(seconds, fn)` — run `fn` once after a delay.
- `sa.spawn_task(fn, ...)` — start a coroutine that can itself `sa.wait`.

Scheduled work advances each tick after `on_update`, on the same fixed-step dt, and is discarded
with the VM on Stop.

### Editor autocomplete

Every project is scaffolded with `library/sa.lua` (a LuaLS `---@meta` description of the whole
surface above) and a `.luarc.json` pointing the language server at it and disabling the sandboxed-out
libraries. Open the project in VS Code with the Lua (sumneko) extension and `self.entity:` completes
to the method set, vectors type as `sa.Vec3`, component names autocomplete from the `sa.ComponentName`
union, and a wrong-arity call is flagged before play. `sa.lua` is an engine-owned generated artifact —
it is **rewritten on every project open** so the definitions track the engine's API and never go stale
(do not hand-edit it). `.luarc.json` holds your editable LuaLS settings, so it is written only when
absent and never clobbered. The whole def file — the `sa.*` API surface and the per-component types —
is **generated from the one binding-descriptor table** the runtime registers from, so there is no second
copy to drift: the gate re-runs the generator and fails on any diff (the old hand-synced def-drift
tripwire is gone).

To get the same completion on a script's own state, annotate the class: `---@class Foo : sa.ScriptSelf`
types `self.entity`, and one `---@field <name> <type>` per `properties` entry or runtime field (with
`sa.Vec3` for vectors) types `self.<name>`. LuaLS cannot infer the engine-injected `properties` fields
on its own, so those annotations are what light them up.

## In the code

| What | File | Symbols |
|---|---|---|
| The data-only component | `scene/src/component.rs` | `Script`, `ScriptSlot` |
| The per-session runtime | `script/src/runtime.rs` | `ScriptHost`, `start_scripts`, `tick_scripts`, `stop_scripts`, `build_instance` |
| The entity facade | `script/src/entity.rs` | `EntityHandle` (the `sa.Entity` `UserData` impl) |
| The `sa.Vec3` value type | `script/src/value.rs` | `SaVec3`, `vec3`, `lerp`, `look_at` |
| The declarative `sa.*` binding table | `script/src/bindings.rs` | `BINDINGS`, `register_no_scene_globals`, `register_scene_globals`, `register_value_types` |
| Deferred structural ops + messaging + scheduler | `script/src/runtime.rs`; `script/src/scheduler.rs` | `flush_structural_ops`, `ScriptHost::dispatch_messages`, `ScriptHost::advance_scheduler`; `scheduler::install`, `SCHEDULER_PRELUDE` |
| The session guard (live-scene invariant) | `script/src/session.rs` | `enter_session`, `ScopedSession`, `with_scene`, `queue_message`, `defer_destroy` |
| Input edges (raw → derived) | `scene/src/script_input.rs` | `ScriptInputState`, `derive_script_input_edges` |
| The physics/log bridge | `script/src/bridge.rs`; `host/src/script_bridge.rs` | `ScriptHostBridge`, `NoopBridge`; `HostScriptBridge` |
| Host play-edge wiring | `host/src/layer.rs` | `start_scripts`/`tick_scripts`/`stop_scripts` calls, the `sim_tick` closure, `install_bridge` |
| The tick + lifecycle seams | `sceneedit/src/context.rs`; `sceneedit/src/play.rs` | `SceneEditContext::sim_tick`, `on_play_state_changed`, `push_script_error` |
| Status, input, and error drain commands | `control/src/commands_scene.rs` | `get-script-status`, `script-input`, `drain-script-errors` |
| The Inspector slot UI | `editor/src/components/ScriptSlots.tsx` | `ScriptSlots` |
| The src/ scaffold + starter script | `assets/src/project.rs` | `ensure_script_src`, `STARTER_SCRIPT` |
| The `library/sa.lua` + `.luarc.json` scaffold | `assets/src/project.rs` | `ensure_script_library`, `LUARC_JSON` |
| Generated `sa` Luau defs (API + per-component) | `xtask gen-protocol` → `schemas/control/sa.generated.luau` | `emit_api_defs`, `emit_component_defs`, `emit_defs` (from the `BINDINGS` table) |
| New-script boilerplate (`create-script`) | `assets/src/project.rs`; `control/src/commands_asset.rs` | `create_project_script` |
| Error toasts during play | `editor/src/lib/scriptErrorToasts.ts` | `routeScriptErrorToasts` |
| End-to-end coverage | `tests/e2e/script.test.ts` | component write / vectors / spawn-reparent-destroy / messaging / input edges / physics bindings |

## Related

- [Lua runtime](../lua-runtime/) — the VM, sandboxing, and the errors-as-values boundary underneath
- [Play mode](../../ui-and-editor/play-mode/) — the duplicate-and-discard state machine this rides on
