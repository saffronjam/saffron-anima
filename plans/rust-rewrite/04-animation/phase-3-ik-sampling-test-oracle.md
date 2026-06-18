# Phase 3 — The IK + sampling test oracle (the ported self-test)

**Status:** COMPLETED

**Depends on:** 04-animation:phase-2-two-bone-ik

## Goal

Port the ~430-line C++ `runAnimationSelfTest` **into Rust `#[test]` units** — the explicit charter of
this area. The self-test is the project's accumulated oracle for the pose math and the two-bone IK; it
is deleted as a runtime function and re-expressed as deterministic, named unit tests over the pure
functions ported in phases 1–2. This phase covers the math + IK slices (the runtime-driven slices —
preview/transition/loop-wrap — move to phase 5 once `tick_animation` exists). No production code in
this phase; it is purely the test oracle that locks fidelity.

## Why this shape (NO LEGACY)

- **No in-engine self-test survives (`conventions.md` §8).** The C++ engine ran `runAnimationSelfTest`
  at host startup (`host.cppm:1325`) and returned `Result<void>` accumulating `failures`. That whole
  mechanism is deleted: there is no `run_animation_self_test` symbol, nothing runs it at boot, and the
  `Err(... N check(s) failed)` aggregation pattern is replaced by ordinary `assert!`/`assert_close`
  in `#[cfg(test)] mod tests`, where each check is an independently-reported test.
- **The oracle's numeric tolerances are kept exactly.** The C++ used `eps = 1e-4` for most checks,
  `1e-3` for IK reach, `1e-2` for the over-reach clamp, and a quaternion double-cover comparator
  `|dot(a,b)| > 1 - 1e-4`. These tolerances are the contract — they distinguish a correct slerp
  midpoint from a lerp one, and a reached IK target from a near-miss. We port a `quat_close` helper and
  an `assert_close` for vectors with the same epsilons.
- **The IK oracle is the load-bearing part of the whole area.** The three IK cases pin
  `solve_two_bone_ik`'s correctness by *composing the returned world deltas back onto the chain* and
  checking the end effector lands on the target — exactly the C++ `solvedEnd` composition. We keep all
  three (in-range exact, pre-bent chain exact, over-reach clamped + aimed, no NaN) and **add** two new
  cases the C++ exercised only indirectly: the `rotation_between` antiparallel 180° case, and a
  pole-twist case where the desired pole flips the knee to the opposite side (asserting the chosen
  signed-`atan2` axis keeps the end on target rather than flipping it off).
- **`.sanim` round-trip is NOT re-tested here.** The C++ self-test included a `.sanim` save/load
  round-trip; that belongs to `02-math-and-geometry` (it owns the format) and is its acceptance gate,
  not animation's. Animation does not duplicate it (one code path, one owner).

## Grounding (real files/symbols)

- `engine-old/source/saffron/animation/animation.cpp` — `runAnimationSelfTest` (`:766`):
  - the `expect`/`quatClose`/`eps` harness (`:769`–`:782`) → `assert_close`/`quat_close` test helpers.
  - LINEAR translation block (`:784`–`:799`); STEP scale (`:801`–`:810`); CubicSpline translation
    midpoint-`0.75` (`:812`–`:827`); LINEAR-rotation-slerp `45°` midpoint (`:829`–`:843`);
    `sampleClip` integration + untracked-joint-keeps-rest (`:845`–`:892`).
  - `poseDiff`/`applyDelta` weight-0/weight-1 block (`:1017`–`:1033`).
  - Two-bone IK block (`:1135`–`:1188`): `solvedEnd` composition lambda (`:1147`), in-range exact
    (`:1154`), pre-bent chain exact (`:1164`), over-reach clamped+aimed+no-NaN (`:1176`).
- `engine-old/source/saffron/host/host.cppm` — `runAnimationSelfTest` boot call (`:1325`) that is
  **deleted** (no startup self-test in the Rust host; `conventions.md` §8).

## Acceptance gate

- `cargo test -p saffron-animation` passes the full math + IK oracle, each as a named `#[test]`:
  - the six sampling/pose-algebra checks from phase 1 (re-homed here as the canonical oracle if not
    already present), plus
  - **`ik_in_range_reaches_exactly`** — end lands within `1e-3` of an in-range target, no NaN.
  - **`ik_pre_bent_chain_reaches`** — a chain whose mid is already off-axis still reaches within `1e-3`.
  - **`ik_over_reach_clamps`** — `|reached-root|` within `1e-2` of `upper+lower`, aimed at the target,
    no NaN.
  - **`rotation_between_antiparallel`** (new) — `from = +X`, `to = -X` produces a 180° rotation taking
    `from` onto `to` (`distance(q*from, to) < 1e-3`).
  - **`ik_pole_flips_knee_not_chain`** (new) — flipping the pole to the opposite side moves the knee to
    that side while the end stays on the target (within `1e-3`), proving the signed-`atan2` twist axis.
- No `run_animation_self_test` (or any startup self-test) symbol exists anywhere in the workspace
  (`! grep -rn "self_test" engine/crates/animation` finds nothing outside `#[cfg(test)]`).
- Workspace build green; `cargo clippy` + `cargo fmt --check` clean.
