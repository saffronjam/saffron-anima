# Phase 7 — The cross-engine parity rig (C++ vs Rust, cutover only)

**Status:** NOT STARTED

**Depends on:** 13-testing-and-verification:phase-3-bun-e2e-as-parity-harness, 13-testing-and-verification:phase-2-golden-snapshot-infrastructure, 05-physics-jolt-bridge:phase-5-determinism-gate

## Goal

Build the rig that asserts the Rust engine matches the C++ engine where it *must*, used as the
sign-off input to the cutover (area `14`). The four standing gates prove the Rust engine is
*internally* correct; parity proves it is *behaviorally identical to the engine it replaces* on the
three contracts the editor and existing projects cannot tolerate drifting: rendered pixels, physics sim
traces, and serialized bytes. The rig exists only while both binaries are alive and is deleted with
`engine-old/` at cutover (NO LEGACY).

Concretely, three comparators, each run against both binaries via `SAFFRON_ANIMA_BIN`:

- **Golden images.** Drive the same scene through the `screenshot` / `preview-render` commands on each
  binary and compare the PNG buffers (the existing `*_render.test.ts` pixel tier, `Buffer.equals`, no
  image-diff dep). Under llvmpipe both engines rasterize on the CPU, so exact-match is the target where
  the pipeline is deterministic; where it is not (TAA/temporal jitter), compare with a tolerance the
  rig records and the cutover sign-off accepts. The baselines are C++-generated and committed (the
  golden infra, phase 2).
- **Physics sim traces.** Run the fixed stacking + ragdoll scenario from the determinism gate through
  both engines over the wire and diff the serialized body-state traces for bit-exactness. This is the
  determinism gate's scenario reused on a *different axis* (C++-engine-vs-Rust-engine rather than
  x86-vs-ARM): the physics phase 5 gate proves the Rust bridge is cross-arch deterministic; this proves
  it is bit-identical to the C++ engine's Jolt. Both must hold for lockstep replay compatibility.
- **Serde byte-equality.** A scene/material/model authored by the C++ engine loads and re-saves
  byte-identically through the Rust engine and vice versa (the frozen-format requirement made
  bidirectional). This reuses the golden fixtures (phase 2) plus a round-trip that boots each binary,
  saves a project, and byte-diffs the on-disk JSON / `.smat` / `.smodel`.
- **The driver** is the bun e2e harness (phase 3) parameterized over `SAFFRON_ANIMA_BIN`, plus the
  golden snapshot helper (phase 2). It produces a parity report (which comparators are exact, which are
  within a recorded tolerance and why) consumed by `14-migration`'s sign-off.

## Why this shape (NO LEGACY)

- **Parity is the cutover gate, and it is inherently transitional.** Pre-plan §0/PP-14: "the editor
  stays on the C++ `SaffronAnima` (via `SAFFRON_ANIMA_BIN`) until the Rust binary passes the full
  e2e/contract/parity gate; then the binary is flipped." The rig is the thing that produces the "passes
  parity" verdict. It is not permanent infrastructure — once `engine-old/` is deleted there is no second
  engine to compare against, so the rig is removed with it. Keeping it alive after cutover would be
  exactly the "old path kept for back-compat" the conventions forbid.
- **It reuses, never re-implements.** The three comparators are the *existing* pixel tier, the
  *existing* determinism scenario, and the *existing* golden fixtures, run against two binaries. The
  feasibility study's recommended spike already does C++-vs-Rust trace diffing for physics; this phase
  generalizes that one technique to pixels and serde, on the same harness. No new test harness, no new
  fixture format.
- **Tolerances are recorded, not hidden.** Where exact pixel match is impossible (temporal effects), the
  rig records the tolerance and the reason rather than silently loosening; the cutover sign-off
  (`14-migration`) reviews each non-exact comparator explicitly. A drift that cannot be explained is a
  cutover blocker, not a rounding footnote.

## Grounding (real files/symbols)

- The pixel tier to parameterize: `tests/e2e/*_render.test.ts` (e.g. `material_codegen_render.test.ts`
  — `preview-render` → PNG → `Buffer`/base64 compare), `tests/e2e/imggen.ts` (`makePng` + PNG decode,
  the no-image-diff-dep precedent), `tests/e2e/AGENTS.md` (the pixel-tier description: "compare buffers
  directly … Golden-image baselines are not wired up yet" — this phase wires them).
- The physics scenario: `05-physics-jolt-bridge:phase-5-determinism-gate` (the fixed stacking + ragdoll
  trace) and `engine-old/source/saffron/physics/physics.cpp` — `runPhysicsSelfTest` (`:1533`) as the
  scenario seed; the body-state trace serialization the diff consumes.
- The serde formats: the golden fixtures from `13` phase 2 (`.smesh`/`.smat`/`.sanim`/`.smodel` + scene
  JSON) and their C++ writers under `engine-old/source/saffron/{geometry,assets,scene}`.
- The driver: `tests/e2e/harness.ts` `Engine.boot` honoring `SAFFRON_ANIMA_BIN` (`:17`) — run twice,
  once per binary.

## Acceptance gate

- The parity rig runs all three comparators against both the C++ and Rust binaries and emits a parity
  report: per-scene golden-image verdict (exact or recorded-tolerance + reason), the physics trace
  bit-exact verdict, and the serde byte-equality verdict.
- A worked example is green: one `preview-render` scene matches byte-for-byte across both binaries; one
  scene project saved by C++ loads + re-saves byte-identically through Rust.
- The physics-trace comparator confirms the Rust bridge's trace equals the C++ engine's for the gate
  scenario (complementing the cross-arch determinism gate).
- The rig is gated behind the presence of `engine-old/` (it no-ops / is absent once that directory is
  deleted), documented as cutover-only.
- The Cargo workspace compiles; `cargo test --workspace` and the bun smoke slice stay green.
