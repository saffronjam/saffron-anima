+++
title = 'Motion vectors'
weight = 5
math = true
+++

# Motion vectors

A motion vector is, per pixel, the screen-space offset from where a surface point appears this frame to
where it appeared in the previous frame. Stored in a per-pixel buffer, it lets a later pass find the
history sample that corresponds to the same surface point.

Temporal techniques reuse last frame's pixels, but the camera moves between frames, so a surface that
sat at one pixel last frame lands at a different pixel now. The motion vector closes that gap.
[TAA](../taa/) follows it backward to the correct history sample. Saffron computes camera-reprojection
velocity into an `rg16f` target.

## How it works

The pass renders the instanced scene depth-tested, the same way the other prepasses do. A push constant
carries the current and previous camera `viewProj` matrices, and each instance carries its current and
previous world matrix (`model` / `prevModel`). The vertex stage builds two clip-space positions — the
current one from `curViewProj · model · position`, the previous one from
`prevViewProj · prevModel · prevPosition` — so both camera and object motion reproject together. The
fragment stage performs the perspective divide on both, turning clip space into NDC, and outputs the
difference scaled into UV space:

$$
\text{motionUv} = \big(\text{ndc}_\text{prev} - \text{ndc}_\text{cur}\big) \cdot 0.5,
\qquad \text{ndc} = \frac{\text{clip}_{xy}}{\text{clip}_w}
$$

The factor of $0.5$ is the NDC→UV scale: NDC spans $[-1, 1]$ across the screen and UV spans $[0, 1]$,
so a delta in NDC is half as large in UV. The result is the offset from this pixel's current UV to
where the surface was last frame, which is exactly what TAA adds to its own UV to find history
(`histUv = uv + mv`). Both `viewProj` matrices use the same Y-flipped projection the scene renders
with, so the Y sign matches the images TAA samples, and no separate flip is needed.

## In the code

| What | File | Symbols |
|---|---|---|
| The reprojection | `motion.slang` | `vertexMain`, `fragmentMain`, the `Push` cur/prev viewProj |
| Recorder + push | `aa.rs` | `record_motion`, `MotionPush`, `MOTION_FORMAT` |
| Pass declaration | `renderer.rs` | the `motion` pass, `motion_depth` |
| The consumers | `taa.slang`, `ssgi_accum.slang` | `motion` sampler, `histUv = uv + mv` |

> [!NOTE]
> The pass tracks camera motion through the cur/prev viewProj, and a moving object's per-instance
> previous-model transform (`inst.prevModel`) gives it the correct velocity; the skinned deform-motion
> path supplies distinct cur/prev deformed vertex buffers so animated geometry reprojects too.

> [!NOTE]
> The motion pass has its own depth attachment (`motion_depth`) and runs before the scene pass, so both
> consumers can sample it: SSGI temporal accumulation (before the scene) and the TAA resolve (after).
> It is a dedicated prepass rather than a reuse of the scene depth, because the scene's depth target may
> be multisampled or otherwise shaped by the active AA mode. The pass runs whenever TAA *or* SSGI is on,
> since both own a temporal accumulation that reprojects through it.

## Related

- [TAA](../taa/) — a consumer of the motion buffer, for its resolve reprojection
- [SSGI](../ssgi/) — its temporal accumulation reprojects last frame's GI through the same buffer
- [G-buffer](../thin-gbuffer/) — the sibling prepass that records normal + depth
- [ReSTIR](../../global-illumination-and-raytracing/) — temporal reuse that also wants reprojection
