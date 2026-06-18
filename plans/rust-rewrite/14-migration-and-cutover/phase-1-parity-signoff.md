# Phase 1 — Parity sign-off: qualify the Rust binary for the flip

**Status:** NOT STARTED
**Depends on:** 13-testing-and-verification:phase-7-cross-engine-parity-rig, 13-testing-and-verification:phase-9-reproducible-gate-orchestration, 08-host-and-viewport:phase-6-teardown-drop-graph, 09-control-plane:phase-6-physics-commands, 11-sa-cli:phase-4-help-completions-and-start, 12-scripting:phase-9-luau-api-typegen

## Goal

Prove the Rust `saffron-host` binary is a drop-in replacement for the C++ `SaffronAnima` across the
entire frozen wire surface, by running the full reproducible gate and the cross-engine parity rig with
`SAFFRON_ANIMA_BIN` pointed at the Rust binary while the C++ binary stays the oracle. Produce a single
go/no-go sign-off: every gate green ⇒ qualified for the flip (phase 2); any gate red ⇒ reopen the
failing area's phase. This phase writes **no engine code** — it runs checks owned by earlier phases and
records the verdict.

## Why this shape (NO LEGACY)

The cutover risk must be discharged *before* the flip, not discovered after. The feasibility study's
entire migration premise is that the Rust engine byte-matches the two cross-process contracts so the
editor cannot tell the difference; this phase is the empirical confirmation of that premise. It does not
add a new test harness (that would be a second verification path — forbidden); it composes the harnesses
that already exist — the reproducible gate (`13:phase-9`), the bun e2e suite as a parity harness
(`13:phase-3`), and the cross-engine parity rig (`13:phase-7`) — and asserts them all green against the
Rust binary. The three feasibility go/no-go gates (physics determinism, ECS speed, renderer/shm) are
re-confirmed here, but they already gated their own areas; this is the final cross-check, not their first
run.

## Grounding (real files/symbols)

- `editor/src-tauri/src/lib.rs` — `engine_binary()` (`:186`) reads `SAFFRON_ANIMA_BIN`; pointing it at
  `target/<profile>/saffron-host` makes the editor spawn the Rust engine with no source change.
- `tests/e2e/harness.ts` — `SAFFRON_ANIMA_BIN` (`:18`); the same suite drives either binary.
- `13-testing-and-verification/phase-7-cross-engine-parity-rig.md` — the golden-image / sim-trace /
  serde-byte-equality diffs this sign-off runs C++-vs-Rust.
- `13-testing-and-verification/phase-9-reproducible-gate-orchestration.md` — the gate sequence
  (build → smoke → contract → projects → frontend → `cargo test` → clippy/fmt).
- `05-physics-jolt-bridge/phase-5-determinism-gate.md`, `03-ecs-and-scene/phase-2-ecs-benchmark-gate.md`,
  `08-host-and-viewport/phase-3-shm-abi-gate.md` — the three front-loaded gates re-confirmed here.
- `plans/rust-rewrite-feasibility.md` §8 — the go/no-go bar this phase formalizes.

## Acceptance gate

- The Cargo workspace compiles and `cargo test --workspace` is green.
- With `SAFFRON_ANIMA_BIN` = the Rust `saffron-host`: the reproducible gate (`13:phase-9`) passes
  end-to-end; the full `tests/e2e` bun suite passes; the cross-engine parity rig (`13:phase-7`) is clean
  on all three diffs (golden images within tolerance, the Jolt scenario sim trace bit-identical
  C++-vs-Rust, scene/material/model serde byte-identical both directions).
- The three subsystem gates re-confirm green: physics cross-arch determinism (`05:phase-5`), ECS
  per-frame iteration within ~10% of entt (`03:phase-2`), validation-clean offscreen frame shown in the
  unchanged editor via the shm ring (`08:phase-3`).
- A written sign-off records each check's result; the phase is `COMPLETED` only when **every** check is
  green. Any red check blocks the phase and names the area phase to reopen.
