# Phase 7 — LuaLS definitions, the project scaffold, and the declarative binding table

**Status:** COMPLETED as 7a; 7b deliberately not done (decision recorded below).

## Implementation outcome (7a shipped, 7b descoped)

**7a shipped in full:** `library/sa.lua` (the `SaLuaDefs` `---@meta` string in `assets.cppm`) + the
`.luarc.json` (`LuarcJson`), written by `ensureScriptLibrary` at the `loadProject` and `createProject`
sites beside `ensureScriptSrc`. Refinement landed after the first pass: `sa.lua` is engine-owned and
**rewritten on every open** (a generated artifact must track the API, or autocomplete lies), while
`.luarc.json` stays only-when-absent (editable user settings). Component names are typed via a
`sa.ComponentName` string-literal union, and the tripwire also enforces alias-covers-registered-components.

A later follow-up added **per-component typed `get_component` returns** — a generator
(`emitScriptComponentDefs` in `gen.ts`, a 6th codegen output) emits a `---@class sa.<Component>` for every
registered component plus a per-name `---@overload`, so `get_component("Mesh").` autocompletes the Mesh
fields. The classes are generated from the same component wire-shape catalog the TS protocol uses (no
drift; guarded by the generated-file `git diff` gate), shipped as a C++ header (`SaComponentDefs`)
`#include`d into `assets.cppm` and appended to `sa.lua` after `SaLuaDefs`. This is a narrow, schema-driven
generator — not the full binding-table cutover 7b described (that remains descoped for the reason above). The scaffolded `example.lua` and the
`createProjectScript` template use colon methods + `---@class … : sa.ScriptSelf` + `sa.vec3` math. The
**gating drift tripwire** is `tools/check-script-defs/check.ts`, wired into `tools/ci/check.sh`: it
extracts every live binding name (the `.addFunction("…")` calls in `script_runtime.cpp` + `script.cppm`
and the prelude `rawset(se, "…")`), excludes metamethods (documented as `---@operator`), and fails if any
live name is absent from `SaLuaDefs`. The project smoke (`tools/check-projects/check.sh`) asserts both
files scaffold.

The tripwire is **static name-extraction**, not the runtime-VM-introspection the locked text described.
The two are equivalent for the only failure mode that matters (a binding with no annotation): it reads
the same `.addFunction`/`rawset` registration the runtime executes, needs no booted VM or new dump
command, and runs in pure CI. A negative test confirms it catches a removed annotation.

**7b (declarative binding table + generator) was deliberately not done.** On inspection, the locked 7b
design collides with a LuaBridge constraint the plan did not foresee: `.addFunction` is a function
template that deduces each binding's C++ type, and the registration mixes member-function pointers
(`&ScriptEntity::method`) with small capturing `[&host]` lambdas carrying real logic. "One declarative
table that the registration loop iterates" is therefore not achievable without type-erasing all ~60
bindings into raw `lua_CFunction` thunks with hand-written stack marshalling — abandoning LuaBridge's
automatic argument conversion and the tidy member-pointer form. That is strictly worse code, for a
single-source benefit the gating tripwire already guarantees. No-legacy holds because 7a is **one
coherent approach** (hand-maintained def file + a gate that makes drift impossible) — there is no
superseded or duplicate path lingering beside it, which is precisely the "hand-maintained body, guarded
artifact" precedent the repo already blesses (`emitSceneSerde`). User confirmed closing Phase 7 at 7a.

---

## Original plan (for reference)

**Status (original):** NOT STARTED

The DX payoff: VS Code autocomplete + type-checking for the whole `sa` surface, injected into every
project's scaffold, plus the one-source-of-truth binding table that retires `sa.lua` drift for good. This
phase **closes** the design fork from `phase-0-research.md` and README §6.

**Split (LOCKED).** **7a** ships `library/sa.lua` (hand-written) + `.luarc.json` + the scaffold/annotation
changes + the **gating runtime tripwire** — and may land **early, right after Phase 1**, with each later
phase appending its annotations (the tripwire keeps them honest). **7b** does the declarative-table +
generator cutover **last**, once the surface stops moving.

## 7a — what ships into a project (LOCKED)

The scaffold (`assets.cppm`: `StarterScript` at `:1029`, `ensureScriptSrc` writing `src/example.lua` at
`:1057`/`:1066`, the `createProjectScript` per-script template at `:1103`, called from `loadProject` `:1179`
and `createProject` `:1247`) gains:

