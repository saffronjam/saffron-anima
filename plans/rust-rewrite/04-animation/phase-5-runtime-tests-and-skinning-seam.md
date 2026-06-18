# Phase 5 — Runtime tests + the skinning-prepass seam contract

**Status:** COMPLETED

**Depends on:** 04-animation:phase-4-player-runtime, 06-rendering (seam documentation only — no code dependency)

## Goal

Port the runtime-driven slices of the C++ self-test into `#[test]` units over `tick_animation`, and
add the one cross-area contract test that locks the animation→rendering skinning seam: that a ticked
rig produces `PoseOverrideComponent` data which `saffron-scene`'s `joint_matrices` turns into the joint
palette the renderer expects. This closes the area: every behaviour the C++ self-test exercised has a
Rust test, and the seam to rendering is asserted rather than assumed.

## Why this shape (NO LEGACY)

- **The preview / transition / loop-wrap oracle becomes runtime `#[test]`s, built on a real `Scene`.**
  These C++ cases ran `tickAnimation` against a constructed `Scene` with a directly-seeded
  `clipCache` (bypassing the loader/disk). The Rust tests do the same with `saffron-scene`'s public
  entity/component API — no startup self-test, no `Result`-accumulator (`conventions.md` §8). Each
  scenario is one named test.
- **The seam is asserted through the *real* scene helpers, not a mock.** Animation's only output toward
  rendering is `PoseOverrideComponent`; rendering reads it transitively via `scene.joint_matrices`
  (`worldBone * inverseBind`). The contract test ticks a rig, calls `update_world_transforms` then
  `joint_matrices`, and asserts the palette reflects the animated pose (not the rest pose) — pinning
  that the override actually flows into the world composition. This is the byte-path
  `renderScene`/`jointMatrices` use (`assets.cppm`, `scene.cppm`), so the test is the contract the
  rendering skinning phase relies on. There is no GPU in this test; rendering's compute-skinning
  prepass is tested in `06-rendering`. The dependency on `06-rendering` is documentation-only: this
  phase needs the seam *doc* (README §6) frozen, not rendering code.
- **Non-destructiveness is a first-class assertion.** The preview test pins that the rest-pose
  `TransformComponent` stays untouched (Edit preview never dirties the project) and that clearing the
  preview reverts the bone to rest next tick — the exact C++ guarantees. This is the property that lets
  the editor scrub the timeline without mutating saved data.
- **Inertialization C0-at-the-switch is the no-pop guarantee, kept as a test.** The transition tests
  assert both modes start at the outgoing pose at the switch frame and end at the incoming clip, and
  that the loop-wrap blend holds the pre-wrap pose across the seam (a hard cut would jump ~72° — which
  `quat_close` rejects). These pin the `smoothstep01`/`quintic_decay`/`TransitionState` machinery from
  phases 1 and 4 end-to-end.

## Grounding (real files/symbols)

- `engine-old/source/saffron/animation/animation.cpp` — the runtime self-test slices:
  - preview evaluator block (`:942`–`:1015`): Edit-without-preview inert, Edit+preview writes the
    `45°` override, rest-pose `TransformComponent` stays at identity, world composition reflects the
    override, clearing preview reverts, Play animates without preview.
  - transition block (`:1035`–`:1091`): cross-fade starts at outgoing / ends at incoming;
    inertialization C0-at-switch / ends at incoming.
  - loop-wrap blend block (`:1093`–`:1133`): wrap frame holds the pre-wrap pose (no pop).
- `engine-old/source/saffron/scene/scene.cppm` — `updateWorldTransforms` (`:920`), `worldRotation`
  (`:904`), `jointMatrices` (`:957`, `worldMatrix * inverseBind`), `localMatrix` (`:858`, prefers the
  override).
- `engine-old/source/saffron/assets/assets.cppm` — `renderScene` skinned branch (`:5764`):
  `jointMatrices` per rig → `frameJoints` → `DrawItem.skinned`/`jointOffset`/`jointCount` — the
  consumer the seam test mirrors (README §6).

## Acceptance gate

- `cargo test -p saffron-animation` passes the full runtime oracle, each a named `#[test]`:
  - **`edit_without_preview_is_inert`** — no `PoseOverrideComponent` appears.
  - **`preview_writes_override_and_advances`** — playhead at `0.5`, the override holds the `45°` Y
    rotation, world rotation reflects it, the rest-pose `TransformComponent` is untouched.
  - **`clearing_preview_reverts_to_rest`** — the override is removed and the bone reverts next tick.
  - **`play_animates_without_preview`** — Play writes the override regardless of `preview_in_edit`.
  - **`crossfade_starts_outgoing_ends_incoming`** and **`inertialize_c0_at_switch`** — both transition
    modes, asserting the switch-frame and steady-state poses.
  - **`loop_wrap_holds_pre_wrap_pose`** — `loop_blend > 0` keeps the wrap frame at the pre-wrap pose.
- **`skinning_seam_palette_reflects_animation`** (cross-area contract) — tick a rig, run
  `update_world_transforms` + `joint_matrices`, assert the palette encodes the animated bone pose
  (distinct from the rest-pose palette by more than `1e-3`), confirming `PoseOverrideComponent` flows
  into the joint palette the renderer consumes.
- Workspace build green; `cargo clippy` + `cargo fmt --check` clean; crate root `#![deny(unsafe_code)]`.
- The `saffron-animation` area is complete: every behaviour `runAnimationSelfTest` covered (sampling,
  pose algebra, IK, preview, transitions, loop-wrap) has a Rust `#[test]`, and no startup self-test
  function exists.
