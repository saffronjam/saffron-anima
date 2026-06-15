# Phase 1 — Binary & target identity (atomic, repo-wide)

**Status:** NOT STARTED

Rename the host executable and its library/alias/project from `SaffronEngine` to `SaffronAnima`, and
update **every** reference to the binary's name, path, and process identity in one change so nothing
ever spawns a stale path. This is the invariant the rest of the rebrand builds on.

## Build system (CMake)

`CMakeLists.txt`
- `project(SaffronEngine …)` → `project(SaffronAnima …)` (description text unchanged).

`engine/CMakeLists.txt`
- `add_library(SaffronEngineLib STATIC)` → `add_library(SaffronAnimaLib STATIC)` and every reference
  to `SaffronEngineLib` (the module-source lists, `target_link_libraries`, `set_target_properties …
  CXX_MODULE_STD ON`) → `SaffronAnimaLib`.
- `add_library(Saffron::Engine ALIAS SaffronEngineLib)` → `add_library(Saffron::Anima ALIAS SaffronAnimaLib)`.
- `add_executable(SaffronEngine)` → `add_executable(SaffronAnima)`; its `target_link_libraries(… Saffron::Anima)`,
  `set_target_properties(SaffronAnima … CXX_MODULE_STD ON)`, the `saffron_compile_shaders(SaffronAnima …)`
  call (the **function name stays** `saffron_compile_shaders`), and the asset-copy
  `add_custom_command(TARGET SaffronAnima POST_BUILD …)`.

Keep unchanged: `saffron_third_party`, `SAFFRON_RUNTIME_DIR`, `CMakePresets.json` (generic
`debug`/`release`), `cmake/CompileShaders.cmake` (`saffron_compile_shaders` is a family-brand helper),
`cmake/Slang.cmake`, `cmake/Dependencies.cmake`.

## Every binary-path / spawn-site reference

- `Makefile`: `ENGINE_BIN := $(BUILD_DIR)/bin/SaffronEngine` → `…/SaffronAnima`; the `SAFFRON_ENGINE_BIN`
  references → `SAFFRON_ANIMA_BIN`; comments naming the binary.
- `editor/src-tauri/src/lib.rs`: `std::env::var("SAFFRON_ENGINE_BIN")` → `"SAFFRON_ANIMA_BIN"`; the
  default path fallback `build/debug/bin/SaffronEngine` → `…/SaffronAnima`.
- `tests/e2e/harness.ts:18`: `process.env.SAFFRON_ENGINE_BIN ?? join(REPO,"build","debug","bin","SaffronEngine")`
  → `SAFFRON_ANIMA_BIN` + `"SaffronAnima"`; the comments at `harness.ts:1,6` and `tests/e2e/AGENTS.md:3`.
- `tools/ci/check.sh`: the `"$REPO/build/debug/bin/SaffronEngine"` smoke invocation → `SaffronAnima`.
- `tools/check-projects/check.sh`: the binary path it boots → `SaffronAnima`.

## Process / trace identity (`"SaffronEngine"` as a name string)

Group these with the binary rename so the editor profiler test stays green:
- `engine/source/saffron/control/control_commands_render.cpp`: the GPU-trace capture payload
  `{ "pid", "SaffronEngine" }` and `{ "name", "SaffronEngine" }` → `"SaffronAnima"` (5 sites).
- `editor/src/lib/chromeTrace.ts`: `pid: "SaffronEngine"` → `"SaffronAnima"`.
- `editor/src/lib/profilerTransforms.test.ts`: the `"SaffronEngine"` expectations → `"SaffronAnima"`.

## Deliberately deferred (not this phase)

- The window title `"Saffron Engine"` in `engine/source/main.cpp` → phase 3 (with the C++ string-literal pass).
- README badges, Hugo `baseURL`, `.github/workflows/ci.yml` comments → phase 7.
- Editor user-visible app name / bundle id → phase 6.

## Verify

```sh
toolbox run -c saffron-build bash -lc '
  cmake --preset debug
  cmake --build build/debug -j8
  test -x build/debug/bin/SaffronAnima && ! test -e build/debug/bin/SaffronEngine
'
```
Then a headless smoke (weston per AGENTS.md): `SAFFRON_EXIT_AFTER_FRAMES=1 build/debug/bin/SaffronAnima`
exits 0. Run `make prepare-for-commit`. Grep check: no `SaffronEngine` remains as a **binary/target/path**
(prose mentions are handled later); `Saffron::Engine` and `SaffronEngineLib` return zero hits.
