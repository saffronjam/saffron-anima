+++
title = 'GTAO'
weight = 2
math = true
+++

# GTAO

Ground-truth ambient occlusion (GTAO) is a screen-space technique that estimates how much of a
surface's ambient light is blocked by nearby geometry. Ambient light fills the parts of a surface the
direct lights miss; a crease or an overhang receives less of that fill because adjacent surfaces
occlude it. GTAO computes a per-pixel occlusion factor and uses it to darken only the indirect term.

The pass is horizon-style and reads the [thin G-buffer](../thin-gbuffer/) — normals and depth — so it
needs no extra geometry.

## How it works

Each pixel reconstructs its view-space position $p$ and normal $n$ from the G-buffer, then samples the
neighborhood for occluders. The pass walks a few azimuth slices around the pixel, takes a few steps
along each, and turns every tap's depth into a view-space position $s$. A tap occludes when it rises
above the pixel's tangent plane and lies close enough:

$$
\text{occlusion} \mathrel{+}= \max\!\big(n \cdot \hat{d} - \text{bias},\ 0\big)\;\cdot\;\text{rangeCheck},
\qquad \hat{d} = \frac{s - p}{\lVert s - p\rVert}
$$

The dot product $n \cdot \hat{d}$ is the cosine of the angle between the normal and the direction to
the tap. A positive value places the tap inside the hemisphere above the surface, where it blocks
ambient light from that direction. The `bias` (0.02) discards nearly coplanar taps, which avoids
self-occlusion on flat ground. The `rangeCheck` fades occlusion out past the configured radius, so a
distant wall cannot darken a pixel it should not; only nearby geometry counts. Taps on background
(`viewZ > -1e-4`) or off-screen contribute nothing.

The loop runs four slices of six steps. The screen-space step size comes from the world radius
projected to screen, clamped so near surfaces do not over-sample a large region and far ones still
register. A small per-pixel rotation, hashed from the pixel coordinates, jitters the slice angles so
the low sample count does not band:

$$
\varphi_s = \frac{s + \text{rnd}}{\text{sliceCount}} \cdot 2\pi
$$

The result is averaged, scaled by a strength knob, and stored as a single AO factor in $[0, 1]$, where
1 is fully open:

$$
\text{ao} = \operatorname{saturate}\!\big(1 - \text{occlusion} \cdot \text{strength}\big)
$$

### Denoise pass

Four slices of six steps is noisy, and the per-pixel rotation trades banding for grain. GTAO writes its
raw factor to an intermediate `ao_raw` target; a second compute pass (`ao-blur`) reads `ao_raw` plus the
G-buffer normal and writes the final `ao_map` (sampled in the shader as `aoMap`). The blur is bilateral:
it smooths across the noise but respects normal discontinuities, so AO does not bleed across edges.

### Where the AO lands

GTAO modulates only the indirect term. The mesh fragment shader computes the ambient contribution (flat
fallback or IBL) first, then multiplies by the AO factor. Direct lights are untouched. AO is a coarse
stand-in for the visibility of the ambient hemisphere, while a direct light already has a known
direction and its own shadow term, so applying AO there would double-darken.

## In the code

| What | File | Symbols |
|---|---|---|
| AO pass | `gtao.slang` | `computeMain`, `viewPosFromUv`, `sliceCount`, `stepCount`, `bias` |
| Pass + denoise wiring | `renderer.rs` | the `gtao` + `ao-blur` passes, `ao_raw`, `ao_map` |
| Push + camera setup | `ssao.rs` | `GtaoPush`, `Ssao::gtao_push`, `Ssao::set_camera` |
| Where AO is applied | `lighting.slang` | `aoMap` (set 4), `counts.w` gate |

> [!NOTE]
> The AO factor is `r8` (one 8-bit channel). It is a visibility scalar, not a color, so an `rgba16f`
> target would waste three channels and twice the bits. The output is written as a storage image in
> `GENERAL`, the same in-place compute shape as every other screen-space pass.

## Related

- [G-buffer](../thin-gbuffer/) — the normal + depth it reads
- [Image-based lighting](../../image-based-lighting/) — the indirect term AO darkens
- [Compute post-process](../compute-post-process-pattern/) — the dispatch + RMW shape
