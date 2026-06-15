# tools/check-script-defs

The script-API drift tripwire. One Bun/TypeScript file, `check.ts`, asserts that every Lua binding the
engine exposes at runtime is also described in the `SaLuaDefs` LuaLS (`---@meta`) string in
`engine/source/saffron/assets/assets.cppm` — the file scaffolded into every project as `library/sa.lua`
for VS Code autocomplete.

## Why

The Lua surface is bound imperatively in C++ (`.addFunction("name", …)` in `script_runtime.cpp` /
`script.cppm`, plus a few prelude `rawset(sa, "name", …)`). Nothing forces the hand-written def file to
keep up, so a new binding silently leaves users without autocomplete and rots the docs. This check fails
the gate (`tools/ci/check.sh`) the moment a live name is missing from `SaLuaDefs`.

## What it checks

Names only — presence, not signatures. Two coverage checks, both by regex (no running VM needed):

1. **Bindings.** Every live `.addFunction("…")` / prelude `rawset(sa, "…")` name (excluding metamethods,
   which are documented as `---@operator` overloads) must appear as a `:name(` / `.name(` entry in the
   `R"(...)"` body of `SaLuaDefs`.
2. **Components.** Every `registerComponent<…>(reg, "Name", …)` in `scene_edit_components.cpp` must appear
   in the `---@alias sa.ComponentName` union (the typed name set for `get`/`set`/`add`/`remove`/`has_component`),
   so a newly registered component is spellable and typed in scripts.

When it fails, add the missing names/components to the `SaLuaDefs` string in `assets.cppm`.

Run from the repo root: `bun tools/check-script-defs/check.ts`.
