# Phase 1 — `saffron-script` crate: Luau VM, sandbox, instruction/memory budget

**Status:** COMPLETED

**Depends on:** 00-foundations:phase-1-workspace-scaffold, 00-foundations:phase-2-core-crate, 03-ecs-and-scene

## Goal

Stand up the `saffron-script` crate and the VM primitive: create an `mlua::Lua` with the `luau`
feature, enable Luau's sandbox, install an instruction-and-memory budget, load+run a chunk, and map any
load/runtime error to a typed `Error` carrying a Luau traceback. This is the `newScriptVm` + `runString`
+ the sandbox/budget layer, with no `sa.*` bindings yet (those land in phase 2).

## Why this shape (NO LEGACY)

- **`mlua` (luau) replaces `lua55` + LuaBridge3.** The locked VM decision (pre-plan §0, feasibility
  4.4). `mlua` confines all `lua_State` unsafety internally, so the crate is `#![deny(unsafe_code)]` —
  the C++ raw-stack discipline (`lua_pushcfunction`/`lua_pcall`/`lua_pop` balance, `tracebackHandler` +
  `luaL_traceback`, `finishRun`/`popError`) is **deleted**, not ported: `Function::call` returns
  `Result<_, mlua::Error>` and Luau errors already carry a traceback.
- **Luau's built-in sandbox replaces the hand-curated library set.** C++ called
  `luaL_openselectedlibs(L, GLIBK | COLIBK | STRLIBK | MATHLIBK | TABLIBK | UTF8LIBK, 0)` to omit
  `io`/`os`/`debug`/`package` (`script.cppm:282`). Luau ships `Lua::sandbox(true)` as a first-class mode
  (no `io`/`os`/`debug`/`package`, frozen base tables, deterministic). One call replaces the bitmask;
  the sandbox-probe self-test (`assert(io == nil and os == nil and debug == nil and package == nil)`,
  `script.cppm:326`) becomes a `#[test]`, not a runtime `runScriptSelfTest` (no in-engine self-tests).
- **The instruction/memory budget is the upgrade Rust gets.** The C++ VM had no budget. `mlua` exposes
  `Lua::set_interrupt` (Luau's interrupt hook, called periodically) + `Lua::set_memory_limit`. Decision:
  install an interrupt that trips when a per-call instruction budget is exceeded and returns
  `VmYield::Yield`-or-error so a runaway gameplay loop aborts the *tick* (mapped to a `ScriptRunError`
  on the same pause-on-error path), never hangs the host frame. The budget is reset at the start of each
  scripted call (start/tick/contact/message), not globally.
- **A typed crate `Error`, not `Result<T, String>`.** Per the error model, `saffron-script` defines its
  own `thiserror` `Error` (a `Load`/`Runtime` variant carrying the Luau message+traceback string, a
  `Budget` variant) and `Result<T>`. A `mlua::Error` composes in via `#[from]`. This replaces the C++
  `Result<void>` + `Err(traceback)` (`script.cppm:54`,244).
- **VM is owned, `!Send`, single-thread.** `mlua::Lua` is `!Send`; the host owns it by value (per
  08-host-and-viewport: host state is single-thread-owned, no `Arc`). No interior mutability for the VM.

## Grounding (real files / symbols)

- `engine-old/source/saffron/script/script.cppm`: `ScriptVm` (move-only, `lua_close` in dtor, 36–46),
  `newScriptVm` (the library-set bitmask + namespace bring-up, 275–292), `runString` +
  `tracebackHandler` + `finishRun` (294–301, 231–268), `runScriptSelfTest` (the good/broken/sandbox
  probe, 303–332 — the test oracle).
- mlua `luau` feature: `Lua::new`, `Lua::sandbox`, `Lua::set_interrupt`, `Lua::set_memory_limit`,
  `Lua::load(...).set_name(...).exec()`, `mlua::Error` (carries the traceback).

## Acceptance gate

- `cargo build -p saffron-script` and `cargo build --workspace` succeed; `#![deny(unsafe_code)]` holds;
  clippy + fmt clean.
- `#[test]` (the `runScriptSelfTest` oracle, as units): a good chunk (`assert(1 + 1 == 2)`) runs Ok; a
  broken chunk (`error('deliberate')`) returns `Err` whose message contains `deliberate` **and** a
  traceback; a sandbox probe asserts `io`/`os`/`debug`/`package` are absent.
- `#[test]`: a chunk that loops without yielding (a tight `while true do end`) trips the instruction
  budget and returns the `Budget` error variant within the budget, rather than hanging; a chunk that
  allocates past the memory limit returns an error.
- `#[test]`: a load error (syntax error) maps to the `Load` variant; a runtime error to `Runtime`.
