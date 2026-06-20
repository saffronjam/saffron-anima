# Phase 3 — Retire the C++ tree: delete `engine-old/`, the C++ tooling, and the parity rig

**Status:** NOT STARTED
**Depends on:** 14-migration-and-cutover:phase-2-binary-flip

## Goal

Make the repository Rust-only. Delete `engine-old/` (the C++26 engine, now reference-only and
superseded), the C++-only tooling that fed it, the parity rig that only existed to compare the two
engines, and every build/tooling reference that pointed at `engine-old/`. After this phase there is one
engine, one build, one code path — the NO-LEGACY end state.

## Why this shape (NO LEGACY)

The locked ground rules state `engine-old/` is reference-only and **deleted after cutover**, and that a
feature is not done while a superseded flow still exists anywhere in the tree. The Rust binary has been
the editor's default since phase 2 and passed the full gate + parity rig in phase 1, so the C++ engine
has no remaining role. Keeping it "just in case" is exactly the deferred-cutover the rules forbid. The
parity rig (`13:phase-7`) only had meaning while two engines were alive; it is deleted with the C++ tree.
Project-data migration is out of scope (start a fresh project), so there is no save-format bridge to
preserve. This is the single irreversible phase; it is gated on phase 2 being green and is the author's
deliberate retirement act.

## Grounding (real files/symbols)

What is deleted (each was either C++ engine source, C++-only tooling, or a repointed reference):

- `engine-old/` — the entire C++26 engine tree (the reference the whole plan grounded against).
- The C++ build references repointed in `01-build-and-toolchain/phase-1-relocation-repoint.md` —
  `add_subdirectory(engine-old)` in the root `CMakeLists.txt`, the `cmake/` modules, `CMakePresets.json`,
  the Makefile's C++ targets/flock/clang-tidy roots — all removed (Cargo + the justfile are the build).
- `tools/gen-control-dto/` (the 3504-LOC `gen.ts` regex parser) — superseded by `xtask gen-protocol`
  (`10-protocol-codegen:phase-5`).
- `tools/sa/` (the C++ CLI + vendored `args.hxx`) and `cmd/sa` (the Python wrapper) — superseded by the
  Rust `sa` bin (`11-sa-cli`).
- `tools/check-script-defs/` — the Luau-defs drift tripwire, obsolete once defs are generated from the
  binding source (`12-scripting:phase-9` / `10-protocol-codegen:phase-6`).
- `13-testing-and-verification/phase-7-cross-engine-parity-rig.md`'s rig — deleted (it only compared
  the two engines).
- `CONVENTIONS.md` (Go-flavored C++, already retired in favor of `00-foundations/conventions.md`) — the
  README §3 note that it stays "only as historical reference for reading `engine-old/`" lapses once
  `engine-old/` is gone.

What stays: `editor/` (unchanged), `tests/e2e/` (the wire-driven suite, now single-engine),
`schemas/control/` (the frozen contract + regenerated artifacts), `engine/` (the Rust workspace),
`docs/`, and the kept toolbox/justfile/gate.

## Acceptance gate

- `engine-old/` and the listed C++-only tooling/references no longer exist; a tree-wide grep finds no
  `engine-old`, no `add_subdirectory(engine`, no `gen.ts`/`args.hxx`/`cmd/sa`/`check-script-defs`
  reference outside the plan docs.
- The Cargo workspace compiles, `cargo test --workspace` is green, and the reproducible gate
  (`13:phase-9`) passes with no path resolving into `engine-old/`.
- The editor builds and boots on the Rust engine; the full `tests/e2e` suite passes (now only ever
  against the Rust binary — the parity rig is gone, the suite remains).
- `SAFFRON_ANIMA_BIN` pointed at the old C++ path now fails to launch (the binary no longer builds) —
  confirming the single-engine end state; the env var survives only as a path override for the Rust
  host (e.g. a release vs debug build).
- The corresponding `plans/rust-rewrite/` phase files are marked `COMPLETED`; the plan tree itself may be
  retired per the repo's plans policy once the whole rewrite is `COMPLETED`.
