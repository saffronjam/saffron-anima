# Rebrand: Saffron Engine → Saffron Anima

**Status:** NOT STARTED

Rename the product from **Saffron Engine** to **Saffron Anima**. "Saffron" is the umbrella brand
(sibling to *Saffron Hive*) and stays; only the product half — "Engine" — becomes "Anima", and the
C++/Lua short namespace `se` (S·affron E·ngine) becomes `sa` (S·affron A·nima). This is a clean-slate
codebase with the NO-LEGACY rule, so there is no compat shim, no dual-named path, and no data
migration: every renamed thing is renamed in place and every caller updated in the same change.

## The identity model (decided)

| Axis | Decision |
|---|---|
| Family brand | **Saffron** — retained everywhere it stands alone |
| Product name | **Saffron Engine → Saffron Anima** |
| Short namespace | **`se` → `sa`** (C++ `sa::`, Lua `sa.`) |
| C++ module names + source dir | **kept** (`Saffron.<Area>`, `engine/source/saffron/`) |
| `SAFFRON_*` env prefix | **kept** (only `SAFFRON_ENGINE_BIN` → `SAFFRON_ANIMA_BIN`) |
| `saffron-build` toolbox + asset extensions | **kept** |
| GitHub repo + docs URL | **renamed** (`saffron-engine` → `saffron-anima`) |

The guiding test: does the token contain "engine" or is it the `se` short-name? Then it changes.
Is it "saffron" standing for the family brand? Then it stays.

## Canonical rename table

| Concept | Old | New |
|---|---|---|
| Product name (prose) | `Saffron Engine` / `SaffronEngine` | `Saffron Anima` / `SaffronAnima` |
| C++ namespace | `se::` | `sa::` |
| Lua global table | `se.` | `sa.` |
| Host exe target + binary | `SaffronEngine` → `build/debug/bin/SaffronEngine` | `SaffronAnima` → `build/debug/bin/SaffronAnima` |
| Static lib target | `SaffronEngineLib` | `SaffronAnimaLib` |
| Lib alias | `Saffron::Engine` | `Saffron::Anima` |
| CMake `project()` | `SaffronEngine` | `SaffronAnima` |
| Window title | `"Saffron Engine"` | `"Saffron Anima"` |
| Trace/process name (pid) | `"SaffronEngine"` | `"SaffronAnima"` |
| Engine-binary env var | `SAFFRON_ENGINE_BIN` | `SAFFRON_ANIMA_BIN` |
| Generated TS protocol file | `editor/src/protocol/se-types.ts` | `editor/src/protocol/sa-types.ts` |
| Lua type defs | `---@class se.*`, `SeLuaDefs`, `library/se.lua` | `---@class sa.*`, `SaLuaDefs`, `library/sa.lua` |
| Control CLI | `se` (`tools/se/`) | `sa` (`tools/sa/`) |
| Editor app (user-visible) | `Saffron Editor` | `Saffron Anima` |
| Tauri bundle id | `dev.saffron.engine.editor` | `dev.saffron.anima.editor` |
| OpenRPC doc title | `"Saffron control DTOs"` | `"Saffron Anima control DTOs"` |
| GitHub repo / Hugo `baseURL` | `saffron-engine` | `saffron-anima` |

## Retained — do NOT rename (Saffron = family brand)

These contain "saffron" but **not** "engine" and are not the `se` short-name, so they stay:

- C++ module names `Saffron.<Area>` and partitions (`Saffron.Rendering:Types`, …) — 22 decls, ~92 imports.
- Source directory `engine/source/saffron/` and every include/path under it.
- Toolbox container `saffron-build`; the self-hosted CI runner label `saffron-build`.
- Env prefix `SAFFRON_*` for all vars **except** `SAFFRON_ENGINE_BIN` (e.g. `SAFFRON_CONTROL_SOCK`,
  `SAFFRON_PROJECT`, `SAFFRON_EXIT_AFTER_FRAMES`, `SAFFRON_VIEWPORT_SHM_*`, `SAFFRON_WEBVIEW_HW`,
  `SAFFRON_EDITOR_NATIVE_VIEWPORT`, …).
- CMake helpers/targets/vars: `saffron_compile_shaders`, `saffron_third_party`, `SAFFRON_RUNTIME_DIR`,
  `SAFFRON_JOLT_*`, `SAFFRON_SLANG_VERSION`.
