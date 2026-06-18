# Phase 2 — The binary flip: editor and tests default to the Rust engine

**Status:** NOT STARTED
**Depends on:** 14-migration-and-cutover:phase-1-parity-signoff

## Goal

Make the Rust `saffron-host` the engine the editor and the test/gate machinery spawn **by default**, by
re-pointing the single `SAFFRON_ANIMA_BIN` default that both the editor spawn site and the e2e harness
read. After this phase, `just run` / `bun run tauri dev` / `make e2e`-equivalent all drive the Rust
engine with no further env override, and the editor (frontend + `src-tauri`) is unchanged.

## Why this shape (NO LEGACY)

Because both consumers already key off `SAFFRON_ANIMA_BIN` (proven in phase 1), the flip is a *default*
change, not a feature flag and not a runtime selector. There is exactly one editor child process; which
binary it is, is decided once at spawn by the env default. We do **not** add a "use C++ if Rust fails"
fallback — that would be a second code path for the same job (forbidden); if the Rust binary is wrong,
the gate is red and phase 1 did not complete, so the flip never happened. The flip is reversible only in
the trivial sense that the env var can be overridden back to the C++ path while `engine-old/` still
exists (phase 3 removes that escape hatch deliberately).

## Grounding (real files/symbols)

- `editor/src-tauri/src/lib.rs` — `engine_binary()` (`:186`): the `unwrap_or_else` default changes from
  `build/debug/bin/SaffronAnima` to the Rust host path (`target/<profile>/saffron-host`, resolved the
  same `repo_root()`-relative way). `SAFFRON_ANIMA_BIN` still overrides, so a developer can still point
  at the C++ binary while it exists.
- `tests/e2e/harness.ts` — `SAFFRON_ANIMA_BIN` default (`:18`): same change, same default target.
- `01-build-and-toolchain/phase-5-justfile-and-toolbox.md` — the `just run` / `run-engine` / `e2e`
  recipes' engine-binary env: re-pointed to the Rust host (the recipes that set `SAFFRON_ANIMA_BIN` or
  invoke the engine binary directly).
- `01-build-and-toolchain/phase-6-reproducible-gate.md` — the gate script's present-only smoke + e2e
  step run against the Rust host after the flip.

## Acceptance gate

- The Cargo workspace compiles and `cargo test --workspace` is green.
- With no `SAFFRON_ANIMA_BIN` set, the editor spawn site and the e2e harness both resolve to the Rust
  `saffron-host`; a smoke `bun run tauri dev` boots the editor on the Rust engine and shows a live
  viewport frame, and the full `tests/e2e` suite passes at the new default.
- The reproducible gate (`13:phase-9`) passes with the Rust binary as the default engine.
- Setting `SAFFRON_ANIMA_BIN` back to the C++ path still works (the override is honored) — confirming the
  flip is a default, not a hard-coded removal, until phase 3.
