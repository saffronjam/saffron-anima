+++
title = 'Scripting'
weight = 16
bookCollapseSection = true
+++

# Scripting

Entities run gameplay logic written in Lua. An entity carries a `ScriptComponent` — an ordered
list of `.lua` script slots — and on Play each slot becomes a live instance whose
`on_update(self, dt)` runs every tick against the throwaway play duplicate. The engine embeds a
Lua 5.5 VM behind the `Saffron.Script` module, which is the only place Lua exists: the rest of
the engine sees a small `se::` facade of plain structs and `Result`-returning functions, and the
Host owns the VM and wires it to the play loop. Script errors become values, never crashes.

Authoring is editor-first: every project carries a `src/` folder (scaffolded with a starter
`example.lua`), the Inspector renders the Script component as ordered slots with each script's
declared fields as widgets — New Script writes a class-table boilerplate (`create-script`) and
assigns it in one step — the project menu jumps to the sources with Open in VS Code, and a
contained script error during play raises a toast carrying the traceback.

## Pages

| Page | Covers | Code |
|---|---|---|
| `lua-runtime` | the embedded Lua 5.5 VM, the sandboxed library set, errors as `Result` with tracebacks | `script.cppm` |
| `script-components-and-runtime` | `ScriptComponent` slots, the class-table script shape, the play lifecycle, error containment + the drain commands, the `se`/entity API reference | `script_runtime.cpp`; `scene_edit_context.cppm`; `host.cppm` |
| `script-declared-fields` | the `properties` table, inferred types, defaults-in-Lua vs overrides-in-scene, the edit-time schema reader + commands | `script_runtime.cpp`; `host.cppm` |
