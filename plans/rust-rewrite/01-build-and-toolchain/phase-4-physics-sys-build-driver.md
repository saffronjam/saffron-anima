# Phase 4 — `saffron-physics-sys` build driver (Jolt determinism flags)

**Status:** COMPLETED
**Depends on:** phase-2-profiles-and-workspace-build

## Goal

Establish the `saffron-physics-sys` `build.rs` build-driving skeleton: vendored Jolt 5.3.0 compiled
from source by `cc`/`cxx-build`, with the determinism + arch + FP flags confined to that crate's
translation units, and the link flags emitted so `saffron-physics` links cleanly. This phase owns *how
the FFI build is driven and flagged*; the bridge C++ shim classes, the `cxx` interface, and the
determinism gate are PP-11 / `05-physics-jolt-bridge`. After this phase the `*-sys` crate compiles
vendored Jolt with the exact flag set the C++ build used, isolated from the rest of the workspace.

## Why this shape (NO LEGACY)

The published Rust Jolt crates (`joltc-sys` pins Jolt 5.0.0, `rolt`) are rejected: they pin the wrong
version and miss the advanced API (CharacterVirtual, Ragdoll, SwingTwist+motors), and — the build-side
reason this phase exists — they build Jolt *without* the determinism flags, silently breaking
bit-exactness. So the build is owned: vendor 5.3.0, compile it ourselves with the flags re-applied.

The flag *isolation* is the load-bearing decision and it is exactly the C++ design re-expressed at a
crate boundary. In C++, `physics.cpp` was the **only** TU that included `<Jolt/...>`, and the arch/FP
flags (`SAFFRON_JOLT_COMPILE_OPTIONS`) were applied to *only that source* via
`set_source_files_properties`, because leaking `-pthread`/`-mavx2` into other TUs broke the `import std`
BMI. In Rust there is no `import std` BMI to protect, but the isolation principle survives for a
different reason: the determinism FP flags (`-ffp-model=precise`, `-ffp-contract=off`) and `-mavx2`
must apply to the Jolt + shim TUs **only**, so the rest of the workspace is not silently recompiled with
arch flags that would change its float results. The `*-sys`/safe-wrapper split *is* that isolation: the
flags live in `saffron-physics-sys`'s `build.rs` and reach nothing else.

The `-pthread` complication from CMake disappears: it was dropped from the per-TU compile options
*because* it toggled the std-module POSIX-thread langopt; with no module-std, Jolt's
`JobSystemThreadPool` just links against the platform threads normally and `build.rs` emits the link
flag. One fewer special case.

`saffron-physics-sys` is one of the three crates that `#![allow(unsafe_code)]` (the FFI seam, per the
foundations lints policy); the safe wrapper `saffron-physics` keeps `deny`.

## Grounding (real files/symbols)

- `cmake/Dependencies.cmake` — the Jolt build contract:
  - `CROSS_PLATFORM_DETERMINISTIC ON` (→ `JPH_CROSS_PLATFORM_DETERMINISTIC`), `DOUBLE_PRECISION OFF`
    (single precision), `OVERRIDE_CXX_FLAGS OFF`;
  - the `TARGET_SAMPLES/UNIT_TESTS/HELLO_WORLD/VIEWER/PERFORMANCE_TEST OFF` exclusions;
  - `GIT_TAG v5.3.0`, `SOURCE_SUBDIR Build` (Jolt's CMake lives under `Build/`), the exported `Jolt`
    target;
  - `target_compile_options(Jolt PRIVATE -Wno-error)` (clang 21 flags the `-ffp-model=precise` +
    `-ffp-contract=off` determinism pairing under `-Woverriding-option`, failing Jolt's own
    `-Werror`); `build.rs` must pass `-Wno-error` (or omit `-Werror`) for Jolt's TUs;
  - `SAFFRON_JOLT_COMPILE_OPTIONS` = Jolt's `INTERFACE_COMPILE_OPTIONS` (`-mavx2` etc.), captured and
    applied to only the physics TU; the `-pthread` removal note (link-only, dropped from compile).
- `engine-old/CMakeLists.txt` — `set_source_files_properties(source/saffron/physics/physics.cpp
  PROPERTIES COMPILE_OPTIONS "${SAFFRON_JOLT_COMPILE_OPTIONS}")` — the single-TU arch-flag application
  that the `*-sys` crate boundary replaces.
- `engine-old/source/saffron/physics/physics.cpp` — the sole Jolt TU + its `pimpl`; PP-11 ports the
  orchestration above this build skeleton.
- The crate-graph entry: `00-foundations` README §2.1 reserves the `saffron-physics-sys` /
  `saffron-physics` split ("This area (00) only reserves the split; PP-11 designs it"); this phase fills
  the *build-driving* half, PP-11 fills the bridge/wrapper half.

## Acceptance gate

- `saffron-physics-sys` has a `build.rs` that vendors Jolt 5.3.0 (submodule or vendored source under the
  crate), compiles it via `cc`/`cxx-build` with `JPH_CROSS_PLATFORM_DETERMINISTIC` defined, single
  precision, `-ffp-model=precise -ffp-contract=off -mavx2`, and `-Wno-error` for Jolt's TUs.
- `cargo build -p saffron-physics-sys` succeeds inside the toolbox and links the static Jolt archive;
  `cargo build --workspace` still succeeds.
- The arch/FP flags appear **only** in `saffron-physics-sys`'s build (verified via
  `cargo build -p saffron-physics-sys -vv` showing the flags on the Jolt TUs, and *not* present in any
  other crate's compile lines).
- `saffron-physics-sys/src/lib.rs` carries `#![allow(unsafe_code)]` with a top-of-file justification
  naming the Jolt FFI seam; every other crate still satisfies the workspace `unsafe_code = "deny"` lint.
- A trivial FFI round-trip (a single `JPH::Vec3` or a Jolt version-string call through the shim) returns
  the expected value, proving the link is real — the *minimal* bring-up; the full bridge + the
  determinism gate are PP-11's acceptance, referenced not duplicated here.
