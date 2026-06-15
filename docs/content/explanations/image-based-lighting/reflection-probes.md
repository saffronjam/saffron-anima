+++
title = 'Reflection probes'
weight = 8
math = true
+++

# Reflection probes

The global [IBL bake](../ibl-bake-pass/) captures one environment for the whole scene. That is right for the sky and the open-air fill, but it cannot show local detail: a mirror sphere in a red room should reflect the red walls, not the procedural sky. A reflection probe is a per-entity environment that fixes this. It captures a *local* cubemap from a point in the scene, prefilters it with the same convolution the global IBL uses, and supplies specular ambient to meshes inside its influence sphere. Outside every probe, a mesh falls back to the global IBL unchanged.

## The component

A `ReflectionProbeComponent` is a plain entt struct, positioned by the entity's `Transform` (no position field of its own).

| Field | Meaning |
|---|---|
| `influenceRadius` | the sphere of effect around the probe origin |
| `intensity` | a specular multiplier on the probe contribution |
| `boxProjection` | parallax-correct the reflection ray against an influence box |
| `boxExtent` | half-extents of that box (used when `boxProjection`) |

A runtime-only `dirty` flag (not serialized) marks a probe for capture. It is set on add, on edit, on load, and by `sa recapture-probes`. The component registers through `registerComponent`, so save/load, the inspector, and add/remove all come for free, with no scene-version bump.

## Capture is on demand

A probe capture renders the *entire scene six times* — once per cube face — and then runs the irradiance and prefilter convolutions, which is far heavier than the single-cube global bake. So capture is strictly on demand: it runs only when a probe is `dirty`, never per frame. `submitReflectionProbes` (called each frame from `renderScene`) arms `capturePending` only when a slot is genuinely new, moved, resized, or flagged dirty — the same exact-compare guard the [IBL re-bake](../ibl-bake-pass/) uses to avoid float churn.

`beginFrameGraph` consumes the flag at its GPU-idle top, the same editor-time stall point as the IBL re-bake. `captureReflectionProbe` renders the six faces with the point-shadow face matrices into the probe's cube, then convolves it with `ibl_irradiance`/`ibl_prefilter` — the shipped shaders, reused verbatim because they read the source cube as an opaque `SamplerCube`. The probe being captured is never sampled during its own capture (its slot stays the global fallback until capture completes), so there is no feedback.

## Sampling and blend

The mesh fragment reads the probes through a new descriptor set 8: a prefiltered-cube array, an irradiance-cube array, and a metadata SSBO (`MaxReflectionProbes = 8` slots). Every array slot is seeded with the global IBL cubes at startup, so the bind is always valid; a captured probe overwrites its slot.

The fragment picks the nearest probe whose influence sphere contains the surface, samples its prefiltered cube by the reflection vector, and lerps the specular IBL term toward it by an edge-soft weight — full near the center, ramping to zero at the influence boundary. With `boxProjection` on, the reflection ray is re-projected against the influence box for parallax-correct local reflections:

$$
R' = \big(p + R\, d\big) - o, \qquad
d = \min_i \max\!\big(t^{+}_i, t^{-}_i\big)
$$

where $p$ is the world position, $R$ the reflection vector, $o$ the probe origin, and $t^{\pm}$ the slab intersections with $o \pm \text{extent}$.

The probe count rides in the light UBO (`ambientColor.w`). When it is zero — no probes in the scene, or `sa set-probes 0` — the specular term is byte-identical to the global IBL fallback, so a probe-free scene renders exactly as before.

## Driving it

| What | File | Symbols |
|---|---|---|
| Component | `scene.cppm` | `ReflectionProbeComponent` |
| Renderer state | `renderer_types.cppm` | `ReflectionProbe` · `ReflectionProbes` · `ReflectionProbeUpload` |
| Capture + convolve | `renderer_detail.cppm` | `captureReflectionProbe` · `seedReflectionProbeSet` |
| Dirty drive | `renderer_lighting.cpp` | `submitReflectionProbes` · `setReflectionProbes` |
| Fragment blend | `mesh.slang` | set 8 · `boxProject` |
| Control | `control_commands_scene.cpp` | `set-probes` · `recapture-probes` · `list-probes` |

From a shell against a running host: `sa set-probes {0\|1}` toggles probe sampling (the A/B identity gate), `sa recapture-probes` re-arms every probe, and `sa list-probes` reports each probe's origin, radius, intensity, and captured state.
