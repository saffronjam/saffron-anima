# Phase 5 — The control CLI `se` → `sa`

**Status:** COMPLETED

The control CLI is named `se` (S·affron E·ngine control); it becomes `sa`. Rename the directory, the
binary target, the help/usage text, and the few CLI-owned socket/temp names, then fix every reference.

## Directory & target

- Move `tools/sa/` → `tools/sa/` (source, `CMakeLists.txt`, `AGENTS.md`, the `cmd/sa` wrapper → `cmd/sa`).
- `tools/sa/CMakeLists.txt`: `add_executable(se …)` → `add_executable(sa …)` and the
  `target_compile_options(se …)` / `target_link_options(se …)` references → `sa`.
- Wire the new path into the top-level CMake (wherever `tools/sa` is `add_subdirectory`'d).

## CLI strings — `tools/sa/source/main.cpp`

- The arg-parser program/help: `args::ArgumentParser("se — SaffronAnima control CLI")` →
  `"sa — Saffron Anima control CLI"`; `parser.Prog("se")` → `Prog("sa")`.
- User-facing usage examples in messages: `"se profiler.capture-stop"`, `"se profiler.capture-start"`
  → `"sa …"`.
- Error prefixes `"se: "` (4 sites) → `"sa: "`.
- CLI-owned temp file `saffron-profile.json` → keep (Saffron family, no "engine"); but if you prefer
  it match the CLI name, it is internal-only. Default decision: **keep** `saffron-profile.json`.

## Sockets

- The default control socket `saffron-control.sock` and the `SAFFRON_CONTROL_SOCK` override **stay**
  (Saffron family, shared with the engine — both sides must agree, and the engine keeps `SAFFRON_*`).
- CI/test socket names that use the `se` short-name: `/tmp/se-ci.sock` → `/tmp/sa-ci.sock`
  (in `tools/ci/check.sh`). Project/contract test sockets named `saffron-*` stay.

## References to update

- `Makefile`, `AGENTS.md`, `docs/`, `tools/ci/check.sh`, `tools/check-control-schema`,
  `tools/check-projects`: every invocation of the `sa` CLI / `tools/sa` path → `sa` / `tools/sa`.
  (Prose-heavy doc/agent updates are batched in phase 7; this phase covers the build-wiring and the
  CLI's own files.)
- `.clang-tidy` `HeaderFilterRegex: '(engine/source|tools/sa)'` → `'(engine/source|tools/sa)'`.

## Verify

Build the CLI in the toolbox; run a round-trip against a headless engine over the control socket
(e.g. `sa scene.list` or `sa profiler.capture-start`/`-stop`) and confirm a valid JSON response.
Grep: no `tools/sa` path or `\bse\b`-as-CLI-name remains.
