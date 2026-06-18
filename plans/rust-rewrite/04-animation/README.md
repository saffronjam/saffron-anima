# 04 — Animation: pose math, the player runtime, IK, and the skinning hand-off

`saffron-animation` is the easiest meaningful subsystem to port and the cleanest fit for idiomatic
Rust: it is **pure CPU pose math with zero FFI**. Two C++ files (`animation.cppm` interface + a single
`animation.cpp` implementation, ~1.3k LOC together) hold everything — track sampling, clip evaluation,
pose blending, a two-bone IK solver, the per-rig player runtime with transitions/loop-blend, and a
foot-IK producer. There is no GPU concept here at all: the crate's *only* output is a
`PoseOverrideComponent` written onto each driven bone in `saffron-scene`'s world, and its *only*
interface to rendering is that scene composes those overrides into world matrices, from which
rendering builds its joint palette.

Three things shape this area:

1. **Dependency order.** Animation reads `saffron-geometry`'s clip types (`AnimClip`/`AnimTrack` +
   the `.sanim` reader) and `saffron-scene`'s components and world-transform helpers. So this area
   depends on `02-math-and-geometry` and `03-ecs-and-scene` being in place. It depends on neither
   rendering nor physics (the hand-offs are one-directional and via scene components).
2. **The two-bone IK is the only delicate part.** `solveTwoBoneIk` is a law-of-cosines solve with a
   signed-`atan2` pole twist and a thicket of epsilons (antiparallel fallback, bend-sign disambiguation,
   straight-chain pole skip). glam's `Quat::from_xyzw` *deletes* the worst glm hazard (the wxyz quat
   memory order), but the numerical recipe must port byte-for-byte. The ~430-line C++
   `runAnimationSelfTest` is the oracle: it is **deleted as a runtime function** and re-expressed as
   `#[test]` units that pin the exact endpoints, midpoints, and reach/clamp behaviour.
3. **`PoseBuffer` and the blend layer are deliberately ahead of their consumers.** `PoseBuffer.override_`
   / `weight` and the `PoseDelta` machinery exist so a physics handoff (ragdoll) has a tested target to
   write into; v1 leaves `weight` zero (pure animation). We port the machinery and its tests now;
   `05-physics-jolt-bridge` wires the ragdoll producer onto it later.

`ozz-animation-rs` is **rejected** (feasibility §3): it would impose a second clip format and a
jobs/SoA re-architecture, and it still lacks foot IK. We port the existing math directly onto glam.

---

## 1. What the crate owns (and what it does not)

`saffron-animation` owns the pure functions and the per-session runtime:

- **Sampling:** `sample_track` (one channel at time `t`), `sample_clip` (a whole clip into a
  `PoseBuffer`), and the index-or-name track-binding variant used by the runtime.
- **Pose algebra:** `JointPose`, `PoseBuffer`, `PoseDelta`, `pose_diff`, `apply_delta`, joint blend,
  and the cross-fade/inertialization weight curves.
- **IK:** `solve_two_bone_ik` (pure, returns world-space delta rotations) plus the `apply_foot_ik`
  producer that resolves a chain by forward kinematics from the sampled pose and writes the result
  back into the per-frame local poses.
- **Runtime:** `AnimationRuntime` (the per-session clip cache + transition/last-pose state) and
  `tick_animation`, which samples and advances every rig, applies transitions and foot-IK, and writes
  a `PoseOverrideComponent` onto each driven bone (and removes it from inactive rigs).

It does **not** own:

- The clip *types* or the `.sanim` byte format — those are `saffron-geometry` (`AnimClip`, `AnimTrack`,
  `load_animation_from_bytes`). Animation consumes them.
- The scene *components* — `SkinnedMeshComponent`, `AnimationPlayerComponent`, `PoseOverrideComponent`,
  `FootIkComponent`/`FootChain` live in `saffron-scene`. Animation reads and writes them.
- The clip *loader* — clip bytes live in a `.smodel` SANM chunk whose reader is in `saffron-assets`,
  so `AnimationRuntime` holds an injected loader closure (`clip_loader`), installed by the host. The
  C++ free-function `loadAnimationClipAsset` (`assets.cppm`) is what the host wires into it.
- Any GPU concept — `jointMatrices` (the palette builder) lives in `saffron-scene`; the compute-skinning
  prepass, skinned-BLAS refit, and motion vectors all live in `saffron-rendering`. Animation's
  contribution to those is *only* the `PoseOverrideComponent` it writes (see §6).

