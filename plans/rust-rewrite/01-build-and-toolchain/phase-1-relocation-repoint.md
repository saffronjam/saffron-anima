# Phase 1 — Repoint the C++ build references to `engine-old/`

**Status:** COMPLETED
**Depends on:** —

## Goal

Make the existing C++ `SaffronAnima` build and its tooling buildable again after the `engine/` →
`engine-old/` relocation, by repointing every build/codegen/CI reference that still names `engine/` at
`engine-old/`. The C++ engine must stay green for the entire rewrite, because the cutover strategy
(greenfield parallel binary) keeps the editor spawning the C++ host via `SAFFRON_ANIMA_BIN` until the
Rust host passes the full parity gate.

This phase touches **no Rust** and writes **no engine code** — it is the mechanical relocation fix that
keeps the reference build alive. It is sequenced first so the rest of the rewrite proceeds against a
green C++ baseline.

## Why this shape (NO LEGACY)

The relocation already happened (`engine-old/source/saffron/...` is the C++ tree; `engine/` is now the
Rust workspace root). Leaving the old references dangling would silently break `make engine`, the
codegen drift guard, and the contract tests — and a broken reference build hides regressions during the
rewrite. NO LEGACY does not mean "delete the C++ build now": the C++ engine is the *running* binary
until cutover, so it is kept buildable on purpose. The legacy gets deleted in `14-migration-and-cutover`
when `engine-old/` is removed, not before. This phase is the minimal repoint, nothing more — it does
not "improve" the C++ build or start the Rust one.

The one new artifact this phase justifies: the C++ `add_subdirectory(engine)` now points at the Rust
workspace, which CMake cannot build. Repointing to `engine-old/` is the only correct fix; there is no
second "compat" path.

## Grounding (real files/symbols)

- `CMakeLists.txt` — `add_subdirectory(engine)` (line 31) must become `add_subdirectory(engine-old)`;
  `add_subdirectory(tools/sa)` (line 32) is unaffected (it builds the C++ `sa`, which the Rust `sa`
  later supersedes).
- `engine-old/CMakeLists.txt` — already self-consistent (its `target_sources` paths are relative to
  `CMAKE_CURRENT_SOURCE_DIR`, which is now `engine-old/`); its `POST_BUILD copy_directory` of
  `assets/models|fonts|icons` and `saffron_compile_shaders(... assets/shaders ...)` already resolve
  against `engine-old/assets/` (the assets moved with the tree). No edit needed beyond confirming.
- `tools/gen-control-dto/gen.ts` — `repoRoot` (lines 37–38) + the six output/source paths naming
  `engine/source/saffron/...` (`dtoFile`, `cppOut`, `sceneSerdeOut`, `componentDefsOut`,
  `sceneEditComponentsFile`) must become `engine-old/source/saffron/...`. The `tsOut`, `openRpcOut`,
  `manifestOut` paths (`editor/`, `schemas/`) are unaffected.
- `tools/ci/check.sh` — the `git diff --exit-code` generated-file list (lines 25–30) names
  `engine/source/saffron/...`; repoint to `engine-old/source/saffron/...`.
- `tools/check-script-defs/check.ts` — `read("/engine/source/saffron/...")` (lines 14–18); repoint to
  `/engine-old/source/...`.
- `tools/check-projects/check.sh` — `ENGINE="$REPO/build/debug/bin/SaffronAnima"` (line 8) is
  unaffected (build output dir, not source); `import-model "$REPO/engine/assets/models/cube.gltf"`
  (line 66) must become `engine-old/assets/models/cube.gltf`.
- `Makefile` — the `lint:` recipe `run-clang-tidy -p "$(BUILD_DIR)" engine/source tools/sa/source`
  (line 182) must become `engine-old/source tools/sa/source`. The `CPP_LS` `git ls-files` glob (line
  53) already matches `engine-old/` files by extension; confirm no path filter excludes it.
- `tests/e2e/harness.ts` — `SAFFRON_ANIMA_BIN ?? join(REPO, "build", "debug", "bin", "SaffronAnima")`
  (line 18) is unaffected (build output dir); recorded here so the reviewer knows it was checked.

## Acceptance gate

- The C++ build still produces the engine binary inside the toolbox:
  `toolbox run -c saffron-build bash -lc 'cmake --preset debug && cmake --build build/debug -j8'`
  succeeds and emits `build/debug/bin/SaffronAnima`.
- `bun run tools/gen-control-dto/gen.ts` runs and rewrites the generated files under
  `engine-old/source/...` (not `engine/`); a subsequent `git diff --exit-code` over that list is clean.
- The C++ steps of `tools/ci/check.sh` (the `git diff` guard + the `cmake --build` step + the
  `check-script-defs` + `check-projects` steps) pass against `engine-old/`.
- `grep -rE '"\bengine/source|/engine/source|engine/assets' tools/ CMakeLists.txt Makefile` returns no
  hits (every C++-side reference now names `engine-old/`); the Rust-side `engine/` workspace is the only
  remaining user of the bare `engine/` path.
- The Cargo workspace (the `00-foundations` scaffold) still compiles (`cargo build --workspace` from
  `engine/`) — this phase does not disturb it.
