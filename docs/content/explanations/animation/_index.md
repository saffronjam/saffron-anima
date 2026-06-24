+++
title = 'Animation'
weight = 17
bookCollapseSection = true
+++

# Animation

Animation drives a scene over time from authored clips. A clip carries three kinds of channel on
**one** track model: *bone* tracks deform a rigged mesh's skeleton, *node-TRS* tracks move a plain
scene-graph entity's transform, and *morph-weight* tracks slide a mesh between stored blend shapes. All
three are sampled by the same evaluator at the current time and written into a runtime layer rather than
over the authored rest state.

Skeletal animation is the oldest of the three. The engine already skins — the glTF skin import builds
one entity per joint, tags each with a `Bone` component, and a vertex palette deforms the mesh every
frame — so a bone track supplies a new *source* for each joint's local transform: a clip, sampled at the
current time, written into a runtime pose rather than over the authored rest transforms. Node-TRS reuses
that same pose-override seam for plain entities; morph targets deform on the GPU before skinning.

The pose flows **sample → pose buffer → an (inert) per-bone blend layer → world-transform
composition**. The authored bone transforms keep the rest pose and are never overwritten, so
playback is non-destructive in both Edit and Play, and the blend layer is the seam every later
pose producer — foot IK, and the powered ragdoll in `saffron-physics` — plugs into without
touching the sampling path. The whole CPU pose core lives in the FFI-free `saffron-animation`
crate.

This section starts at the bottom: the pure data and math the rest of the system is built on.

## Pages

| Page | Covers | Code |
|---|---|---|
| `animation-data-model` | the clip/track keyframe model, the decomposed joint pose + blend layer, and clip sampling (Step/Linear/CubicSpline with slerp) | `geometry/src/types.rs`; `animation/src/pose.rs`; `animation/src/sample.rs` |
| `playback-runtime` | the per-frame evaluator: sample → pose → blend → pose override → world composition; non-destructive Edit preview vs Play; wrap modes | `animation/src/runtime.rs`; `scene/src/component.rs`; `host/src/layer.rs` |
| `skeleton-overlay` | the line-skeleton viewport overlay for the selected rig — bone segments, joint dots, optional RGB axes; on-top, Edit + Play; the `set-skeleton-overlay` toggle | `host/src/overlay.rs`; `sceneedit/src/overlay.rs`; `control/src/commands_animation.rs` |
| `timeline` | the editor Timeline panel — the clip bar with real per-channel keyframe ticks, ms ruler, a scrubbable playhead, Edit-preview transport; reads playback via the `animationVersion` poll gate | `TimelinePanel.tsx`; `timelineCanvas.ts`; `store.ts` |
| `node-trs-animation` | non-skeletal node animation — the generalized `AnimTarget`, the live entity forest, name→entity binding, and reuse of the one playback surface | `geometry/src/types.rs`; `assets/src/spawn.rs`; `animation/src/runtime.rs` |
| `morph-targets` | blend shapes — sparse `MorphDelta` storage, the fixed-point atomic-scatter GPU deform before skin, motion/RT, and the weight commands + sliders | `geometry/src/types.rs`; `assets/shaders/morph.slang`; `rendering/src/skinning.rs` |
| `foot-ik-and-physics-ahead` | the blend-layer pose-producer model, two-bone analytic IK, the v1 ground-plane foot planting, and the reserved per-bone `BonePhysics` metadata as the ragdoll on-ramp | `animation/src/ik.rs`; `animation/src/runtime.rs`; `scene/src/component.rs` |