---

## 2. Idiom translation for this area

Per the PP-1 foundations contract (`00-foundations/conventions.md`), applied at each site:

- **`std::function<Result<AnimClip>(Uuid)> clipLoader` → `Box<dyn Fn(Uuid) -> Result<AnimClip>>`** held
  as an `Option` on `AnimationRuntime` (unset ⇒ no clip loads, matching the C++ null-function guard).
  This is a single injected dependency, not an extensible table, so a boxed `Fn` is the right shape —
  not a trait. It is single-threaded (`tick_animation` runs on the main thread), so no `Send` bound.
- **The clip cache is a negative cache.** A broken asset is cached as an empty clip so it is not
  re-read every frame. In Rust this is `HashMap<u64, AnimClip>` where a failed load inserts a
  `AnimClip::default()` — the same shape as the C++ `clipCache.emplace(value, AnimClip{})`. (We do not
  use `Option<Arc>` here because the cached clip is owned by the runtime and read by value into a
  per-frame `PoseBuffer`, not shared.)
- **`AnimTrack::Path` / `AnimTrack::Interp` `enum class` → data-less Rust `enum` + `match`.** These
  are defined in `saffron-geometry` (they are clip-format types); animation matches on them. The
  switch-on-path in `sample_clip` becomes a `match`.
- **`AnimMode { Edit, Play }` → a Rust `enum`** in `saffron-animation` (it is an evaluator concept,
  not a clip-format one). `AnimationPlayerComponent::Wrap` / `::Transition` stay in `saffron-scene`
  with the component.
- **Errors.** The only fallible surface is `clip_loader` returning a geometry/assets error; it is
  swallowed into a `log_warn` + negative-cache exactly as the C++ does (`tick_animation` never fails).
  The crate's own `Error`/`Result` alias (thiserror) exists for symmetry but the public tick/sample
  functions are infallible — they return poses, not `Result`. `run_animation_self_test`'s `Result<void>`
  is **deleted**; its body becomes `#[test]` assertions (`conventions.md` §8).
- **No `Ref`/`Arc` sites.** Everything here is owned-by-value or borrowed `&mut Scene`. There is no
  shared-mutable state and nothing crosses a thread, so this area introduces no `Arc`/`Mutex`/`RefCell`.
- **glam quat order.** Every `glm::quat(w, x, y, z)` literal becomes `Quat::from_xyzw(x, y, z, w)` or
  `Quat::IDENTITY`; the `asQuat`/`fromQuat` helpers (which reorder a `vec4` xyzw ↔ glm wxyz) collapse,
  because glam's `Vec4` and `Quat` are *both* xyzw — `sample_track` can return a `Vec4` whose `.xyzw`
  is already the quaternion. This is the single biggest simplification the port gets for free.

---

## 3. Sampling and pose algebra (the pure core)

`sample_track` is a faithful 1:1 port. The shape:

- 3 components for Translation/Scale, 4 for Rotation; the stride drives all offset arithmetic.
- Empty track ⇒ a path-appropriate identity (`(0,0,0,1)` quat, `(1,1,1)` scale, `0` translation).
- CubicSpline stores `[in-tangent, value, out-tangent]` per key (3× stride); STEP/LINEAR store the
  value flat. The `value_offset` helper encodes this.
- `t` clamps to `[first, last]` (no extrapolation); the interval is found by `upper_bound` (Rust
  `partition_point`), the local parameter is `(t - t0) / dt`.
- STEP holds the previous key; LINEAR lerps (slerp for rotation, **normalized**); CubicSpline is a
  Hermite spline with tangents scaled by `dt`.
- Rotation results are normalized before return (`finish`).

`sample_clip` walks tracks, samples each, and writes `translation`/`rotation`/`scale` by `match track.path`
into pre-filled rest poses. The runtime uses a name-resolving variant (`sample_clip_resolved`) that
re-binds a stale index by the durable `joint_name` — ported as-is, since the durable-name binding is
load-bearing across reimport.

Pose algebra: `pose_diff(from, to)` builds the delta (additive T, `from * inverse(to)` quat, S ratio);
`apply_delta(base, delta, weight)` re-applies it weighted (slerp from identity for the quat, `pow` for
scale). The `blend_joint` cross-fade (lerp T/S, slerp R), `smoothstep01` (C¹ cross-fade alpha), and
`quintic_decay` (C² zero-jerk inertialization weight) are direct ports.

