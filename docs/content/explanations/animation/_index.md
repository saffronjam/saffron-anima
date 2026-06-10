+++
title = 'Animation'
weight = 17
bookCollapseSection = true
+++

# Animation

Skeletal animation deforms a rigged mesh over time by driving its skeleton's joints from
authored clips. The engine already skins — the glTF skin import builds one entity per joint,
tags them with `BoneComponent`, and a vertex palette deforms the mesh every frame — so animation
is the layer that supplies a new *source* for each joint's local transform: a clip, sampled at
the current time, written into a runtime pose rather than over the authored rest transforms.

The pose flows **sample → pose buffer → an (inert) per-bone blend layer → world-transform
composition**. The authored bone transforms keep the rest pose and are never overwritten, so
playback is non-destructive in both Edit and Play, and the blend layer is the seam every later
pose producer — foot IK, and eventually a powered ragdoll — plugs into without touching the
sampling path.

This section starts at the bottom: the pure data and math the rest of the system is built on.

## Pages

| Page | Covers | Code |
|---|---|---|
| `animation-data-model` | the clip/track keyframe model, the decomposed joint pose + blend layer, and clip sampling (STEP/LINEAR/CUBICSPLINE with slerp) | `geometry.cppm`; `animation.cppm`; `animation.cpp` |
| `playback-runtime` | the per-frame evaluator: sample → pose → blend → pose override → world composition; non-destructive Edit preview vs Play; wrap modes | `animation.cpp`; `scene.cppm`; `host.cppm` |
| `skeleton-overlay` | the line-skeleton viewport overlay for the selected rig — bone segments, joint dots, optional RGB axes; on-top, Edit + Play; the `set-skeleton-overlay` toggle | `host.cppm`; `scene_edit_context.cppm`; `control_commands_animation.cpp` |
| `timeline` | the read-only editor Timeline panel — track rows, ms ruler, clip bars, a scrubbable playhead, Edit-preview transport; reads playback via the `animationVersion` poll gate; deferred authoring mode | `TimelinePanel.tsx`; `timelineCanvas.ts`; `store.ts` |