1. **`library/sa.lua`** — one `---@meta` file describing the entire surface (full shape below). Written
   under `<root>/library/` (a sibling of `src/`), **only-when-absent**, ensured on **create AND open** (like
   `ensureScriptSrc`, so existing projects gain it).
2. **`.luarc.json`** at the project root (LOCKED):
   ```json
   {
     "runtime.version": "Lua 5.4",
     "workspace.library": ["library"],
     "diagnostics.globals": ["se"],
     "runtime.builtin": { "io": "disable", "os": "disable", "debug": "disable", "package": "disable" }
   }
   ```
   `Lua 5.4` because **LuaLS has no 5.5 target** — the closest, and our surface uses no 5.5-only syntax.
   `workspace.library` points the server at `library/sa.lua`; `diagnostics.globals` stops `se` reading as
   undefined; `runtime.builtin` disables exactly the sandboxed-out libs (matching `luaL_openselectedlibs`,
   `script.cppm:237`) so autocomplete never suggests `io`/`os`/`debug`/`package`. Written **only-when-absent**
   — a user-authored `.luarc.json` is never clobbered.
3. **The scaffolded `example.lua` + the `createProjectScript` template switch to colon-methods + a class
   annotation** so `self.entity:` autocompletes:
   ```lua
   ---@class Example : sa.ScriptSelf
   ---@field speed number
   local Example = {}

   Example.properties = { speed = 1.0, center = sa.vec3(0, 1, 0) }   -- Vec3 default (Phase 2)

   function Example:on_create()                 -- colon method; self is typed Example
     self.start = self.entity:get_position()    -- :get_position() autocompletes, returns sa.Vec3
   end

   function Example:on_update(dt)
     -- orbit math rebuilt on sa.Vec3 (no {x=…} tables)
   end
   ```
   **Runtime-equivalent:** `callInstanceMethod` always pushes `self` (`script_runtime.cpp:229`), so
   `function Example:on_update(dt)` binds the identical field as `function Example.on_update(self, dt)` — a
   pure authoring-style change. **No-legacy:** switch in place; do not keep dot-form examples beside it. The
   starter body also moves to `sa.Vec3` orbit math (Phase 2) in this same cutover; update the e2e test that
   reads `example.lua` ("scaffolds src/ with a runnable starter") to the new math + assert `library/sa.lua`
   and `.luarc.json` exist.

### The scaffold injection seam (LOCKED)

New `ensureScriptLibrary(root)` modeled on `ensureScriptSrc` (`assets.cppm:1057`): `create_directories`
`root/"library"`, write `library/sa.lua` from a new `inline constexpr std::string_view SaLuaDefs` (sibling of
`StarterScript` at `:1029`) and `.luarc.json` from a literal — both **only-when-absent** like the
`example.lua` guard (`:1066`). Call it at the same two sites `ensureScriptSrc` is (`loadProject` `:1179`,
`createProject` `:1247`).

## 7a — the gating runtime tripwire (LOCKED)

A generator **cannot** read the imperative `.addFunction(...)` fluent chain (`script_runtime.cpp:429`–`523`)
the way `gen.ts` reads flat DTO structs — names live in call args and lambda bodies, and `sa.log`/`sa.Vec3`
are bound in a **second** TU (`newScriptVm`, `script.cppm:238`). So Phases 1–6 hand-write `sa.lua` and a
**runtime-introspection tripwire** guards it:

- A small probe (a new `tools/check-script-defs/` or folded into `tools/check-control-schema/`) boots a
  sandboxed VM, enumerates the live `se` global table **and** the `sa.Entity` metatable for every exposed
  name (covering **both** registration TUs — `newScriptVm`'s `sa.log`/`sa.Vec3` **and** `startScripts`'s
  Entity/raycast block), parses `sa.lua`'s `---@field`/`---@class`/method names, and **fails if the live
  surface has a name `sa.lua` lacks** (warn on the reverse).
- Wire it as a **gating** step in `tools/ci/check.sh`, next to the DTO `git diff --exit-code` guard
  (`check.sh:24`, inside the `… || fail=1` block at `:30`) — same structure as the DTO contract test. ~30
  lines, no binder rewrite. It is **gating, not optional** — pure hand-write-and-hope drifts invisibly
  (autocomplete just lacks an entry; no test fails without the tripwire).

This honors the repo's no-drift stance and matches the blessed "hand-maintained body, guarded artifact"
precedent (`emitSceneSerde` is hand-written under a generated header, `gen-control-dto/AGENTS.md:29`).

