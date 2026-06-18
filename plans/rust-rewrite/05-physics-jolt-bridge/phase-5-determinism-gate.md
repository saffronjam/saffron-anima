# Phase 5 — The cross-arch bit-exact determinism gate (BLOCKING go/no-go)

**Status:** COMPLETED
**Depends on:** 05-physics-jolt-bridge:phase-4

> [!CAUTION]
> **This phase is a BLOCKING go/no-go gate (pre-plan PP-11).** It must pass before any further gameplay
> ports onto the bridge (phases 6-10) and before the rewrite is committed to past the spike. If the
> Rust bridge does not produce sim traces bit-identical to the C++ engine across **both x86 and ARM**,
> the lockstep/replay premise — the entire reason for keeping Jolt over rapier — has collapsed, and the
> decision must be reconsidered (descope lockstep, or stop). Do not "mostly passes." Bit-exact means
> byte-for-byte equal serialized floats.

## Goal

Prove that the vendored-Jolt-5.3.0 + `cxx` bridge, built with the determinism flags (phase 1), produces
the same simulation as the C++ `SaffronAnima` engine, and that the result is identical across
architectures. The mechanism: a fixed deterministic scenario (a body stack + a passive ragdoll + a
walking `CharacterVirtual`), stepped a fixed number of substeps, with a per-step trace of every body's
position/orientation serialized to a stable byte form, diffed (a) Rust-vs-C++ on the same arch and (b)
Rust-x86-vs-Rust-ARM.

## Why this shape (NO LEGACY)

The feasibility study (§4.3, §spike step 2) names this exact gate and its pass condition. Bit-exactness
is a property of the *build*, not the library, so the gate is the only thing that proves the flag set in
phase 1 actually took. It runs early — right after the two hard features are bound — so a flag drift,
an FMA the compiler contracted, an AVX-512 `-march=native` slip, or a `cxx` value-passing reorder is
caught before phases 6-10 build on a non-deterministic base. The trace format is frozen here and reused
as a CI artifact: the C++ engine emits a golden trace once (a one-off harness, or the existing e2e wire
if a `physics-trace` command is added — see area 09/13), and the Rust gate diffs against it. There is no
"tolerance" knob — that would defeat the purpose; floats are compared as raw bytes.

## Grounding (real files/symbols)

- `plans/rust-rewrite-feasibility.md` §4.3 — the determinism verdict and the silent-flag-drift risk;
  §spike step 2 — "run a fixed scenario through both the C++ engine and the Rust bridge and **diff sim
  traces for bit-exactness across x86 and ARM.** *Pass: bit-identical traces + the two hard features
  bound.*"
- `cmake/Dependencies.cmake:75-76` — `CROSS_PLATFORM_DETERMINISTIC` + single precision, the flags this
  gate validates took effect.
- `engine-old/source/saffron/physics/physics.cpp:960-989` — the fixed-step `Update(PhysicsFixedStep, 1,
  …)` the trace must reproduce step-for-step; `PhysicsFixedStep = 1/60` (`physics_types.cppm:43`).
- `engine-old/source/saffron/physics/physics.cpp:517` — the creation-order body storage that keeps the
  sim reproducible (the trace iterates bodies in that order).
- `engine-old/source/saffron/physics/physics.cpp:1318-1382` — `writeRagdollPoses` produces the
  per-bone transforms a ragdoll trace samples (read the part world transforms directly for the trace,
  pre-blend).

## Work

- Define the frozen scenario as a fixture (a hardcoded scene, not loaded from disk, so it cannot drift):
  ~10 dynamic boxes in a stack + a Static floor, one 3-5 bone passive ragdoll, one `CharacterVirtual`
  given a fixed desired-velocity sequence. Fixed `PhysicsFixedStep`, fixed substep count (e.g. 600
  steps = 10 s).
- Define the trace format: per step, for each body in creation order, the position `[f32;3]` and
  orientation `[f32;4]` written as little-endian raw bytes (and the character position, and each ragdoll
  part transform). A whole-run SHA-256 over the byte stream is the comparison key.
- A Rust harness (a `tests/` integration test in `saffron-physics`, gated to run in the toolbox) runs
  the scenario and emits the trace hash.
- A C++ golden trace: a one-off harness in `engine-old` (or a `physics-trace` control command driven by
  the e2e suite, area 13) emits the same scenario's trace from the C++ engine. Commit the golden hash +
  byte file as a fixture.
- Run on **both x86_64 and aarch64** (the self-hosted runner / a second arch in the toolbox). Compare:
  Rust-x86 == C++-x86, Rust-aarch64 == C++-aarch64 (if a C++ aarch64 build exists) or at minimum
  Rust-x86 == Rust-aarch64. The cross-arch Rust-vs-Rust equality is the non-negotiable assertion.
- Wire the gate as a blocking CI test (area 13's determinism gate slot), failing the build on any
  mismatch.

## Acceptance gate

- `cargo test -p saffron-physics --test determinism` passes: the Rust trace hash equals the committed
  C++ golden trace hash on x86_64.
- The same test passes on aarch64, and **Rust-x86 trace hash == Rust-aarch64 trace hash** (byte-exact).
- Any single-byte difference fails the test (no tolerance). A failure is escalated per the go/no-go rule
  — it is not "fixed" by relaxing the comparison.
- The two hard features (CharacterVirtual `ExtendedUpdate`, the passive SwingTwist ragdoll) are present
  in the traced scenario and produced finite, bounded, identical output.
