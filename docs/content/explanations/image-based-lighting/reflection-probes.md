+++
title = 'Reflection probes'
weight = 8
math = true
+++

# Reflection probes

The global [IBL bake](../ibl-bake-pass/) captures one environment for the whole scene. That is right for the sky and the open-air fill, but it cannot show local detail: a mirror sphere in a red room should reflect the red walls, not the procedural sky. A reflection probe is a per-entity environment that fixes this. It supplies specular ambient to meshes inside its influence sphere, prefiltered with the same convolution the global IBL uses. Outside every probe, a mesh falls back to the global IBL unchanged.

## The component

A `ReflectionProbe` is a `hecs` component, positioned by the entity's `Transform` (no position field of its own).

| Field | Meaning |
|---|---|
| `influence_radius` | the sphere of effect around the probe origin |
| `intensity` | a specular multiplier on the probe contribution |
| `box_projection` | parallax-correct the reflection ray against an influence box |
| `box_extent` | half-extents of that box (used when `box_projection`) |

A runtime-only `dirty` flag (not serialized) marks a probe for capture, set on add/edit. The component registers through the `register_component!` macro, so save/load, the inspector, and add/remove all come for free, with no scene-version bump.

## Capture is on demand

A probe capture is far heavier than the single-cube global bake. So it is strictly on demand: it runs only when a probe is `dirty`, never per frame. The host feeds the renderer a per-frame `ReflectionProbeUpload` slice, and `ReflectionProbes::submit` arms `capture_pending` only when a slot is genuinely new, moved, resized, or flagged dirty — the same exact-compare guard the [IBL re-bake](../ibl-bake-pass/) uses to avoid float churn. The flag is consumed at a GPU-idle frame start, the same stall point as the IBL re-bake, so an in-flight frame is never disturbed.

## Sampling and blend

The probe array rides the IBL descriptor set (set 3). Within that set, binding 3 is a prefiltered-cube array, binding 4 an irradiance-cube array, and binding 5 a metadata SSBO of `ProbeMeta` records — `MAX_REFLECTION_PROBES = 8` slots. Packing probes into the IBL set (rather than a separate set) keeps the mesh pipeline within the bound-descriptor-set budget. Every array slot is seeded with the global IBL cubes at startup (`ReflectionProbes::seed`), so the bind is always valid; a captured probe overwrites its slot.

The mesh fragment picks the nearest probe whose influence sphere contains the surface, samples its prefiltered cube by the reflection vector, and lerps the specular IBL term toward it by an edge-soft weight — full near the center, ramping to zero at the influence boundary. With `box_projection` on, the reflection ray is re-projected against the influence box for parallax-correct local reflections (`boxProject` in the shader):

$$
R' = \big(p + R\, d\big) - o, \qquad
d = \min_i \max\!\big(t^{+}_i, t^{-}_i\big)
$$

where $p$ is the world position, $R$ the reflection vector, $o$ the probe origin, and $t^{\pm}$ the slab intersections with $o \pm \text{extent}$.

The probe count rides in the light UBO (`ambientColor.w`, bit-cast from the `u32` count). When it is zero — no probes in the scene, or `sa set-probes 0` — the specular term is byte-identical to the global IBL fallback, so a probe-free scene renders exactly as the global IBL alone.

## Driving it

| What | File | Symbols |
|---|---|---|
| Component | `engine/crates/scene/src/component.rs` | `ReflectionProbe` |
| Per-frame upload | `engine/crates/rendering/src/ibl.rs` | `ReflectionProbeUpload`, `ProbeMetaGpu` |
| Probe array + capture state | `engine/crates/rendering/src/ibl.rs` | `ReflectionProbes`, `ReflectionProbe`, `submit`, `seed`, `write_slot`, `capture_pending` |
| Renderer entry points | `engine/crates/rendering/src/renderer.rs` | `submit_reflection_probes`, `set_reflection_probes`, `reflection_probes` |
| Dirty drive | `engine/crates/assets/src/render_scene.rs` | `render_scene` — probe upload gather |
| Fragment blend | `engine/assets/shaders/lighting.slang` | set 3 bindings 3-5 (`probeCubes`, `probeIrradiance`, `probeMeta`), `boxProject`, `ProbeMeta` |
| Control | `engine/crates/control/src/commands_render.rs` | `set-probes`, `recapture-probes`, `list-probes` |

From a shell against a running host: `sa set-probes {0|1}` toggles probe sampling (the A/B identity gate), `sa recapture-probes` re-arms every probe, and `sa list-probes` reports each probe's origin, radius, intensity, and captured state.