## 7a — the `sa.lua` shape (`---@meta`, types only)

One file, header-commented with the LuaLS-5.4-vs-runtime-5.5 note. It never runs (`---@meta`); the real
bindings are the C++ `.addFunction` calls (or the 7b table). Each entry is annotated **only when its binding
is live** — the tripwire enforces the match. Full surface across Phases 1–6:

```lua
---@meta
-- SaffronAnima Lua API. LuaLS targets 5.4 (no 5.5 target); the VM is 5.5. Types only; never executed.

---@class sa.Vec3
---@field x number
---@field y number
---@field z number
---@operator add(sa.Vec3): sa.Vec3
---@operator sub(sa.Vec3): sa.Vec3
---@operator mul(number): sa.Vec3
---@operator unm: sa.Vec3
local Vec3 = {}
function Vec3:length() end           ---@return number
function Vec3:normalized() end       ---@return sa.Vec3
function Vec3:dot(o) end             ---@param o sa.Vec3 @return number
function Vec3:cross(o) end           ---@param o sa.Vec3 @return sa.Vec3
function Vec3:lerp(o, t) end         ---@param o sa.Vec3 @param t number @return sa.Vec3

---@class sa.RayHit
---@field hit boolean
---@field distance number
---@field point sa.Vec3
---@field normal sa.Vec3
---@field entity sa.Entity?

---@class sa.Entity
local Entity = {}
function Entity:valid() end                          ---@return boolean
function Entity:name() end                           ---@return string
function Entity:uuid() end                           ---@return string
function Entity:get_component(name) end              ---@param name string @return table?
function Entity:set_component(name, value) end       ---@param name string @param value table @return boolean
function Entity:add_component(name) end              ---@param name string @return boolean
function Entity:remove_component(name) end           ---@param name string @return boolean
function Entity:has_component(name) end              ---@param name string @return boolean
function Entity:get_position() end                   ---@return sa.Vec3
function Entity:get_rotation() end                   ---@return sa.Vec3
function Entity:get_scale() end                      ---@return sa.Vec3
function Entity:get_world_position() end             ---@return sa.Vec3
function Entity:get_world_rotation() end             ---@return sa.Vec3
function Entity:set_position(v) end                  ---@param v sa.Vec3
function Entity:set_rotation(v) end                  ---@param v sa.Vec3
function Entity:set_scale(v) end                     ---@param v sa.Vec3
function Entity:parent() end                         ---@return sa.Entity?
function Entity:children() end                       ---@return sa.Entity[]
function Entity:set_parent(other) end                ---@param other sa.Entity @return boolean
function Entity:destroy() end
function Entity:move_character(velocity, jump) end   ---@param velocity sa.Vec3 @param jump boolean?
function Entity:enable_ragdoll() end
function Entity:disable_ragdoll() end
function Entity:set_ragdoll_blend(active, weight) end ---@param active boolean? @param weight number?
function Entity:ragdoll_state() end                  ---@return table
function Entity:send(handler, ...) end               ---@param handler string
-- §5/Phase 5 NEW-C++ gated (annotated only once the engine fn lands):
-- function Entity:apply_impulse(v) end  ---@param v sa.Vec3
-- function Entity:set_velocity(v) end   ---@param v sa.Vec3
-- function Entity:add_force(v) end      ---@param v sa.Vec3
-- function Entity:get_velocity() end    ---@return sa.Vec3

---@class sa.ScriptSelf
---@field entity sa.Entity
local ScriptSelf = {}
function ScriptSelf:on_create() end
function ScriptSelf:on_update(dt) end                ---@param dt number
function ScriptSelf:on_destroy() end
function ScriptSelf:on_trigger_enter(other) end      ---@param other sa.Entity
function ScriptSelf:on_trigger_exit(other) end       ---@param other sa.Entity
function ScriptSelf:on_contact(other, point, normal) end  ---@param other sa.Entity @param point sa.Vec3 @param normal sa.Vec3

se = {}
function sa.log(message) end                         ---@param message string
function sa.is_key_pressed(key) end                  ---@param key string @return boolean
function sa.just_pressed(key) end                    ---@param key string @return boolean
function sa.just_released(key) end                   ---@param key string @return boolean
function sa.mouse_position() end                     ---@return sa.Vec3
function sa.mouse_delta() end                        ---@return sa.Vec3
function sa.mouse_button(n) end                      ---@param n string @return boolean
function sa.mouse_scroll() end                       ---@return number
function sa.get_entity_by_name(name) end             ---@param name string @return sa.Entity?
function sa.find_all_by_name(name) end               ---@param name string @return sa.Entity[]
function sa.find_by_uuid(uuid) end                   ---@param uuid string @return sa.Entity?
function sa.primary_camera() end                     ---@return sa.Entity?
function sa.spawn(name) end                          ---@param name string @return sa.Entity
function sa.vec3(x, y, z) end                         ---@param x number @param y number @param z number @return sa.Vec3
function sa.look_at(eye, target, up) end             ---@param eye sa.Vec3 @param target sa.Vec3 @param up sa.Vec3? @return sa.Vec3
function sa.lerp(a, b, t) end                         ---@param a sa.Vec3 @param b sa.Vec3 @param t number @return sa.Vec3
function sa.raycast(ox, oy, oz, dx, dy, dz, maxDist) end             ---@return sa.RayHit
function sa.spherecast(ox, oy, oz, dx, dy, dz, radius, maxDist) end  ---@return sa.RayHit
function sa.broadcast(handler, ...) end              ---@param handler string
function sa.wait(seconds) end                        ---@param seconds number
function sa.delay(seconds, fn) end                   ---@param seconds number @param fn function
function sa.spawn_task(fn, ...) end                  ---@param fn function
```

