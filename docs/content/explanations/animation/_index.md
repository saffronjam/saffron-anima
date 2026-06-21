+++
title = 'Animation'
weight = 17
bookCollapseSection = true
+++

# Animation

Skeletal animation deforms a rigged mesh over time by driving its skeleton's joints from
authored clips. The engine already skins — the glTF skin import builds one entity per joint,
tags each with a `Bone` component, and a vertex palette deforms the mesh every frame — so animation
is the layer that supplies a new *source* for each joint's local transform: a clip, sampled at
the current time, written into a runtime pose rather than over the authored rest transforms.

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
| `timeline` | the read-only editor Timeline panel — track rows, ms ruler, clip bars, a scrubbable playhead, Edit-preview transport; reads playback via the `animationVersion` poll gate; deferred authoring mode | `TimelinePanel.tsx`; `timelineCanvas.ts`; `store.ts` |
| `foot-ik-and-physics-ahead` | the blend-layer pose-producer model, two-bone analytic IK, the v1 ground-plane foot planting, and the reserved per-bone `BonePhysics` metadata as the ragdoll on-ramp | `animation/src/ik.rs`; `animation/src/runtime.rs`; `scene/src/component.rs` |
