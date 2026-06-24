# Procedural cameras & cinematics

**Status:** PENDING IDEA

> Inspiration backlog — not yet implementable as written. Needs a codebase pass (the vcam/brain
> component model, camera collision over Jolt shapecast, and generalizing the animation timeline into a
> multi-track Sequencer).

Mostly CPU work that cashes in primitives we already have: camera collision uses Jolt shapecast,
blending reuses the animation-blend curves, shake maps onto the signal/slot system, and cinematic
depth-of-field is a single render-graph compute pass. The Sequencer itself is the one XL piece and wants
scene-graph parenting first.

## What it is

A gameplay camera system (follow/aim/orbit rigs with smoothing and collision) plus a cinematic timeline
for cutscenes.

- **UE5:** Gameplay Camera System (the new vcam-style framework) + Sequencer + Cinematic Camera.
- **Unity:** Cinemachine (virtual cameras + brain) + Timeline.

## Core technique

Many lightweight **virtual cameras**, each a **Body** (position/follow) → **Aim** (look-at/framing) →
**Noise** (shake) pipeline, plus a **brain/director** that picks the live vcam by priority and **blends**
the real camera toward it (eased state interpolation). **Camera collision** shapecasts from the target to
the desired position and pulls in on a hit. **Shake** is impulse-driven noise. **Cinematic DoF** computes
a circle-of-confusion from depth and does a scatter-as-gather bokeh blur. The **Sequencer** is a
multi-object, multi-property **track model** (transform/property/audio/camera-cut tracks) evaluated along
a timeline.

## Build size

- **L** the procedural gameplay camera (vcam/brain) — pure CPU.
- **S** camera blending (state interp + easing, reuses animation-blend curves).
- **S–M** framing / look-ahead (Aim modules).
- **M** camera collision/occlusion (Jolt shapecast — low risk).
- **S–M** shake/impulse (maps onto `SubscriberList` signals).
- **M** cinematic DoF/bokeh — the one *rendering* item here not yet built; slots after TAA/tonemap.
- **XL** the Sequencer — generalize the existing animation-clip timeline into a multi-track model
  (**one** model, per the no-legacy rule — do not keep the anim-only timeline beside it).

## Dependencies (do these first)

- **Scene-graph parenting** (a known gap) — follow targets, attach tracks, vcam hierarchies.
- The Sequencer should **replace and absorb** the existing animation-timeline panel, not sit beside it.
- *DoF* is independent — a single compute pass after tonemap.

## What we reuse / what's missing

**Reuse:** hecs components + transform math, the node editor (rig authoring), the existing animation
timeline panel + blend curves (Sequencer foundation + camera blends), Jolt shapecast (collision), the
signal/slot system (shake impulses), and the control plane (camera-cut scrubbing from `sa`).

**Missing:** the vcam/brain component model, the cinematic DoF pass, and a generalized multi-track
timeline (plus parenting).

## Notes & references

- Unity Cinemachine docs — the Body/Aim/Noise + brain/priority model worth copying directly.
- UE5 Gameplay Camera System + Sequencer docs.
- "CryEngine"/scatter-bokeh DoF references for the cinematic depth-of-field pass.
