# Phase 5 — The validation-layer-clean standing gate

**Status:** NOT STARTED

**Depends on:** 06-rendering:phase-1-device-swapchain-bringup, 08-host-and-viewport:phase-4-host-crate-lifecycle-wiring

## Goal

Make "the Vulkan validation layers report zero errors" a first-class, continuously-green deliverable
rather than an incidental property. This gate detects the entire class of GPU-state bugs that never
throw and never corrupt a wire byte — a wrong barrier, a layout mismatch, an MSAA sample-count
regression — and is therefore the only automated detector for the render subsystem's silent failures.
It is woven into every render-touching e2e test and into the present-only smoke run.

Concretely:

- **The engine runs with validation layers enabled in debug** and routes every validation message
  through its debug messenger to stdout/stderr in the exact form the harness greps:
  `[saffron:vulkan] error: [validation] …`. This output contract is what `harness.ts:57` matches; the
  Rust host must reproduce it byte-for-byte (it is the `08-host-and-viewport`/`06-rendering`
  debug-utils messenger wiring, asserted here).
- **Every render-exercising e2e test asserts `validationErrors() == []`** (the tier-1 discipline in
  `tests/e2e/AGENTS.md`). This phase makes that assertion mandatory in the coverage map: a feature phase
  that adds a render path also adds the e2e test that asserts the log stays clean while exercising it.
- **The present-only smoke run greps the log for validation errors** and fails the gate on any hit. This
  is the `tools/ci/check.sh` "engine present-only smoke" step (`check.sh:34`) plus a log grep, carried
  into the justfile reproducible gate (`01-build-and-toolchain` phase 6 / `13` phase 9).
- **The gate is operated from frame one** — the walking-skeleton milestone (a blank shm frame the editor
  shows) must already be validation-clean, and it never regresses as later render phases land. Each
  render phase's acceptance gate cites this gate.

## Why this shape (NO LEGACY)

- **The C++ engine already ran validation-on and the harness already asserts on it** — this is not a new
  capability, it is promoting an existing discipline to a named, enforced gate. The feasibility study
  names "the existing validation-layer-clean log" as one of the three first-class deliverables that are
  "the only automated detectors for the entire silent-failure class." The C++ relied on developers
  remembering to look; the Rust gate makes a dirty log a test failure.
- **The output string is a frozen contract, like the wire.** `harness.ts` and the smoke grep both match
  the literal `[saffron:vulkan] error: [validation]` prefix. Changing the prefix would silently disable
  the gate (every test would see an empty `validationErrors()` and pass). So the prefix is pinned and
  the Rust messenger reproduces it exactly — there is one validation-output format, matched in one place.
- **Clean-from-frame-one, not clean-at-the-end.** Pre-plan §2's walking-skeleton milestone requires the
  earliest runnable spine to be validation-clean; deferring validation cleanliness to "after rendering
  is done" would let a barrier bug accumulate undetected across a dozen phases. Each render phase keeps
  the log clean as it lands.

## Grounding (real files/symbols)

- `tests/e2e/harness.ts` — `validationErrors()` (`:54`) and its exact filter
  `line.includes("[saffron:vulkan] error: [validation]")` (`:57`): the frozen output contract the Rust
  debug messenger must reproduce.
- `tests/e2e/AGENTS.md` — the tier-1 convention "Assert on `validationErrors()` … assert the log stays
  free of `[saffron:vulkan] error: [validation]` lines — that is what catches GPU-state bugs (e.g. the
  MSAA sample-count regression) headlessly."
- `tools/ci/check.sh` — the "engine present-only smoke (bounded, headless)" step (`:34`,
  `SAFFRON_EXIT_AFTER_FRAMES=5`): the smoke run this gate greps.
- The debug-utils messenger wiring: `engine-old/source/saffron/rendering/` instance/debug-messenger
  setup (the `[saffron:vulkan]` log routing) — ported by `06-rendering` phase 1, asserted here.

## Acceptance gate

- The Rust engine emits validation messages in the exact `[saffron:vulkan] error: [validation]` form;
  an e2e test that deliberately triggers a known-clean render path observes `validationErrors() == []`.
- The present-only smoke run (`SAFFRON_EXIT_AFTER_FRAMES=5`) on the Rust binary produces a log with zero
  `[saffron:vulkan] error: [validation]` lines, and the reproducible gate (phase 9) fails on any hit.
- A regression probe is in place: a test that asserts the grep *would* catch a planted validation error
  (verifying the detector is wired, not silently disabled).
- The walking-skeleton frame is validation-clean; the Cargo workspace compiles; `cargo test --workspace`
  green; clippy + fmt clean.
