# Phase 1 — Workspace scaffold: the Cargo crate graph that compiles empty

**Status:** COMPLETED

**Depends on:** —

## Goal

Replace the placeholder `engine/Cargo.toml` with the real virtual workspace and stub every member
crate from §2 of the area README so the whole graph compiles empty (`cargo build` and `cargo test`
green over a graph of `lib`/`bin` crates whose `lib.rs`/`main.rs` are skeletons). This is the
physical shape every later phase fills in: each subsystem already has its crate, its dependency edges,
and its place in the leaf-up order. Nothing of substance is ported here — this phase exists so that
from the very first real port (phase 2, `saffron-core`) the workspace is already a green, correctly-
wired DAG, and adding code to a crate never requires also inventing its `Cargo.toml` or its edges.

The crate graph (names, directories, dependency edges, lib/bin/sys split) is exactly the one locked in
the area README §2. This phase materializes it; it does not re-decide it.

## Why this shape (NO LEGACY)

- The placeholder `engine/Cargo.toml` (a bare `[workspace]` with `members = ["crates/*"]` and an empty
  `crates/.gitkeep`) is **deleted and rewritten**, not extended. It was a marker created during setup;
  PP-1 was always going to supersede it (it says so in its own comment). There is no "keep the
  placeholder around" — the workspace root is rewritten in one move.
- **All member crates are created up front, empty, in one phase** rather than added one-at-a-time as
  each subsystem is ported. The DAG is the keystone contract; materializing it whole means the
  dependency edges are reviewable as a unit and every later phase only *fills* a crate that already
  exists with the right edges. A crate added lazily mid-port risks a wrong or missing edge slipping in
  unreviewed. The empty crates compile (a `lib.rs` with a doc comment), so the workspace is green from
  this phase onward — the "green at every phase" gate holds from the first phase.
- **One crate per logical module** (README §2.1 records the three deliberate deviations:
  `saffron-protocol` pulled out, `saffron-physics-sys` split, `saffron-rendering` kept whole). No
  catch-all `util` crate, no facade crate that merely re-exports — crates depend on each other
  directly. This is the NO-LEGACY "one way, one path" rule applied to the build graph.
- **Version pins live once in `[workspace.dependencies]`.** Member crates use `dep.workspace = true`.
  This is the Cargo replacement for the C++ `FetchContent` pin-in-one-place; it makes the PP-2 pin
  list the single source of dependency versions and structurally prevents two crates pinning different
  versions of `glam`/`serde`/`ash`.

## Grounding (real files/symbols)

- `engine/Cargo.toml` — the placeholder virtual workspace this phase replaces (its own comment says
  "Expect PP-1 to rewrite it"); `engine/crates/.gitkeep` — the empty crate dir.
- `engine-old/CMakeLists.txt` — the `FILE_SET CXX_MODULES` block is the source of the module DAG that
  becomes the workspace member list and the inter-crate dependency edges; the
  `SAFFRON_JOLT_COMPILE_OPTIONS` line on `physics.cpp` is why `saffron-physics-sys` is a separate
  `*-sys` crate (its `build.rs` re-applies the determinism/arch flags to only its TUs — reserved here,
  designed in PP-11).
- `engine-old/source/saffron/control/control_dto.cppm` — the DTO source that, in Rust, becomes the
  standalone `saffron-protocol` crate (so the `sa` CLI links it without the engine).
- Area README §2 (this folder) — the locked crate graph this phase materializes; `conventions.md` §9
  — the `#![deny(unsafe_code)]` workspace lint + the three FFI opt-outs and the naming rules the stub
  crates follow.

## Acceptance gate

- `cargo build` over the full workspace (every member crate from README §2 present as an empty
  `lib`/`bin`/`sys` stub) succeeds inside the toolbox.
- `cargo test` succeeds (zero tests is green; the harness compiles every crate).
- `cargo metadata` shows the dependency edges exactly matching README §2 (a `saffron-scene` that
  depends on `saffron-rendering`, or any other edge not in the locked graph, fails review). A small
  `#[test]` in `xtask` or a review checklist asserts the member list and edge set match the README
  table.
- `cargo check` is clean under the workspace `[workspace.lints]` policy (`unsafe_code = "deny"` with
  the three FFI crates' documented opt-outs); `cargo fmt --check` and `cargo clippy` are clean.
- The placeholder `crates/.gitkeep` is gone and `engine/Cargo.toml` is the real workspace root with
  `[workspace.package]`, `[workspace.dependencies]`, `[workspace.lints]`, and the `[profile.*]`
  section reserved for PP-12.
