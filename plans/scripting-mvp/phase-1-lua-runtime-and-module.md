# Phase 1 — Lua runtime + `Saffron.Script` module (the spike)

**Status:** COMPLETED

## Goal

Stand up the Lua 5.5 runtime and a new, **isolated** `Saffron.Script` module — and prove the
Clang 21 / libc++ / C++26 / named-modules toolchain compiles LuaBridge3. Nothing in the engine imports
this module yet, so it builds in isolation and the first green `make engine` *is* the verification
(see the README caveat). Until this phase is green, do not start Phase 2.

## What exists to build on

- Third-party deps are vendored static via FetchContent in `cmake/Dependencies.cmake`, aggregated into
  the `saffron_third_party` INTERFACE target (`Dependencies.cmake:66-79`). The non-git URL+hash download
  precedent is `cmake/Slang.cmake:12-17`.
- Engine modules are one flat static lib: interface units listed in the `CXX_MODULES` file set
  (`engine/CMakeLists.txt:4-26`), impl `.cpp` units as `PRIVATE` sources (`:30-48`); `cxx_std_26` +
  `CXX_MODULE_STD ON` are already set on the target (`:50-51`).
- The "pure-C header in the global module fragment" pattern is `Saffron.Window` (`window.cppm:1-8`); the
  "heavy C++ header, no `import std`" pattern is `Saffron.Json`/`Saffron.Scene`
  (`json.cppm:1-13`, `scene.cppm:1-23`). **LuaBridge3 is a heavy C++ header, so this module follows the
  latter** even though Lua itself is C.
- The RAII-wrapper + `Result<T>` error idiom: move-only structs that release in their destructor, and
  `std::expected<T,std::string>` returned from fallible calls (`core.cppm`; mirrored across the
  rendering wrappers).

## Work

### 1. Vendor Lua 5.5.0 (built from source — Lua ships no CMake)

In `cmake/Dependencies.cmake`, alongside the other from-source static libs and **before** the
`saffron_third_party` aggregate (`:66`):

```cmake
# --- Lua 5.5.0 (built from source; ships no CMake) ----------------------------
# Lua's tarball provides only a Makefile, so we populate the sources and compile
# the core + stdlib into one static lib. lua.c / luac.c are the CLI front-ends
# (each has main()) and are excluded.
FetchContent_Declare(lua
    URL https://www.lua.org/ftp/lua-5.5.0.tar.gz
    URL_HASH SHA256=57ccc32bbbd005cab75bcc52444052535af691789dba2b9016d5c50640d68b3d)
FetchContent_MakeAvailable(lua)        # CMake-less project: populates only

file(GLOB lua_sources CONFIGURE_DEPENDS ${lua_SOURCE_DIR}/src/*.c)
list(REMOVE_ITEM lua_sources
    ${lua_SOURCE_DIR}/src/lua.c        # standalone interpreter main()
    ${lua_SOURCE_DIR}/src/luac.c)      # bytecode compiler main()

add_library(lua_static STATIC ${lua_sources})
target_include_directories(lua_static PUBLIC ${lua_SOURCE_DIR}/src)
target_compile_definitions(lua_static PRIVATE LUA_USE_POSIX)   # POSIX, no dlopen/readline pull-in
set_target_properties(lua_static PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
```

- The SHA256 above is the **verified** hash of the official `lua-5.5.0.tar.gz` (`sha256sum` of the
  lua.org download, cross-checked against the Arch packaging + the lua-l release announcement); the
  URL+hash discipline mirrors `Slang.cmake`. Prefer `LUA_USE_POSIX` over `LUA_USE_LINUX` to avoid the
  `dl`/`readline` linkage that `LUA_USE_LINUX` implies (`readline` is only referenced by the excluded
  `lua.c`).
- Lua compiles as **C** (so its error model stays `setjmp`/`longjmp`, never C++ exceptions — see §4).

### 2. Vendor LuaBridge3 (header-only)

```cmake
# --- LuaBridge3 (header-only C++ <-> Lua bindings; supports Lua 5.5.0) ---------
FetchContent_Declare(LuaBridge3
    GIT_REPOSITORY https://github.com/kunitoki/LuaBridge3.git
    GIT_TAG 3.0-rc12 GIT_SHALLOW ON)        # pin a specific rc, do not track master
FetchContent_MakeAvailable(LuaBridge3)
```

LuaBridge3 exposes a `LuaBridge` INTERFACE target (header-only). If its CMake proves awkward under the
pinned CMake 3.31, the fallback is to vendor the single amalgamated header
(`Distribution/LuaBridge/LuaBridge.h`) into the repo and add it as an INTERFACE include dir — it ships
exactly for this.

Append both to the aggregate (`Dependencies.cmake:66-79`):

```cmake
target_link_libraries(saffron_third_party INTERFACE
    SDL3::SDL3
    ...
    lua_static
    LuaBridge)        # or the vendored-header INTERFACE target
```

### 3. Create the `Saffron.Script` module

New file `engine/source/saffron/script/script.cppm`:

