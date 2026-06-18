# Phase 1 — Test conventions and the per-area coverage map

**Status:** COMPLETED

**Depends on:** 00-foundations:phase-1-workspace-scaffold

## Goal

Lock the workspace-wide test conventions and the per-area unit-coverage map so every later feature
phase's acceptance gate cites a known discipline rather than inventing one. This phase writes no engine
code; it produces the testing rules that `00`–`12` reference and seeds the `dev-dependencies` and CI
test invocation. It is the test-architecture equivalent of `00-foundations/conventions.md` for the rest
of the codebase, and is intentionally the first thing this area lands (testing is woven in, not
trailing).

Concretely it locks:

- **Where tests live:** inline `#[cfg(test)] mod tests` for pure functions and type-local invariants;
  a crate's `tests/` directory for fixtures needing on-disk assets / cross-module composition / a built
  artifact; the `tests/e2e` harness for anything needing a running engine over the wire. One owner per
  behavior — never duplicate an e2e behavior as a unit test or vice versa.
- **The shared test helpers** that the ported oracles need (the `assert_close` / `quat_close`
  float comparators with the C++ epsilons; a `bytes_eq_golden` helper that the snapshot crate in
  phase 2 builds on). These live in a tiny `saffron-test-support` dev-only crate so the same comparator
  is used by animation, geometry, and physics tests rather than copy-pasted.
- **The per-area coverage map** (README §2 table) restated as the binding contract: each feature phase's
  gate must name its inline tests, and any byte-exact format it owns must have a golden fixture.
- **The single test entrypoint:** `cargo test --workspace` runs every unit + crate-`tests/` suite; the
  e2e suite is a separate invocation (`bun test` in `tests/e2e`, phase 3) and the standing gates are
  separate runnable checks (phases 5/6) — the orchestrator (phase 9) sequences all three.

## Why this shape (NO LEGACY)

- **The C++ had no unit-test framework at all** — its only CPU-logic verification was the
  `run*SelfTest` functions run at host startup under `SAFFRON_SELFTEST` (`host.cppm:1312`). Rust has
  `#[test]` natively, so the conventions establish the *replacement* discipline, not a port of a
  framework. There is one way to write a test (the three locations above), not a self-test function and
  an external check doing the same job.
- **A shared comparator crate, not per-test copies.** The C++ self-tests each re-defined their own
  `expect`/`eps`/`quatClose` lambdas (`animation.cpp:769`, `scene.cppm:1714`, etc.). Re-expressing
  every one with its own helper would re-create that duplication. One `saffron-test-support` crate holds
  the comparators with the C++ epsilons documented, used everywhere — one source for the tolerances that
  *are* the contract.
- **The coverage map is binding, not advisory.** Without it, "the gate names its tests" is
  unenforceable and a phase could ship a feature with no oracle. The map ties each area to the specific
  C++ self-test that is its oracle, so phase 8's removal ledger can verify a 1:1 mapping with no
  orphans.

## Grounding (real files/symbols)

- `engine-old/source/saffron/host/host.cppm` — the `SAFFRON_SELFTEST` startup block (`:1312`–`:1351`):
  the entire mechanism this convention replaces. Enumerates every self-test that must have a mapped
  `#[test]`/e2e owner.
- The per-test comparator lambdas to consolidate: `animation.cpp:769` (`expect`/`quatClose`/`eps`),
  `scene.cppm:1714` (serde diff), `geometry.cppm:2024`/`:2137` (container/pick checks).
- `plans/rust-rewrite/00-foundations/conventions.md` §8 (tests — no runtime self-tests): this phase
  extends that section into the full per-area policy and the `saffron-test-support` crate.

## Acceptance gate

- A `saffron-test-support` dev-only crate exists and compiles, exporting `assert_close`,
  `quat_close` (with `|dot| > 1 - 1e-4` double-cover), and a `golden` byte-compare helper, with the C++
  epsilons (`1e-4` general, `1e-3` IK reach, `1e-2` over-reach clamp) documented at the symbols.
- `cargo test --workspace` runs and is green (trivially, since only `saffron-test-support`'s own
  self-tests exist yet); `cargo clippy --workspace --all-targets` + `cargo fmt --check` clean.
- The coverage map (README §2) is reproduced as the binding policy and every C++ self-test in the
  ledger (phase 8) has a named owning area and target test location.
- The Cargo workspace compiles; no `*self_test*` symbol exists outside `#[cfg(test)]` (vacuously true
  here, asserted as a `grep` the orchestrator will enforce from phase 8 on).
