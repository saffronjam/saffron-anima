+++
title = 'PCF filtering'
weight = 4
math = true
+++

# PCF filtering

Percentage-closer filtering averages several depth-comparison samples around a lookup point so a shadow edge fades over a few texels instead of snapping from lit to dark. The result is a visibility factor in $[0, 1]$ rather than a single lit-or-shadowed bit.

The 2D shadow maps (directional and spot) use a 3×3 grid of hardware comparison taps. The same `pcfShadow` function serves both maps.

## How it works

The maps are bound as `Sampler2DShadow`, a comparison sampler. Each tap returns whether the stored depth passes the test against a reference, not the stored depth itself. `pcfShadow` projects the world position into the light's clip space, takes `ndc.z` as the reference depth, and averages nine `SampleCmp` taps stepped one texel apart (`texel = 1/2048`, matching `ShadowMapSize`):

```hlsl
sum += map.SampleCmp(uv + float2(x, y) * texel, ndc.z);
```

`SampleCmp` runs the depth-less-than-or-equal test in the sampler hardware. Averaging nine results yields 0, 1/9, 2/9, …, 1, a quantized soft edge.

## Off-map and beyond-far cases

A fragment can project outside the map, past its far plane, or behind the light. None of these positions carry valid shadow information, so an early-out guard handles each case:

| Condition | Meaning | Result |
|---|---|---|
| `clip.w <= 0` | behind the light | lit |
| `uv` outside $[0,1]^2$ | outside the light frustum | lit |
| `ndc.z > 1` | past the far plane | lit |

Treating absent information as lit is the safe default. It avoids a hard black band at the frustum edge and a shadow that swallows everything past the far plane. The cost is that geometry genuinely outside the frustum is never shadowed, which is why the directional frustum is [fit to the whole scene](../directional-shadows/).

## Design and trade-offs

A fixed 3×3 kernel is the cheapest filter that visibly helps. It hides the texel grid at the cost of a constant-width penumbra: the softening is the same regardless of occluder distance, so it does not model contact-hardening soft shadows. Wider kernels, Poisson-disk taps, or a rotated kernel smooth more at more cost. The engine keeps the averaged grid and leaves those as drop-in changes to one function. The point light does not use this path; its cube stores distance and does a hard comparison (see [point shadows](../point-light-cube-shadows/)).

## In the code

| What | File | Symbols |
|---|---|---|
| The 3×3 comparison filter | `mesh.slang` | `pcfShadow` |
| Comparison samplers | `mesh.slang` | `shadowMap`, `spotShadowMap` (`Sampler2DShadow`) |
| Where it's called | `mesh.slang` | `fragmentMain`, `punctual` |
| Map size (texel step) | `renderer_detail.cppm` | `ShadowMapSize` |
| The compare sampler object | `renderer_detail.cppm` | `descriptors.shadowSampler` |

## Related

- [Directional shadows](../directional-shadows/) — the map this filters
- [Spot-light shadows](../spot-light-shadows/) — the other map on the same path
- [Shadow bias](../shadow-bias/) — the bias the reference depth carries into the compare
- [Point shadows](../point-light-cube-shadows/) — the hard-comparison alternative for points