```cpp
module;

#include <LuaBridge/LuaBridge.h>      // heavy C++ template header -> global module fragment
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <expected>                   // every std header used, classic include
#include <string>
#include <string_view>
#include <utility>

export module Saffron.Script;

import Saffron.Core;                  // Result / Err / TimeSpan  -- NO `import std`

export namespace se
{
    /// Owns one Lua VM. Move-only; closes the lua_State in the destructor.
    struct ScriptVm
    {
        lua_State* L = nullptr;
        ScriptVm() = default;
        ScriptVm(ScriptVm&&) noexcept;
        ScriptVm& operator=(ScriptVm&&) noexcept;
        ScriptVm(const ScriptVm&) = delete;
        ScriptVm& operator=(const ScriptVm&) = delete;
        ~ScriptVm();
    };

    /// Create a VM with a minimal, sandboxed library set (base/string/math/table/utf8).
    auto newScriptVm() -> Result<ScriptVm>;

    /// Load + run a Lua source file. Lua syntax/runtime/file errors become Err(traceback).
    auto runFile(ScriptVm& vm, std::string_view path) -> Result<void>;

    /// Load + run a Lua source string (used by the self-test / future eval command).
    auto runString(ScriptVm& vm, std::string_view src, std::string_view chunkName) -> Result<void>;
}
```

Implementation notes (in the same `.cppm`, or a `script_runtime.cpp` reopening the module if it grows):

- **Create:** `luaL_newstate()` (avoids the Lua 5.5 `lua_newstate` `seed` third-parameter break; it also
  installs the default stderr panic/warning handlers). On `nullptr` → `Err("out of memory")`.
- **Libraries:** open only a minimal set with `luaL_openselectedlibs(L, mask, 0)` where `mask =
  LUA_GLIBK | LUA_STRLIBK | LUA_MATHLIBK | LUA_TABLIBK | LUA_COLIBK | LUA_UTF8LIBK` — deliberately
  excluding `io`/`os`/`debug`/`package` (`luaL_openlibs` is the all-on macro we are *not* using).
- **The traceback message handler:** a `static int msgh(lua_State*)` that calls
  `luaL_traceback(L, L, lua_tostring(L, 1), 1)` and returns 1. Push it before the chunk, pass its stack
  index as the `errfunc` of `lua_pcall`. On nonzero return, read the traceback via `lua_tostring`, build
  `Err(...)`, pop, and **leave the stack balanced**.
- **`runFile`:** `luaL_loadfilex(L, path, nullptr)` then `lua_pcall(L, 0, 0, msghIdx)`; map any nonzero
  status to `Err(traceback)` (file-not-found is `LUA_ERRFILE` — return a clear `Err`).
- **The spike binding:** register a trivial `se.log(message)` with LuaBridge3 to exercise the template
  path end-to-end:
  ```cpp
  luabridge::getGlobalNamespace(L)
      .beginNamespace("se")
          .addFunction("log", +[](const char* msg) { /* route to the engine log */ })
      .endNamespace();
  ```

### 4. Register the module + place it in the DAG

- Add `source/saffron/script/script.cppm` to the `CXX_MODULES` FILES list in
  `engine/CMakeLists.txt:9-26`. `cxx_std_26` + `CXX_MODULE_STD ON` already cover it. Place it after
  `scene` (its DAG dependency); CMake derives the real build order from the `import` statements, but keep
  the listing dependency-consistent.
- Add the `Script → {Core, Scene}` row to the module DAG table in `AGENTS.md` (Scene is not yet imported
  in this phase — the edge lands in Phase 3 — but record the intended shape now so the table stays the
  source of truth). For Phase 1 the only real edge is `Script → Core`.

### 5. Minimal self-test

Add an exported `runScriptSelfTest() -> Result<void>` mirroring the scene module's self-test style
(`scene.cppm:1069`): create a VM, `runString(vm, "se.log('script-ok'); x = 1 + 1", "selftest")`, assert
`Result` is ok; then run a deliberately broken chunk and assert the `Err` carries a traceback. This is
optional but makes the spike self-evidencing without any Host wiring.

## Decisions / notes specific to this phase

- This module **does not** depend on SceneEdit/Rendering/Control and **nothing imports it yet** — that is
  intentional. It must build, link, and (optionally) self-test entirely on its own.
- `import std` is **forbidden** in this TU (LuaBridge3 is a heavy C++ header). Use classic `#include` for
  every std header, exactly like `scene.cppm`.
- The Lua C headers need the `extern "C"` block — the FetchContent tarball ships `lua.h`/`lauxlib.h`/
  `lualib.h` (no C++ guard), not `lua.hpp`.
- **Never let a Lua error unwind through C++ frames.** Lua is built as C (`longjmp`); every entry point
  is wrapped in `lua_pcall` and converted to `Result` at the boundary — consistent with the engine's
  no-exceptions rule.

## Gate / done

- `make engine` is clean (**this is the toolchain spike** — if LuaBridge3 will not compile here, fall
  back to the raw Lua 5.5 C API behind the same `se::` facade; the rest of the plan is unchanged).
- `make prepare-for-commit` (format + lint) raises no new warnings.
- AGENTS.md module-DAG table updated; a stub `docs/content/.../scripting.md` concept page created (lead +
  an empty `## In the code` table to be filled in later phases), with its hub row added.
- Run the build in a **private `build/<name>` dir** if another agent is using `build/debug` (adding to
  CMake re-runs configure — AGENTS.md "Concurrent builds").

## Risks

- No published matrix confirms LuaBridge3 on Clang 21 + libc++ + C++26 + modules; this phase exists to
  find out early. Keep the binding surface behind `se::` so the fallback to the raw C API is a
  one-package change.
- CMake 3.31's bundled `FindLua` predates 5.5 — that is *why* we vendor Lua rather than `find_package`.
- Pin LuaBridge3 to a specific `3.0-rcN` tag/commit for reproducibility; it ships only release
  candidates (no final 3.0 object) but is actively maintained.
