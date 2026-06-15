# Phase 3 — C++ namespace `se` → `sa`

**Status:** COMPLETED

Rename the engine's short namespace across all **hand-written** C++ (the generated files are already
correct after phase 2). The namespace is always qualified, so this is mechanical and low-risk with the
anchored patterns below.

## The two safe patterns

- `namespace sa` → `namespace sa` — 26 declaration sites across `engine/source/saffron/` (rendering/*,
  control/*, scene, animation, assets, sceneedit/*, physics, script) and the tools.
- `sa::` → `sa::` — ~43 qualified uses (e.g. `sa::runHost`, `sa::run`, `sa::AppConfig`, `sa::valid`,
  `sa::setParent`).

There is **no** `using namespace sa` and no nested `sa::detail`, so these two anchored replacements
cover the whole engine without touching unrelated tokens. Do it module-by-module (rendering and
control are the largest) and audit each `git diff` for stray in-word hits before building.

Do **not** edit the generated files by hand — `control_dto_serde.generated.cpp`,
`scene_component_serde.generated.cpp`, `script_component_defs.generated.hpp` already carry `sa` from
phase 2; a regen must keep them byte-identical.

## Brand string literals (same pass)

- `engine/source/main.cpp`: `sa::runHost("Saffron Anima", 1600, 900)` → the call becomes
  `sa::runHost("Saffron Anima", 1600, 900)` (both the namespace and the window-title string).
- Confirm the `runHost` / `run` / `AppConfig` declarations (in `host/host.cppm`, `app/app.cppm`) move
  to `sa` along with their definitions; these are the public entry points `main.cpp` calls.

The `"SaffronAnima"` process-name literals were already handled in phase 1.

## Keep unchanged

- Module names `Saffron.<Area>` and all `import Saffron.*` / `export module Saffron.*` lines.
- The `engine/source/saffron/` directory and include paths.
- All `SAFFRON_*` env-var lookups except the already-renamed `SAFFRON_ANIMA_BIN`.

## Verify

`make engine` builds clean (this is the first full build after phases 2+3 together). `make prepare-for-commit`
is green. A re-run of the control generator yields an empty diff (proves hand and generated namespaces
agree). Grep: `namespace sa\b` and `\bse::` return zero hits in `engine/source` and `tools` (excluding
the `se`→`sa` CLI handled in phase 5).
