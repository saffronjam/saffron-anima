+++
title = 'Scripting'
weight = 16
bookCollapseSection = true
+++

# Scripting

Entities run gameplay logic written in Lua. An entity carries a `Script` component — an ordered
list of `.lua` script slots — and on Play each slot becomes a live instance whose
`on_update(self, dt)` runs every tick against the throwaway play duplicate. The engine embeds a
Luau VM in the `saffron-script` crate, which is the only place the VM exists: the rest of the
engine sees a small surface of plain types and `Result`-returning functions, and the host owns the
VM and wires it to the play loop. Script errors become values, never crashes — every failure
carries a Luau traceback and pauses play rather than taking down the host.

Authoring is editor-first: every project carries a `src/` folder (scaffolded with a starter
`example.lua`) and a `library/sa.lua` + `.luarc.json` that give VS Code full autocomplete and
type-checking for the `sa` surface; the Inspector renders the Script component as ordered slots with
each script's declared fields as widgets — New Script writes a class-table boilerplate
(`create-script`) and assigns it in one step — the project menu jumps to the sources with Open in VS
Code, and a contained script error during play raises a toast carrying the traceback.

Scripts reach the engine through a deliberately small but complete `sa` API: typed `sa.Vec3` math,
read/write access to any non-structural component, transform and hierarchy control, spawn and
destroy, per-tick input (held keys, edges, mouse), physics impulses/queries and the ragdoll blend,
entity messaging, and a coroutine scheduler (`sa.wait`/`sa.delay`). The full reference lives on the
`script-components-and-runtime` page.

## Pages

| Page | Covers | Code |
|---|---|---|
| `lua-runtime` | the embedded Luau VM, the sandboxed library set, the instruction/memory budgets, errors as `Result` with tracebacks | `script/src/vm.rs`; `script/src/error.rs` |
| `script-components-and-runtime` | `Script` slots, the class-table script shape, the play lifecycle, error containment + the drain commands, the `sa`/entity API reference | `script/src/runtime.rs`; `script/src/entity.rs`; `host/src/layer.rs` |
| `script-declared-fields` | the `properties` table, inferred types, defaults-in-Lua vs overrides-in-scene, the edit-time schema reader + commands | `script/src/schema.rs`; `script/src/runtime.rs` |
