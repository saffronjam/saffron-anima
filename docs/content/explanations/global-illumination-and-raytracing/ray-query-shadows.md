+++
title = 'Ray-query shadows'
weight = 8
+++

# Ray-query shadows

A ray-query shadow tests whether a shaded point sees a light by tracing a single inline ray toward
that light through an acceleration structure, instead of sampling a depth map. Any geometry along
the ray means the point is occluded.

Inline ray queries run inside an ordinary shader stage. They use no ray-tracing pipeline and no
shader binding table, so the shadow test lives in the fragment shader alongside the rest of shading.

> [!NOTE]
> This path is feature-gated and runs at ~1 FPS on the software dev GPU. It is correctness-validated
> and waits on real ray-tracing hardware.

## How it works

The fragment shader builds a ray from the shaded point toward the light, traces it against the
[TLAS](../raytracing-foundation/), and reads back whether the ray hit anything. A hit returns 0
(occluded); a miss returns 1 (lit). The shader sets up a `RayDesc`, calls `TraceRayInline` and
`Proceed()` on a `RayQuery` object, then inspects the committed status.

```hlsl
ray.Origin = worldPos + toLight * 0.02;  // bias off the surface to avoid self-hit
ray.TMax   = maxDist;
RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
q.TraceRayInline(rtScene, /* same flags */, 0xFF, ray);
q.Proceed();
return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
```

`ACCEPT_FIRST_HIT_AND_END_SEARCH` is the shadow-ray optimization: any hit shadows the point, so
traversal stops at the first triangle rather than searching for the closest. The `0.02` origin bias
along the light direction lifts the ray off the surface so it does not immediately strike the
triangle it started on.

## Integration with shading

A single runtime flag (`globals.pointShadowMeta.z`) switches the whole engine between shadow maps
and ray-query shadows. When set, the directional light traces one long ray toward the sun
(`maxDist = 1e4`) in place of a PCF lookup, and every punctual light traces one ray toward that
light inside the `punctual` function. The result feeds the same `shadow` scalar the BRDF multiplies.

## Why a ray over a map

The shadow-map paths shadow exactly one spot light and one point light (the cube map) plus the
directional; the remaining punctual lights are unshadowed. Ray-query shadows every punctual light
with one ray each, under no per-light shadow-map budget. The ray distance is the actual light
distance, so it cannot report an occluder past the light. It is the correctness baseline the map
paths approximate: same `shadow` scalar, same BRDF, sourced from a ray instead of a depth comparison.

## No pipeline or binding table

Inline ray queries traverse within the existing graphics pipeline. There is no ray-tracing pipeline
object, no shader binding table, and no hit or miss shaders — only the `RayQuery` object and the
TLAS bound in set 6. The sole RT-specific frame work is building the TLAS
([the foundation](../raytracing-foundation/)); the shadow itself is a few instructions in the
fragment shader.

## In the code

| What | File | Symbols |
|---|---|---|
| The inline shadow ray | `lighting.slang` | `rayQueryShadow` |
| TLAS binding (set 6) | `lighting.slang` | `rtScene` |
| Directional / punctual switch | `lighting.slang` | `evalLighting`, `punctual` (the `pointShadowMeta.z` branches) |
| The runtime toggle | `rendering/src/renderer.rs` | `Renderer::set_rt_shadows`, `rt_shadows_enabled` |
| TLAS supply | `rendering/src/rt.rs` | `Rt::prepare_tlas_build`, `Rt::tlas_ready` |

> [!WARNING]
> The mesh PSO declares `rtScene` and `rayQueryShadow` unconditionally, so the compiled SPIR-V
> carries the `RayQueryKHR` capability even on a non-RT GPU. The binding is never *accessed* there
> (the runtime flag stays off), but the capability is in the module — a driver that rejects the
> capability outright would fail to create the PSO.

## Related

- [Acceleration structures](../raytracing-foundation/) — the TLAS this traces against
- [RT device gating](../raytracing-device-gating/) — why the flag exists and when it's safe to set
- [Directional shadows](../../shadows-and-culling/directional-shadows/) — the shadow-map path this replaces
- [Cook-Torrance BRDF](../../lighting-and-brdf/cook-torrance-brdf/) — what the `shadow` scalar multiplies
