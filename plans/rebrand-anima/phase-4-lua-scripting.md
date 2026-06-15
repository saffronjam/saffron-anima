# Phase 4 — Lua scripting `se.` → `sa.`

**Status:** COMPLETED

Rename the user-facing Lua global table from `se` to `sa` across the runtime bindings, the type-def
file, the LuaLS config, and any shipped example scripts. This is the most user-visible surface of the
rebrand: every script people write will call `sa.*`.

## Runtime bindings — `engine/source/saffron/script/script_runtime.cpp`

- `namespace sa` → `namespace sa` (the C++ namespace housing the bindings; folds into phase 3's pass,
  re-confirmed here because the Lua-facing strings live alongside it).
- The Lua prelude injected at VM startup: `rawset(se, "spawn_task", …)`, `rawset(se, "wait", …)`,
  `rawset(se, "delay", …)` and any internal `sa.log(...)` calls → `rawset(sa, …)` / `sa.log(...)`.
  Here `se`/`sa` is the **Lua global table variable**, not a C++ symbol — change it explicitly.
- User-visible error strings: `"se: task error: "`, `"sa.wait called outside …"`, `"se: coroutine error:"`
  → `"sa: …"` / `"sa.wait …"`.

## Type defs & API surface — `engine/source/saffron/assets/assets.cppm`

- The `SaLuaDefs` constant → `SaLuaDefs`.
- The whole `se.*` API in the def block → `sa.*`: `---@class sa.Vec3` / `sa.Entity` / `sa.RayHit` /
  `sa.ScriptSelf`, the `---@alias sa.ComponentName …` union, and every function decl (`sa.log`,
  `sa.is_key_pressed`, `sa.vec3`, `sa.spawn`, `sa.wait`, `sa.delay`, `sa.spawn_task`, `sa.raycast`, …)
  → `sa.*`. Also the example snippet (`sa.ScriptSelf`, `sa.vec3(...)`) → `sa.*`.
- The emitted `.luarc.json`: `"diagnostics.globals": ["se"]` → `["sa"]`.
- The emitted def file path `library/sa.lua` → `library/sa.lua`.

Note the component `---@class …` lines that come from the **generator** are already `sa.*` after
phase 2; this phase covers the hand-authored core API in `assets.cppm` and the runtime prelude.

## Shipped example scripts / project `src/` scaffold

- Grep the engine assets and any project-scaffold templates for `se.` usage in `.lua` files (the
  scripting hub / scaffold ships example scripts). Update each to `sa.`.

## Verify

`make engine` clean. Boot headless and run a script that exercises the API over the control plane
(e.g. an entity with a `ScriptComponent` calling `sa.log` / `sa.vec3`) — confirm it runs and the log
is validation-clean. Regenerate to confirm `library/sa.lua` and `.luarc.json` globals are correct.
Grep: no ` se\.` remains in `.lua` files or the Lua def blocks.
