+++
title = 'Lua runtime'
weight = 1
+++

# Lua runtime

The engine embeds Lua 5.5 so entities can run gameplay logic without recompiling C++. All
knowledge of Lua lives in one module, `Saffron.Script`; nothing else imports the Lua headers or
the LuaBridge3 binding layer, so the rest of the engine stays free of VM concerns and the binding
library could be swapped without touching callers.

## How it works

A `ScriptVm` owns one `lua_State` and closes it in its destructor — the same move-only RAII shape
the rendering wrappers use. `newScriptVm` opens a deliberately minimal library set
(base, coroutine, string, math, table, utf8) and withholds `io`, `os`, `debug`, and `package`,
so a script cannot touch the filesystem or load arbitrary native code.

Lua is compiled as C, which means its error model is `setjmp`/`longjmp` rather than C++
exceptions. No Lua error is allowed to unwind through C++ frames: every chunk runs under
`lua_pcall` with a message handler that appends a `luaL_traceback`, and a failure surfaces as an
`Err` carrying the full traceback string. This matches the engine-wide errors-as-values rule —
a broken script is a logged, inspectable failure, never a crash.

Lua 5.5 and LuaBridge3 are vendored through CMake (CMake's bundled `FindLua` predates 5.5). The
Lua core and stdlib compile into one static C library; LuaBridge3 is a header-only C++ layer over
the C API used to register engine functions, starting with a `se.log` that routes into the engine
log.

> [!NOTE]
> This page covers the runtime layer only. The per-entity `ScriptComponent`, the `self.entity`
> API surface, and script-declared editable fields are later phases of `plans/scripting-mvp`.

## In the code

| What | File | Symbols |
|---|---|---|
| VM ownership and lifecycle | `script.cppm` | `ScriptVm`, `newScriptVm` |
| Running chunks, errors → `Result` | `script.cppm` | `runString` |
| Spike self-check | `script.cppm` | `runScriptSelfTest` |
| Vendored Lua 5.5 + LuaBridge3 | `Dependencies.cmake` | `lua_static`, `LuaBridge` |

## Related

- [Error handling](../../core-and-conventions/error-handling/) — the `Result<T>` idiom script errors convert into
- [Module DAG](../../architecture-and-conventions/) — where `Saffron.Script` sits and why only the Host may import it
