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
---@class Turret : se.ScriptSelf
---@field speed number
local Turret = {}

Turret.properties = {
  speed   = 5.0,                -- number   -> float
  target  = "Player",          -- string   -> text
  enabled = true,              -- bool     -> checkbox
  offset  = se.vec3(0, 1, 0),  -- se.Vec3  -> vec3
}

function Turret:on_update(dt)
  if self.enabled then
    local p = self.entity:get_position()
    self.entity:set_position(p + se.vec3(0, self.speed * dt, 0))
  end
end

return Turret
```

## How it works

Each field's type is inferred from its default's Lua type: number, boolean, string, or an `se.Vec3`
(built with `se.vec3(x, y, z)`). Anything else — an arbitrary table, a bare 3-number list — is
skipped with a logged note rather than an error; a richer descriptor form
(`{ default = 5, min = 0 }`) is a deferred extension. The `---@class` annotation is optional but
lets the LuaLS server (see [the scaffolded `library/se.lua`](../script-components-and-runtime/#editor-autocomplete))
type `self` and autocomplete the field and method set.

The schema is read in a throwaway VM with the same minimal library set as the play runtime
(no io/os/debug/package): the chunk runs just far enough to return its class table, so property
declaration must be side-effect-free. This happens at edit time only — on assign or when the
Inspector asks — never per frame. Running project-local code at edit time is a deliberate,
sandbox-bounded trade-off for a single-user editor.

At instance creation the runtime sets `declared defaults ⊕ slot overrides` directly onto `self`,
so `on_update` reads `self.speed` with no lookup ceremony. Overrides win; a stale override key
(field renamed or removed in the script) is silently dropped; a missing override falls back to the
default. Table defaults are copied per instance, so mutating `self.offset` never bleeds into other
instances. Two control commands carry the feature: `get-script-schema` returns the declared fields
(registered by the Host, which alone may reach the Lua runtime), and `set-script-override` writes
one override onto a slot — a null value clears it.

## In the code

| What | File | Symbols |
|---|---|---|
| The edit-time schema reader | `script_runtime.cpp` | `readScriptSchema`, `ScriptField`, `ScriptFieldType` |
| Defaults ⊕ overrides injection | `script_runtime.cpp` | `injectFields`, `makeInstance` |
| Override storage | `scene.cppm` | `ScriptSlot::overrides` |
| The control commands | `host.cppm`; `control_commands_scene.cpp` | `get-script-schema`; `set-script-override` |
| The Inspector field widgets + reset | `ScriptSlots.tsx` | `ScriptSlots` |
| End-to-end coverage | `tests/e2e/script.test.ts` | schema read / default vs override / stale-key reconciliation |

## Related

- [Script components and the play runtime](../script-components-and-runtime/) — the slots these overrides live on
- [Lua runtime](../lua-runtime/) — the sandboxed VM the schema reader reuses
