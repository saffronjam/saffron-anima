# Phase 2 — Cargo profiles + workspace build

**Status:** COMPLETED
**Depends on:** 00-foundations:phase-1-workspace-scaffold, phase-1-relocation-repoint

## Goal

Fill the `[profile.*]` section that `00-foundations` reserved in `engine/Cargo.toml`, and establish that
`cargo build --workspace` produces the engine artifacts inside the toolbox — replacing the
`CMakePresets.json` debug/release build types and the clang/libc++/lld preset machinery. After this
phase the workspace builds and the optimization/debug-info knobs are decided.

## Why this shape (NO LEGACY)

CMake carried two build types (`Debug`, `RelWithDebInfo`) plus a hidden `clang-libcxx` preset pinning
the compiler, stdlib, and linker. In Rust the **toolchain is the toolbox's** (clang/libc++ are not the
Rust compiler; `cargo` uses the toolbox's `rustc`), so the preset's compiler/stdlib/linker pins have no
Cargo analogue and are deleted — they were only there to make `import std` + modules work. What remains
is the genuine profile content: optimization level, debug info, overflow checks. There is exactly one
debug profile (`dev`) and one release profile (`release`); no third "compat" profile.

The one non-default decision — `[profile.dev.package."*"]` raising dependency opt-level — is the
idiomatic Rust expression of the same intent CMake had implicitly (third-party code, like Jolt and the
math libs, was always compiled optimized enough to run): a debug *engine* build whose *dependencies*
(`glam`, `ash` inlining, Jolt via `saffron-physics-sys`) are optimized so the debug build is usable.
This is a measured knob, not a guess — the phase pins the level after measuring a debug-build frame
rate, not before.

`panic = "unwind"` (the default) is kept deliberately: the FFI/shm seams must unwind cleanly across the
Rust/C++ boundary guards (PP-11/PP-10), and the test suite uses `#[should_panic]`; `panic = "abort"`
would defeat both. Recording it here so it is not silently flipped for a marginal size win.

## Grounding (real files/symbols)

- `CMakePresets.json` — `clang-libcxx` (the compiler/stdlib/linker pins: `CMAKE_CXX_COMPILER clang++`,
  `-stdlib=libc++`, `-fuse-ld=lld`, `CMAKE_EXPORT_COMPILE_COMMANDS`), and the `debug`
  (`CMAKE_BUILD_TYPE Debug`) / `release` (`RelWithDebInfo`) configure+build presets. The compiler/
  linker pins are deleted (toolchain is the toolbox's); only the build-type intent maps to profiles.
- `engine/Cargo.toml` — the `[profile.*]` section reserved by `00-foundations` README §2.3 ("The
  `[profile.*]` blocks live here too (PP-12 owns the exact knobs; 00 reserves the section)") is filled
  here. The `[workspace]`/`[workspace.package]`/`[workspace.dependencies]`/`[workspace.lints]` blocks
  are 00's; this phase edits only `[profile.*]` (and adds the `xtask` member to `members` if 00 did not
  already — 00 §2.3 lists `members = ["crates/*", "xtask"]`, so it should already be present; confirm).
- `cmake/Dependencies.cmake` — `JSON_NOEXCEPTION` (the abort firewall) and `GLM_FORCE_DEPTH_ZERO_TO_ONE`
  are *not* profile concerns and not reproduced here — they are subtractions (`serde_json` returns
  `Result`; `glam` has no global depth define, the `[0,1]` clip is a per-projection call decided in
  `02-math-and-geometry`). Noted so the reviewer does not look for them in a profile block.
- `engine-old/CMakeLists.txt` — `CXX_MODULE_STD ON` and `cxx_std_26` (deleted; no Cargo analogue).

## Acceptance gate

- `engine/Cargo.toml` has a `[profile.dev]` (`opt-level = 0`, `debug = true`, `overflow-checks = true`),
  a `[profile.dev.package."*"]` (`opt-level` set to the measured level), and a `[profile.release]`
  (`opt-level = 3`, `debug = true`, `panic = "unwind"`).
- `cargo build --workspace` from `engine/` succeeds inside the toolbox (over the `00-foundations`
  empty-but-compiling member crates), producing `target/debug/` artifacts.
- `cargo build --workspace --release` succeeds.
- `cargo metadata --format-version 1` lists exactly the crates from the foundations crate graph plus
  `xtask`; there is no orphaned or duplicate member.
- No reference to `clang`/`libc++`/`lld`/`CMAKE_*` survives in any Cargo file (the toolchain is the
  toolbox's; the profiles carry only Cargo knobs).