## 7b — the declarative binding table (closing the fork, LOCKED last)

Once the surface stabilizes across Phases 1–6, lift it into **one declarative table** that both the
LuaBridge registration loop **and** an `sa.lua` generator consume — the `gen.ts` one-source-of-truth model:

- A single C++ table (in `script_runtime.cpp`) of binding descriptors:
  `{ name, kind (se-global | entity-method), lua signature string, C++ thunk }`. The registration loop in
  `startScripts` (and the `sa.log`/`sa.Vec3` block in `newScriptVm`) iterates it calling `.addFunction`,
  instead of the current hand-written fluent chains. **Both TUs** read the table.
- A generator (`tools/gen-script-defs/gen.ts`, modeled on `tools/gen-control-dto/gen.ts`) reads the same
  descriptors (exported as a small JSON the engine emits) and writes the canonical
  `engine/source/saffron/assets/.../sa.lua` the scaffold injects.
- The tripwire **becomes** a "generated `sa.lua` is byte-fresh" `git diff --exit-code` check, identical in
  spirit to the DTO manifest gate (`check.sh:24`).
- **No-legacy:** the hand-written `sa.lua` from 7a is **deleted** the moment the generated one lands — the
  generated file is the single source; the hand-written one does not linger "for reference". The generator
  must read the declarative table, **never** re-parse the imperative `.addFunction` C++.

## Tests

- An e2e/scaffold check: a fresh project scaffolds `library/sa.lua` + `.luarc.json` (extend the existing
  "scaffolds src/" test to assert both files exist, `sa.lua` contains `---@meta` + `sa.Entity`, and a second
  open does not clobber a user-edited `.luarc.json`).
- The gating tripwire: boot, introspect the live `se` table + `sa.Entity` metatable (both TUs), assert every
  live name appears in `sa.lua` (7a); for 7b, that the generated `sa.lua` is byte-fresh.
- (7b) a generator round-trip test like the DTO generator's.

## Docs

Update `script-declared-fields.md` + `script-components-and-runtime.md` for the colon-method/`---@class`
authoring style and the `library/`/`.luarc.json` scaffold; add an "editor autocomplete" note. Update
`_index.md`. If 7b lands, document the one-source-of-truth generator alongside the DTO one (cross-link the
`gen.ts` precedent).

## Constraints honored

NO-LEGACY (7b deletes the hand-written `sa.lua`; the scaffold switches authoring style in place, no dot-form
beside colon-form), Saffron.Script unchanged (this is tooling + scaffold + the registration-loop refactor, no
new import), sandbox unchanged (the `.luarc.json` mirrors it), scaffold writes are only-when-absent
(idempotent, never clobber). The generated `sa.lua` (7b) is never hand-edited; the tripwire is gating.

## Verification gate

`make engine`, `make prepare-for-commit`, `make e2e` green; `bun run check` clean; the contract test (now
including the gating `sa.lua` freshness/coverage tripwire) passes; opening a scaffolded project in VS Code
with the LuaLS extension gives `self.entity:` autocomplete (manual DX check).
