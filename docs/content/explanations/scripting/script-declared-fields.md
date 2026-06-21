+++
title = 'Script-declared fields'
weight = 3
+++

# Script-declared fields

A script declares its own editable fields in a `properties` table; the engine reads that schema at
edit time, the editor renders widgets for it, and each slot stores only the values the user
changed. The `.lua` file owns the fields and their defaults — the scene never duplicates them, so
editing a default in the script updates every instance that has no override.

```lua
---@class Turret : sa.ScriptSelf
---@field speed number
local Turret = {}

Turret.properties = {
  speed   = 5.0,                -- number   -> float
  target  = "Player",          -- string   -> text
  enabled = true,              -- bool     -> checkbox
  offset  = sa.vec3(0, 1, 0),  -- sa.Vec3  -> vec3
}

function Turret:on_update(dt)
  if self.enabled then
    local p = self.entity:get_position()
    self.entity:set_position(p + sa.vec3(0, self.speed * dt, 0))
  end
end

return Turret
```

## How it works

Each field's type is inferred from its default's Luau type: a number, a boolean, a string, or an
`sa.Vec3` (built with `sa.vec3(x, y, z)`). Anything else — an arbitrary table, a function, a bare
3-number list — is skipped with a logged note rather than an error; a richer descriptor form
(`{ default = 5, min = 0 }`) is a deferred extension. The `---@class` annotation is optional but
lets the LuaLS server (see [the scaffolded `library/sa.lua`](../script-components-and-runtime/#editor-autocomplete))
type `self` and autocomplete the field and method set.

The schema is read in a throwaway sandboxed VM with the same minimal library set as the play
runtime (no io/os/debug/package): the chunk runs just far enough to return its class table, so
property declaration must be side-effect-free — `read_script_schema` reads only `properties`, never
running `on_create`/`on_update`. This happens at edit time only — on assign or when the Inspector
asks — never per frame. Running project-local code at edit time is a deliberate, sandbox-bounded
trade-off for a single-user editor. Each surviving entry comes back as a `ScriptField` carrying its
name, the inferred `ScriptFieldType`, and the default as a `serde_json::Value` (a scalar, or a
3-number array for a vec3 — the shape override storage round-trips).

At instance creation `build_instance` calls `inject_fields`, which writes `declared defaults ⊕ slot
overrides` directly onto `self`, so `on_update` reads `self.speed` with no lookup ceremony.
Overrides win; a stale override key (field renamed or removed in the script) is silently dropped; a
missing override falls back to the default. Table-shaped defaults are materialized per instance, so
mutating `self.offset` never bleeds into other instances. Two control commands carry the feature:
`get-script-schema` returns the declared fields (registered by the host, which alone reaches the
Luau runtime), and `set-script-override` writes one override onto a slot — a null value clears it.

## In the code

| What | File | Symbols |
|---|---|---|
| The edit-time schema reader | `script/src/schema.rs` | `read_script_schema`, `ScriptField`, `ScriptFieldType` |
| Defaults ⊕ overrides injection | `script/src/runtime.rs` | `inject_fields`, `build_instance` |
| Override storage | `scene/src/component.rs` | `ScriptSlot` (the `overrides` field) |
| The control commands | `control/src/commands_scene.rs`; `control/src/registry.rs` | `set-script-override`; `get-script-schema` |
| The Inspector field widgets + reset | `editor/src/components/ScriptSlots.tsx` | `ScriptSlots` |
| End-to-end coverage | `tests/e2e/script.test.ts` | schema read / default vs override / stale-key reconciliation |

## Related

- [Script components and the play runtime](../script-components-and-runtime/) — the slots these overrides live on
- [Lua runtime](../lua-runtime/) — the sandboxed VM the schema reader reuses
