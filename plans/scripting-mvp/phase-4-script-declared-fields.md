# Phase 4 — script-declared editable fields

**Status:** COMPLETED

## Goal

Let a script **declare its own editable fields** in a `properties` table, have the engine read that schema
**at edit time without running gameplay**, store per-instance **overrides** on the script slot, and inject
`declared defaults ⊕ overrides` into `self` at instance creation. This phase is engine + control plane
only; the Inspector UI that renders the fields is Phase 5. The `.lua` owns the fields and their defaults;
the scene stores only overrides.

## What exists to build on

- The class-table script shape + the per-entity instancing runtime from Phase 2 (`startScripts` builds
  `self` per slot — this is where merged fields get injected).
- `ScriptSlot { std::string scriptPath; Json overrides; }` from Phase 2 — `overrides` already exists to
  hold per-instance values.
- `ScriptVm` / `runFile` from Phase 1 — reused to load a script in a throwaway VM for schema reading.
- The control command + DTO codegen pipeline (`registerCommand`, `control_dto.cppm`, `gen.ts`,
  `bun run tools/gen-control-dto/gen.ts`).

## The declaration shape

```lua
local Turret = {}

Turret.properties = {
  speed   = 5.0,            -- number  -> float
  target  = "Player",      -- string  -> text
  enabled = true,          -- bool    -> checkbox
  offset  = { 0, 1, 0 },   -- 3 numbers -> vec3
}

function Turret:on_update(dt)
  if self.enabled then
    local x, y, z = self.entity:get_position()
    self.entity:set_position(x, y + self.speed * dt, z)
  end
end

return Turret
```

The type is **inferred from each default's Lua type**: number→float, boolean→bool, string→string, a
3-number array→vec3. (A richer descriptor form — `speed = { default = 5, min = 0 }` — is a deferred
extension; the MVP reads bare values.)

## Work

### 1. The edit-time schema reader (`Saffron.Script`)

```cpp
export namespace se
{
    enum class ScriptFieldType { Number, Bool, String, Vec3 };
    struct ScriptField { std::string name; ScriptFieldType type; Json defaultValue; };
    auto readScriptSchema(std::string_view path) -> Result<std::vector<ScriptField>>;
}
```

- `readScriptSchema`: create a **fresh, sandboxed `ScriptVm`** (the Phase-1 minimal lib set — base/string/
  math/table/utf8, no io/os/debug), `runFile(path)`; the chunk returns the class table; read its
  `properties` subtable; reflect each entry into a `ScriptField` (infer the type, capture the default as
  `Json`). Discard the VM. **No gameplay runs** — the chunk only builds tables. A load error → `Err`
  (surfaced to the editor as an invalid-script notice).
- Convention (document it): property declaration is **side-effect-free**; a script with heavy top-level
  effects would run them here.
- Type inference rule for ambiguity: a **table of exactly 3 numbers** is a vec3; anything else unhandled is
  skipped with a logged note (until the descriptor form lands).

### 2. Inject merged values into `self` (extend Phase 2's `startScripts`)

When building each instance `self`, before the first tick:
1. read the class's declared defaults (the class table is already loaded — read its `properties`);
2. overlay the slot's `overrides` (Phase 2 `ScriptSlot.overrides`), keyed by field name;
3. set each merged value onto `self` (`self.speed = 8`, `self.target = "Player"`, …) so `on_update` reads
   `self.<field>` directly.

Overrides win over defaults; unknown override keys (renamed/removed fields) are ignored; missing overrides
fall back to the default.

### 3. Control command (keep-current)

- `get-script-schema { path: std::string } -> { fields: std::vector<ScriptFieldDto> }` where
  `ScriptFieldDto { std::string name; std::string type; Json defaultValue; }`. These are *control* DTOs in
  `control_dto.cppm`, so the generic codegen path applies, and a `Json` result field is supported and
  proven — `cppJsonValue` passes it through (`gen.ts:700`) and `InspectResult.components`
  (`control_dto.cppm:845`) is the precedent. The handler calls `readScriptSchema`. Register it from the Host (capturing the `ScriptHost`, like Phase 2's status command)
  so `Saffron.Control` stays free of any `Saffron.Script` import — or, if simpler, as a thin scene command
  that forwards through a `std::function` the Host installs. Declare DTOs in `control_dto.cppm`, add to
  `gen.ts` (fixture: a seeded test script path, or a skip if it reads disk), regenerate, commit.
- Setting an override is just writing the slot's `overrides` — reuse the existing component-set path
  (`set-component`-style) or add a small `set-script-override { entity, slot, name, value: Json }` command.

## e2e (extend `tests/e2e/script.test.ts`)

Author `src/mover.lua` with `properties = { speed = 2.0 }` and an `on_update` that moves `self.entity` at
`self.speed`. Then:
1. `get-script-schema` returns one field `speed` (type number, default 2.0).
2. Attach the slot with **no** override → `play` → assert the entity moved at the **default** rate.
3. `stop`, set an override `speed = 10` on the slot → `play` → assert it moved at the **overridden** rate.
4. Reconciliation: change the script's `properties` (e.g. rename `speed`) and assert the stale override is
   ignored (the new default is used). Close with the `validationErrors()` case.

## Gate / done

- `make engine` + `make prepare-for-commit` clean; `make e2e` green; `bun run check` passes (protocol
  regen).
- `scripting.md` gains the **declared fields** section: the `properties` shape, the inferred types, the
  defaults-in-Lua / overrides-in-scene split, and the edit-time-read caveat; hub row kept.

## Risks

- Executing a project script at edit time to read its schema is a (mild) code-exec surface; keep it
  sandboxed (minimal libs) and limited to project-local scripts. Acceptable for a single-user editor; note
  it for any future shared-content path.
- Type inference is lossy (e.g. an empty table, a number meant as an int, a 3-number list that isn't a
  vec3). Document the rules; the descriptor form removes the ambiguity later.
- Override reconciliation must be total: a schema change (rename/remove/add) must not crash the merge —
  drop unknowns, fill new defaults.
- `Json` field support is confirmed: the control-DTO `defaultValue` passes through the generic codegen
  (`gen.ts:700`, precedent `InspectResult.components`), and the component `overrides` round-trips via the
  hand-authored scene serde (Phase 2). No fallback needed.
- Keep schema reads off the hot path — they happen at edit time / on assign, never per frame.
