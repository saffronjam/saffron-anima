# Phase 2 — Two-bone IK solver

**Status:** COMPLETED

**Depends on:** 04-animation:phase-1-crate-sampling-pose-algebra

## Goal

Port `solve_two_bone_ik` and its two helpers (`rotation_between`, the inline `angle_opposite`) — the
numerically delicate heart of the area. Pure functions, no scene access. This is the law-of-cosines
two-bone solve that returns world-space delta rotations for the upper and lower joints, with a signed
`atan2` pole twist. The exact epsilon recipe must port byte-for-byte, because every branch fails
silently (a chain that flips off-target, a NaN from an out-of-domain `acos`).

## Why this shape (NO LEGACY)

- **One solver, ported exactly — not "a cleaner re-derivation."** The C++ recipe is the contract: a
  cleaner-looking re-derivation risks a different pole-flip or clamp behaviour that the editor's foot-IK
  e2e would catch only as visual instability. We port the five steps verbatim (reach clamp, bend axis
  with fallbacks, ±bend-sign disambiguation, swing, signed-`atan2` pole twist) and pin them with the
  oracle in phase 3.
- **The pole twist is a signed `atan2` about `root→target`, NOT a shortest-arc.** This is the single
  most important fidelity point. Both the current and desired pole directions are perpendicular to the
  target axis; a shortest-arc rotation between them picks an arbitrary axis when they are anti-aligned
  and flips the whole chain off the target. The port keeps
  `atan2(dot(cross(cp, dp), targetDir), dot(cp, dp))` about `targetDir` (`animation.cpp:596`). A comment
  states *why* (anti-aligned poles), per the `conventions.md` "why if non-obvious" rule — without the
  change-journey phrasing.
- **`rotation_between` keeps its antiparallel + degenerate fallbacks.** Shortest-arc rotation taking
  unit `from` onto unit `to`, with: identity for near-zero inputs, identity for `dot > 1 - 1e-6`, and a
  stable perpendicular-axis 180° flip for `dot < -1 + 1e-6` (`X`-cross-`from`, falling back to
  `Y`-cross-`from`). These edge axes are reached on real chains and must port exactly.
- **The bend-sign disambiguation is preserved.** Rotating the lower bone by `±bendDelta` both yield a
  valid chain; the port keeps the "pick the sign that lands `|start-end|` on the clamped reach within
  `1e-3`" check (`animation.cpp:563`) rather than introducing a separate orientation argument.
- **glam quat ops map 1:1.** `glm::angleAxis(angle, axis)` → `Quat::from_axis_angle(axis, angle)`
  (note the **argument order swap** — glam takes axis first); `glm::cross`/`glm::dot`/`glm::normalize`
  → glam `Vec3` methods; `glm::clamp` → `f32::clamp`/`.clamp`. All quats normalized at the same points.
- **Returns `TwoBoneIkResult` by value, no `Result`.** The function is total over its domain (the
  reach clamp + `max(len, 1e-6)` floors keep every `acos`/division valid), so it cannot fail; it
  returns the two delta rotations directly. A `reach < 1e-6` (target on the root) returns identity
  rotations (`out` default), matching C++.

## Grounding (real files/symbols)

- `engine-old/source/saffron/animation/animation.cpp`
  - `rotationBetween` (`:150`): length guards (`:154`), near-parallel identity (`:162`), antiparallel
    perpendicular-axis 180° (`:165`–`:174`), general `angleAxis(acos(d), normalize(cross))` (`:175`).
  - `solveTwoBoneIk` (`:501`): `a`/`b` floors `max(len,1e-6)` (`:510`); `reach`/`reachClamped` into
    `[|a-b|+1e-4, a+b-1e-4]` (`:514`,`:520`); `startMid`/`startEnd`/`lenStartEnd` (`:522`);
    `angleOpposite` law-of-cosines lambda (`:526`); bend axis `cross(startMid,startEnd)` with
    `pole`/`+Z` fallbacks (`:534`–`:547`); `currentMidAngle` (pi for folded chain) / `targetMidAngle` /
    `bendDelta` (`:552`–`:558`); ±bend-sign pick by reach match (`:563`–`:569`); `swing =
    rotationBetween(startEndBent, toTarget)` (`:574`); the pole twist — `currentPole`/`desiredPole`
    projected off `targetDir`, the `0.02*poleScale` straight-chain skip, the signed `atan2` about
    `targetDir` (`:581`–`:599`).
  - `TwoBoneIkResult` (`animation.cppm:61`): `upper`/`lower` quats.
- `conventions.md` (glam pin — quat axis-angle arg order; the "why if non-obvious" comment rule).

## Acceptance gate

- `cargo build -p saffron-animation` and the workspace build are green; crate root
  `#![deny(unsafe_code)]`; `cargo clippy` + `cargo fmt --check` clean.
- `cargo test -p saffron-animation` continues to pass phase-1 tests; the IK behavioural tests land in
  phase 3 (this phase adds `solve_two_bone_ik` + `rotation_between` with at least a smoke `#[test]`
  asserting an in-range solve produces no NaN, so the function is exercised by `cargo test`).
- No new public API beyond `solve_two_bone_ik` (+ `TwoBoneIkResult`, already in phase 1); the helpers
  stay private to the crate.
