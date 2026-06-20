# Phase 1 — `saffron-physics-sys`: fetch Jolt 5.3.0 on demand and own its deterministic build

**Status:** COMPLETED
**Depends on:** 00-foundations (workspace scaffold, `[workspace.dependencies]`, `[workspace.lints]`), 01-build-and-toolchain (the `cc`/`cxx-build` toolbox role)

## Goal

Stand up the `saffron-physics-sys` FFI crate: fetch Jolt 5.3.0 on demand, compile it from `build.rs` with the
exact cross-platform-deterministic flags, and expose the Jolt global lifecycle (`init`/`shutdown`,
mirroring `initPhysics`/`shutdownPhysics`). No bridge surface yet beyond a smoke entry point that proves
Jolt links and the determinism defines are active. This phase isolates the single riskiest variable —
the build flags — so the determinism gate (phase 5) tests a known-good build, not a guess.

## Why this shape (NO LEGACY)

The C++ engine confined `<Jolt/...>` to one TU behind a `pimpl` so the arch/thread flags and Jolt types
never leaked into the `import std` modules (`engine-old/CMakeLists.txt:67`, `Dependencies.cmake:95`). In
Rust the crate boundary *is* the pimpl: `saffron-physics-sys` is the only crate that compiles Jolt or
holds an `unsafe` line, and `#![allow(unsafe_code)]` is scoped to it (foundations contract). We do **not**
use the published `joltc-sys`/`rolt` crates: they pin Jolt 5.0.0, build without the determinism flags,
and lack the API this engine needs (feasibility §4.3, §5). There is one Jolt, one build, one flag list —
mirrored line-for-line from `Dependencies.cmake` so a reviewer can diff them.

The Jolt source is **not** stored in this repo. `build.rs` fetches it **on demand** from the official
pinned release tarball
(`https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.3.0.tar.gz`), verifies it against an
embedded SHA-256 of the canonical archive, and extracts it into the gitignored
`engine/crates/physics-sys/vendor/JoltPhysics-5.3.0/` cache — which is exactly the directory the
tarball's top level unpacks to. This is the direct expression of `FetchContent_Declare(... GIT_TAG
v5.3.0)`: a tag + a checksum, not a range. The cache is a persistent download cache: it survives
`cargo clean` and is removed only by an explicit `just clean-deps`; the first build (or `just
fetch-deps`) populates it. The pin is exact on two axes — the git commit `v5.3.0` resolves to and the
SHA-256 over the release archive (both `const`s in `build.rs`) — so the build is reproducible
byte-for-byte and a Jolt bump is a deliberate, reviewed replay-format migration, never a silent
dependency update. A missing network fails closed with a clear error pointing at `just fetch-deps`.

## Grounding (real files/symbols)

- `cmake/Dependencies.cmake:68-109` — the authoritative flag set this `build.rs` reproduces:
  `CROSS_PLATFORM_DETERMINISTIC ON` (`:75`), `DOUBLE_PRECISION OFF` (`:76`), `OVERRIDE_CXX_FLAGS OFF`
  (`:77`), targets-off for samples/tests/tools (`:78-82`), `GIT_TAG v5.3.0` (`:85`), `Jolt PRIVATE
  -Wno-error` (`:93`), `SAFFRON_JOLT_COMPILE_OPTIONS` capture (`:103`), `-pthread` removed from compile
  (`:109`).
- `engine-old/CMakeLists.txt:67-72` — the per-TU re-apply of `SAFFRON_JOLT_COMPILE_OPTIONS` to
  `physics.cpp` (the `-mavx2` confinement).
- `engine-old/source/saffron/physics/physics.cpp:608` (`initPhysics`) / `:621` (`shutdownPhysics`) —
  `RegisterDefaultAllocator`, install `Factory::sInstance`, `RegisterTypes`; idempotent; the
  `JPH::Trace`/`JPH::AssertFailed` hooks (`joltTrace` `:134`, `joltAssertFailed` `:145`).
- `plans/rust-rewrite-feasibility.md` §4.3 — "an off-the-shelf Rust Jolt crate builds Jolt *without*
  `JPH_CROSS_PLATFORM_DETERMINISTIC` / `-ffp-model=precise` / confined `-mavx2`, silently breaking
  bit-exactness."

## Work

- Create `engine/crates/physics-sys/` (package `saffron-physics-sys`, crate id `saffron_physics_sys`)
  with `#![allow(unsafe_code)]` + a top-of-file justification naming the Jolt FFI seam.
- Fetch Jolt `v5.3.0` on demand in `build.rs`: when the gitignored
  `vendor/JoltPhysics-5.3.0/` cache is absent, download the official release tarball, verify the
  embedded SHA-256 (`fetch::ARCHIVE_SHA256`, derived once from the canonical archive), extract into the
  cache, then proceed. The pinned commit lives alongside it as `fetch::COMMIT` (the former
  `VENDORED_COMMIT.txt`, now folded into `build.rs`). A `just fetch-deps` recipe performs the same
  pinned+checksummed fetch up front as an explicit cold-start entry point (and `just clean-deps` drops
  the cache); the missing-source error points at it. Then drive `cc`/`cxx-build` over
  Jolt's `Build/` source set with: `JPH_CROSS_PLATFORM_DETERMINISTIC` defined, single precision (no
  `JPH_DOUBLE_PRECISION`), `-ffp-model=precise -ffp-contract=off`, `-mavx2`, `-Wno-error` (Jolt trips
  `-Woverriding-option` on the FP pairing), clang + libc++ inside the toolbox. The same `JPH_*` defines
  reach every Jolt TU (they change struct layout). Link `Threads` at link time (the C++ `-pthread`).
- Expose `init() -> Result<(), &'static str>` / `shutdown()` wrapping the Jolt globals (the bridge to
  these lands in phase 2; this phase can call them through a minimal `extern "C"` shim or the first
  `cxx` function).
- Add a build-time assert that the determinism + single-precision defines are set (compile-time
  `#error` in the shim header if `JPH_DOUBLE_PRECISION` is defined or `JPH_CROSS_PLATFORM_DETERMINISTIC`
  is not), so a flag drift fails the build, not the gate.

## Acceptance gate

- Cold start works: with the `vendor/` cache absent, `cargo build -p saffron-physics-sys` (or `just
  fetch-deps`) fetches the pinned tarball, the SHA-256 verifies, the tree extracts, and Jolt
  static-links — all inside the toolbox.
- A `#[test]` `init_shutdown_idempotent` in `saffron-physics-sys` calls `init()` then `shutdown()` twice
  and asserts both succeed (the `runPhysicsSelfTest` init/shutdown spine, `physics.cpp:1533`, re-homed
  as a real test — never a runtime self-test).
- A `#[test]` (or a `build.rs` assertion surfaced as a const) confirms the build saw
  `JPH_CROSS_PLATFORM_DETERMINISTIC` and *not* `JPH_DOUBLE_PRECISION`.
- `#![deny(unsafe_code)]` holds everywhere except this crate; no other crate gained a Jolt dependency.
