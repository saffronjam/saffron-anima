+++
title = 'Lua runtime'
weight = 1
+++

# Lua runtime

The engine embeds a Luau VM so entities can run gameplay logic without rebuilding the engine. All
knowledge of the VM lives in one crate, `saffron-script`; nothing else links `mlua` or holds a VM
handle, so the rest of the engine stays free of VM concerns and the host sees only a small surface
of plain types and `Result`-returning functions.

## How it works

A `ScriptVm` owns one `mlua::Lua` and frees it on `Drop`. It is `!Send` and single-thread-owned by
value, matching the host's ownership model. `ScriptVm::new` loads a deliberately minimal standard
library (base, coroutine, string, math, table, utf8) and withholds `io`, `os`, `debug`, and
`package` — they read as `nil` in script — so a chunk cannot touch the filesystem or load native
code. The VM then calls Luau's `sandbox(true)`, which freezes the base tables and makes execution
deterministic.

Two budgets bound a runaway script so it can never hang the frame. An interrupt callback counts the
cycles a single scripted call takes and aborts it past `DEFAULT_INSTRUCTION_BUDGET`, and a memory
ceiling (`DEFAULT_MEMORY_LIMIT`) aborts a chunk that allocates without bound. The per-call counter
resets before every run, so one heavy handler cannot starve the next.

Errors are values, never crashes. `run_string` loads and calls a chunk and maps any failure to the
crate's `Error` enum: a syntax error becomes `Error::Load`, a raised or faulting runtime error
becomes `Error::Runtime` with the Luau stack traceback already in the message, and a budget or
memory abort becomes `Error::Budget`. `mlua` surfaces a traceback on every Lua error, so a broken
script is a logged, inspectable failure that the play loop can pause on.

Because the whole crate confines its `mlua` unsafety internally, `saffron-script` builds under
`#![deny(unsafe_code)]`.

> [!NOTE]
> This page covers the VM layer only. The per-entity `Script` component, the `self.entity` API
> surface, and script-declared editable fields live on the sibling pages.

## In the code

| What | File | Symbols |
|---|---|---|
| VM ownership, sandbox, budgets | `script/src/vm.rs` | `ScriptVm`, `DEFAULT_INSTRUCTION_BUDGET`, `DEFAULT_MEMORY_LIMIT` |
| Running chunks, errors → `Result` | `script/src/vm.rs` | `ScriptVm::run_string`, `map_lua_error` |
| The typed error and `Result` alias | `script/src/error.rs` | `Error`, `Result` |
| Crate surface and module map | `script/src/lib.rs` | `ScriptVm`, `ScriptHost`, `BINDINGS` |

## Related

- [Error handling](../../core-and-conventions/error-handling/) — the per-crate `Error` + `Result<T>` idiom script errors convert into
- [Architecture overview](../../architecture-and-conventions/) — where `saffron-script` sits and why only the host drives it
