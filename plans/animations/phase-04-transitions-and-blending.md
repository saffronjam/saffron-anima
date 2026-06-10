# Phase 4 — transitions: cross-fade + inertialization

**Status:** COMPLETED

## Goal

Make clip-to-clip changes smooth instead of popping. Add **cross-fade** (blend two clips' poses by an
alpha that ramps over a transition duration) as the mechanism, and **inertialization** (Gears of War,
GDC 2018) as the default transition *policy* — a single-clip evaluation plus a quintic decay of the pose
*offset* captured at the switch. Inertialization is the strategic choice because it is roughly half the
runtime cost of a sustained two-clip blend **and reuses the exact per-joint pose-diff machinery the
physics handoff (Phase 13 / ragdoll) needs** — building it here means that machinery exists before it's
load-bearing.

## What exists to build on

- Phase 1's `PoseBuffer` (`local`/`override_`/`weight`) and `JointPose` + the `blendJoint` (lerp T/S,
  slerp R) helper from Phase 3's blend layer.
- Phase 3's evaluator `tickAnimation` and the `AnimationPlayerComponent` transition fields already declared
  (`prevClip`, `transition`, `transitionDuration`).
- Phase 3 already loads + caches clips and samples into a `PoseBuffer`.

## Work

### 1. Cross-fade (the building block)

Extend the evaluator: when a transition is active (`transition < transitionDuration`), sample **both**
`prevClip` (at its frozen-or-advancing time) and `clip` (at `time`) into two pose buffers and blend
per joint with `alpha = transition / transitionDuration` (smoothstep the alpha for a softer ramp):

```cpp
JointPose out = blendJoint(prevPose[i], curPose[i], smoothstep(alpha));
```

Advance `transition += dt`; when it reaches `transitionDuration`, drop `prevClip` and the second sample.
A new transition is started by an API/command (Phase 5 `play-animation` with a `--blend <seconds>` arg):
set `prevClip = clip`, `clip = next`, `transition = 0`, `transitionDuration = blend`.

### 2. Inertialization (the default policy)

Instead of sustaining a two-clip blend, capture the **pose offset** between the outgoing pose and the
incoming clip's pose at the switch instant, then evaluate only the incoming clip and **decay the offset to
zero** over the transition with a quintic (C²-continuous, zero-jerk) curve:

```cpp
// At switch: offset[i] = poseDiff(outgoingPose[i], incomingPose0[i])   // per joint: ΔT, Δrot (as quat), ΔS
// Each frame: x = clamp(transition / transitionDuration, 0, 1)
//             k = quinticDecay(x)            // 1 → 0, with zero value+slope+accel at x=1
//             final[i] = applyOffset(incomingPose[i], scale(offset[i], k))
```

`poseDiff`/`applyOffset` are the same delta-pose operations a ragdoll uses to nudge an animated target —
factor them into `Saffron.Animation` as reusable helpers (`PoseDelta`), not inline. Add a per-entity
small offset buffer (transient, owned by the evaluator keyed on entity, like the clip cache).

Make inertialization the default; keep cross-fade behind a flag/policy enum on the transition so both are
available (cross-fade is simpler to reason about for debugging and is the obvious fallback if a quintic
artifact shows up).

### 3. Wrap-around looping is a transition too

A `Loop` clip wrapping from `duration` back to `0` can pop if the start/end poses differ. Apply a short
inertialized transition (or a configurable loop-blend window) at the wrap so looped locomotion is seamless
— a small, high-value detail UE/Unity both do.

## Validation (done criteria)

- `make engine` + `make prepare-for-commit` clean.
- Headless: trigger a transition between two clips on the fixture; assert the blended bone pose at
  `alpha≈0` matches the outgoing clip, at `alpha≈1` matches the incoming clip, and that the inertialized
  path is C¹ (no first-derivative jump) across the switch frame within epsilon.
- Manual: a looping clip no longer visibly pops at the wrap.
- `docs/`: extend `playback-runtime.md` with a "transitions" section (cross-fade vs inertialization, why
  inertialization, the shared `PoseDelta` machinery).

## Notes / gotchas

- This phase is **optional for a first visible slice** — Phase 3 already plays + loops. It is included
  because the user chose the thorough path and because the `PoseDelta` helpers are the same ones Phase 13
  and the eventual ragdoll need; building them under animation (where they're easy to test) beats
  reinventing them under physics.
- Keep the transition state on the **component** (serialized as 0/idle by default) so it round-trips
  harmlessly; active-transition state is runtime-only and lives on the play duplicate.
- Quaternion offsets: compute `Δrot = incoming * inverse(outgoing)` and decay via
  `slerp(identity, Δrot, k)` — never lerp raw quat components for the offset.