---

## 4. Two-bone IK (the delicate part)

`solve_two_bone_ik(root, mid, end, target, pole, upper_len, lower_len) -> TwoBoneIkResult` returns two
**world-space delta rotations** (`upper`, `lower`); the caller composes them onto the chain's current
world rotations, then strips parent world rotation to land in local space. The recipe, ported exactly:

1. Clamp the reach into `[|a-b| + ε, a+b - ε]` so each `acos` stays valid (graceful over/under-reach).
2. Build the bend axis as `cross(startMid, startEnd)`, falling back to `cross(startMid, pole)`, then a
   `+Z` seed, when the chain is straight.
3. Knee bend: change the interior mid angle from its current value to the reach value via the law of
   cosines (`angle_opposite`); rotate the lower bone about the bend axis by ±`bendDelta`, picking the
   **sign that lands `|start-end|` on the clamped reach** (disambiguates the cross-product axis sign).
4. Swing the whole chain about the root so the bent `start→end` points at the target
   (`rotation_between`, with its own antiparallel-axis fallback).
5. Pole twist: a **signed `atan2` about the `root→target` axis** (NOT a shortest-arc between the
   projected pole directions — that would flip the chain off-target when the poles are anti-aligned),
   skipped on a near-straight chain where the pole plane is undefined.

This is the section most at risk of silent divergence, so its tests are the heart of the area
(phase 3). The IK oracle from `runAnimationSelfTest` pins: an in-range target reached *exactly*
(`< 1e-3`), a pre-bent chain reached exactly, and an over-reach clamped to a straight chain aimed at
the target with no NaN. We add the `rotation_between` antiparallel case and a pole-twist
direction case as new `#[test]`s, since those branches are exercised only indirectly in C++.

`apply_foot_ik` is the producer: for each enabled `FootChain` it resolves the chain's world transforms
**by forward kinematics from this frame's sampled `finalLocal`** (deliberately NOT the cached
`WorldTransformComponent`, which is last frame's post-IK result and would feed the solver its own
output), lifts the foot target up to `groundHeight`, solves, and writes the new local rotations back
into `finalLocal`. It never touches a bone's `TransformComponent`.

---

## 5. The player runtime (`tick_animation`)

`tick_animation(runtime, scene, dt, mode)` iterates every entity with both an
`AnimationPlayerComponent` and a `SkinnedMeshComponent` (`forEach` → the chosen ECS query) and, per rig:

- Decides `active` (Play animates all; Edit only a `preview_in_edit` rig), resolves the clip through
  the runtime cache/loader, and on no clip clears overrides + drops transition/last-pose state and
  returns.
- Seeds each bone's authored **rest** local TRS (Euler→quat via the scene's `transformMatrix`
  convention) so untracked joints/channels keep their authored value, and collects the
  name↔index maps for durable track resolution.
- Advances the playhead under the wrap mode (`advance_time`: Once clamps+stops, Loop wraps with
  `rem_euclid`, PingPong bounces), noting a Loop wrap so `loop_blend > 0` can seed an inertialization
  across the seam.
- Samples the clip into `final_local`, applies an in-flight transition (cross-fade via `blend_joint`
  + `smoothstep01`, or inertialization via `apply_delta` + `quintic_decay`), capturing the frozen
  outgoing pose + offset **once** at the switch frame into `TransitionState`.
- Runs `apply_foot_ik` if the rig has an enabled `FootIkComponent`.
- Writes a `PoseOverrideComponent` onto each driven bone (`emplace_or_replace` → the ECS insert), and
  snapshots `final_local` into `runtime.last_pose[key]` (the reserved physics-handoff finite-difference
  source — no consumer yet, kept for `05-physics`).

The transition/last-pose state is keyed by the entity's `IdComponent` uuid (`u64`), exactly as C++.

---

## 6. The skinning prepass interface (one-directional, via scene components)

Animation produces **no GPU data**. The contract to rendering is entirely mediated by scene components,
and this is the same byte-path the C++ engine uses:

1. `tick_animation` writes `PoseOverrideComponent` (local TRS) onto each driven bone.
2. `saffron-scene`'s `world_matrix`/`local_matrix` prefer the override over the bone's
   `TransformComponent` (`localMatrix`, `scene.cppm`), so `update_world_transforms` composes the
   animated pose into the cached world matrices.
