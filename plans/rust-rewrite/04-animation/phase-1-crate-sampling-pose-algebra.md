# Phase 1 — `saffron-animation` scaffold: sampling + pose algebra

**Status:** COMPLETED

**Depends on:** 00-foundations:phase-2-core-crate, 02-math-and-geometry (`AnimClip`/`AnimTrack`/`.sanim`), 03-ecs-and-scene (component crate exists)

## Goal

Create the `saffron-animation` crate and port the pure, dependency-light core: the pose types
(`JointPose`, `PoseBuffer`, `PoseDelta`, `TwoBoneIkResult`), the track/clip samplers (`sample_track`,
`sample_clip`), and the pose-algebra helpers (`pose_diff`, `apply_delta`, the joint blend, the
cross-fade/inertialization weight curves). No runtime, no IK solver body, no scene access yet — this
phase is the deterministic math foundation everything else builds on, and the place the glam quat-order
simplification lands.

## Why this shape (NO LEGACY)

- **glam deletes the wxyz quat hazard, so the `asQuat`/`fromQuat` reorder helpers do not survive.** The
  C++ `sampleTrack` returns a `glm::vec4` in xyzw and `asQuat`/`fromQuat` swizzle it into glm's wxyz
  `glm::quat`. glam's `Vec4` *and* `Quat` are both xyzw, so `sample_track` returns a `Vec4` whose four
  lanes are already the quaternion — `Quat::from_vec4(v)` with no reorder. We do not port the swizzle
  helpers "to stay close to C++"; they are noise the type system removes (`conventions.md` naming +
  the glam pin).
- **The samplers are infallible pure functions, not `Result`.** `sample_track`/`sample_clip` cannot
  fail — an empty track yields a path-appropriate identity. They return values, never `Result`. The
  crate still defines its own `thiserror` `Error` + `Result<T>` alias (the only fallible surface is the
  clip loader in phase 4), but the math surface is plain functions (`conventions.md` error model:
  `?` is the check, and there is nothing to check here).
- **Clip types are imported from `saffron-geometry`, not redefined.** `AnimClip`/`AnimTrack` and the
  `Path`/`Interp` enums are the `.sanim` byte-format types; they live in geometry (one code path, no
  duplicate). Animation `use`s them. `match track.path { Path::Translation => …, … }` replaces the
  C++ `switch`.
- **`smoothstep01` and `quintic_decay` are exact-formula ports.** These are the C¹ cross-fade alpha and
  the C² zero-jerk inertialization decay; their exact polynomials are load-bearing (the transition
  tests in phase 5 assert C0 continuity at the switch). Ported coefficient-for-coefficient, not
  "approximately a smoothstep."
- **`CubicSpline` offset arithmetic is preserved verbatim.** The `[in-tangent, value, out-tangent]`
  3×-stride layout and the `valueOffset`/Hermite-basis math fail silently if an index is off by a
  stride, so it ports literally with the same `dt`-scaled tangents.

## Grounding (real files/symbols)

- `engine-old/source/saffron/animation/animation.cppm`
  - `JointPose` (`:25`, T/R/S, quat identity `1,0,0,0` → `Quat::IDENTITY`), `PoseBuffer` (`:36`,
    `local`/`override_`/`weight`), `PoseDelta` (`:47`), `TwoBoneIkResult` (`:61`).
  - `poseDiff`/`applyDelta` decls (`:55`,`:57`); `sampleTrack`/`sampleClip` decls (`:80`,`:85`).
- `engine-old/source/saffron/animation/animation.cpp`
  - `asQuat`/`fromQuat` (`:30`,`:35`) — **collapse**, see above.
  - `sampleTrack` (`:352`): stride/cc by path, empty-track identities (`:364`), `valueOffset`
    CubicSpline 3×-stride (`:379`), `readVec4` (`:387`), `finish` normalize-quat (`:396`), the
    `upper_bound`/local-param interval (`:414`), STEP/LINEAR/CubicSpline branches (`:424`,`:429`,`:440`).
  - `sampleClip` (`:453`): per-track `switch(track.path)` write into `out.local`.
  - `poseDiff` (`:482`, `from*inverse(to)`, scale ratio with `1e-6` floor), `applyDelta` (`:491`,
    slerp-from-identity, `pow(scale, weight)`).
  - `blendJoint` (`:180`, lerp T/S + slerp R), `smoothstep01` (`:190`), `quinticDecay` (`:198`).
- `engine-old/source/saffron/geometry/geometry.cppm`: `AnimTrack` (`:79`) with `Path`/`Interp` enums
  (`:85`,`:91`) + `times`/`values` (`:97`,`:98`), `AnimClip` (`:105`).
- `conventions.md` §2 (sum types → `enum` + `match`), the glam pin (xyzw quat), §8 (no self-tests).

## Acceptance gate

- `engine/crates/animation/Cargo.toml` exists; `saffron-animation` declares deps on `saffron-core`,
  `saffron-geometry`, `saffron-scene` and builds: `cargo build -p saffron-animation` green, full
  workspace build green.
- Crate root `#![deny(unsafe_code)]`; `cargo clippy -p saffron-animation` and `cargo fmt --check` clean.
- `cargo test -p saffron-animation` passes the phase-1 unit tests (the sampling/pose-algebra slice of
  the ported oracle, see phase 3 for the IK slice):
  - **LINEAR translation** — endpoints exact, midpoint `(5,0,0)` at `t=1`, clamp below/above the ends.
  - **STEP scale** — holds key0 until the next key's exact time.
  - **CubicSpline translation** — endpoints exact; asymmetric tangents bend the midpoint to `0.75`
    (distinct from the linear `0.5`), proving the Hermite path runs.
  - **LINEAR rotation = slerp** — `0°→90°` about Y, midpoint exactly `45°` (compared with the
    double-cover-safe `|dot| > 1 - 1e-4`).
  - **`sample_clip`** — joint 0 gets cubic T / slerp R / step S; an untracked joint keeps its
    pre-filled rest value.
  - **`pose_diff`/`apply_delta`** — `apply_delta(to, pose_diff(from,to), 1)` ≈ `from`; weight `0` is
    the base.