- Asset extensions `.smesh` / `.smat` / `.smodel` and any other `.s*` formats.
- Editor-internal "saffron" tokens: MIME `application/x-saffron-entity`; React-Flow node type
  `"saffron"` and `SaffronNode`/`SaffronNodeData`; `localStorage` `saffron.*` keys; sockets
  `saffron-editor-*.sock`; shm `/saffron-viewport-*`; `saffron-backdrop`; the `[saffron]` log prefix;
  `saffron-profile.json`; `VITE_SAFFRON_DEV_MODE`; the `"Saffron Project"` file-dialog label.
- Package/crate names `saffron-editor`, `saffron_editor_lib`, and the `@saffron/protocol` TS package name.
- Default control socket name `saffron-control.sock` and the `SAFFRON_CONTROL_SOCK` override.

## Why the `se` → `sa` find/replace is safe

The investigation found the namespace is always accessed as `se::` (26 `namespace se` decls, ~43
`se::` uses; **no** `using namespace se`, no nested `se::detail`). So the safe, targeted patterns are
`namespace se` → `namespace sa` and `se::` → `sa::`. The Lua side is `se.` (table access) and `"se: "`
(error-prefix strings) — those are real string/identifier contexts, handled deliberately in phase 4,
not by a blind global replace. No unrelated token in the tree is corrupted by these anchored patterns.

## Phasing (dependency-ordered)

| # | File | Scope |
|---|---|---|
| 1 | `phase-1-binary-identity.md` | atomic, repo-wide rename of the exe/lib/targets + binary path + process/trace name |
| 2 | `phase-2-control-generator.md` | `gen.ts` emitters → `sa`/`sa-types.ts`/`sa.*`, then regenerate all artifacts |
| 3 | `phase-3-cpp-namespace.md` | `se` → `sa` across hand-written engine + tools C++; brand string literals |
| 4 | `phase-4-lua-scripting.md` | Lua global `se.` → `sa.`, the def file, `.luarc.json`, runtime prelude, example scaffolds |
| 5 | `phase-5-control-cli.md` | `tools/se/` → `tools/sa/`: binary `se` → `sa`, help text, sockets, references |
| 6 | `phase-6-editor.md` | editor user-visible name → "Saffron Anima", bundle id, `sa-types` import |
| 7 | `phase-7-docs-meta-ci.md` | docs prose + `baseURL`, README, AGENTS/CONVENTIONS, CI comments, repo URL + final full gate |

Order rationale: phase 1 makes the binary's new name the invariant everything else can rely on. Phase 2
fixes the generator **before** the hand-written namespace pass so regenerated C++/TS/Lua already carry
`sa` and a later regen never reverts phase 3/4. Phases 3–6 are the bulk of the code rename. Phase 7 is
prose + the cross-cutting `make check` / `make e2e` gate.

## Verification

- **Per phase** (the milestone gate): `make engine` then `make prepare-for-commit` (format + lint),
  green, plus the phase's own smoke check. Each phase ends on a buildable tree.
- **Whole rebrand done when:** `make check` and `make e2e` are green; the editor builds
  (`cd editor && bun run check && bun run build`); a headless run of `build/debug/bin/SaffronAnima`
  exits clean with a validation-clean log; a Lua script using the `sa.*` API runs; and a tree-wide
  grep for the renamed tokens (`SaffronEngine`, `Saffron::Engine`, `se::`, ` se\.` in Lua/defs,
  `se-types`, `SAFFRON_ENGINE_BIN`) returns only intentional, retained matches.

## Risks

- **Two-letter token.** `se` is short; only the anchored patterns in "Why … is safe" above are allowed.
  Never run an unanchored `se`→`sa`. Audit each batch's `git diff` for stray hits inside words.
- **Generator vs. hand edits.** Generated files (`*.generated.{cpp,hpp}`, `se-types.ts`,
  `openrpc.generated.json`) must be changed in `gen.ts` and regenerated, never hand-edited — phase 2
  precedes phase 3 for exactly this reason. After phase 3, a regen must produce a byte-identical diff.
- **Binary-path consistency.** The exe rename (phase 1) breaks every spawn site (Makefile, e2e harness,
  CI smoke, editor default) at once — phase 1 updates all of them together so no later phase boots a
  stale path.
- **Concurrent work.** This touches a wide surface; land it in focused, phase-scoped commits and
  reconcile against any in-flight branches rather than fixing other agents' work.

## External / manual follow-ups (outside the tree)

- Rename the GitHub repository `saffron-engine` → `saffron-anima` and update the local `git remote`
  (a user action; do not run git writes automatically). The in-repo URLs are updated in phase 7.
- GitHub Pages / Hugo deploy target for the new `…/saffron-anima/` base path.
- The `saffron-build` toolbox container is intentionally **kept**, so no container work is needed.