3. `saffron-scene`'s `joint_matrices(scene, skin) -> Vec<Mat4>` builds `worldBone * inverseBind` per
   joint (`jointMatrices`, `scene.cppm`) — the joint palette.
4. `saffron-assets`' `render_scene` (the `renderScene` enqueue path) calls `joint_matrices` per skinned
   rig, appends the palette into a per-frame `frame_joints` buffer, and tags the `DrawItem` with
   `skinned`/`joint_offset`/`joint_count` (`assets.cppm`). `saffron-rendering`'s compute-skinning
   prepass blends that palette, feeds motion vectors (prev vs current palette), and refits the skinned
   BLAS.

So **this area's only deliverable toward skinning is correct `PoseOverrideComponent` data**. The
palette builder (`joint_matrices`) belongs to `03-ecs-and-scene`; the prepass/BLAS/motion-vector
machinery belongs to `06-rendering`. This README documents the seam so the rendering and scene phases
know exactly what animation guarantees: a per-frame, per-bone local TRS override that is non-destructive
(the rest-pose `TransformComponent` is never touched).

---

## 7. Phases

| Phase | Title | Depends on |
|-------|-------|-----------|
| 1 | Crate scaffold + sampling + pose algebra | 00-foundations, 02-math-and-geometry, 03-ecs-and-scene |
| 2 | Two-bone IK solver | 04-animation:phase-1 |
| 3 | IK + sampling test oracle (the ported self-test) | 04-animation:phase-2 |
| 4 | The player runtime: `tick_animation`, transitions, loop-blend, foot-IK | 04-animation:phase-1, :phase-2 |
| 5 | Runtime tests + the skinning-prepass seam contract test | 04-animation:phase-4, 06-rendering (seam doc only) |

---

## 8. Grounding (real files/symbols)

| What | File | Symbols |
|------|------|---------|
| Pose types + pure fn declarations | `engine-old/source/saffron/animation/animation.cppm` | `JointPose`, `PoseBuffer`, `PoseDelta`, `TwoBoneIkResult`, `poseDiff`, `applyDelta`, `solveTwoBoneIk`, `sampleTrack`, `sampleClip`, `AnimMode`, `TransitionState`, `AnimationRuntime`, `tickAnimation`, `runAnimationSelfTest` |
| Sampling + IK + runtime impl | `engine-old/source/saffron/animation/animation.cpp` | `asQuat`/`fromQuat`, `restPoseOf`, `clearOverrides`, `loadClip`, `advanceTime`, `rotationBetween`, `blendJoint`, `smoothstep01`, `quinticDecay`, `parentWorldRotation`, `applyFootIk`, `sampleClipResolved`, `sampleTrack`, `sampleClip`, `poseDiff`, `applyDelta`, `solveTwoBoneIk`, `tickAnimation`, `runAnimationSelfTest` |
| Clip types + `.sanim` reader (consumed) | `engine-old/source/saffron/geometry/geometry.cppm` | `AnimTrack`, `AnimTrack::Path`, `AnimTrack::Interp`, `AnimClip`, `saveAnimation`, `loadAnimation`, `loadAnimationFromBytes`, `saveAnimationToBuffer` |
| Scene components (read/written) | `engine-old/source/saffron/scene/scene.cppm` | `SkinnedMeshComponent`, `AnimationPlayerComponent` (`Wrap`, `Transition`), `PoseOverrideComponent`, `FootIkComponent`, `FootChain`, `RelationshipComponent`, `WorldTransformComponent`, `BoneComponent` |
| Scene helpers (FK + palette) | `engine-old/source/saffron/scene/scene.cppm` | `forEach`, `transformMatrix`, `localMatrix`, `worldMatrix`, `worldRotation`, `worldTranslation`, `updateWorldTransforms`, `jointMatrices`, `relinkHierarchy` |
| Clip loader wiring (host injects) | `engine-old/source/saffron/host/host.cppm`, `engine-old/source/saffron/assets/assets.cppm` | `AnimationRuntime animation`, `state->animation.clipLoader = ...`, `tickAnimation(...)` call site; `loadAnimationClipAsset` |
| Skinning-prepass seam (rendering consumes) | `engine-old/source/saffron/assets/assets.cppm` | `renderScene` skinned branch (`jointMatrices`, `frameJoints`, `DrawItem.skinned`/`jointOffset`/`jointCount`) |
