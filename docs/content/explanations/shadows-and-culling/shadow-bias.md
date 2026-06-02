+++
title = 'Shadow bias'
weight = 5
math = true
+++

# Shadow bias

Shadow bias is a small offset added to a shadow map's depth comparison so a surface does not shadow itself.

A shadow map stores depth at finite resolution. A surface compared against its own quantized depth tends to half-shadow itself, producing the dark speckles of shadow acne. Bias shifts the comparison enough to stop that. The offset has a working range. Too little bias and acne returns; too much and shadows detach from their casters, an artifact called peter-panning.

## How it works

Saffron applies bias in two places, each matched to what the map stores. The 2D maps for directional and spot lights are biased in the rasterizer during the depth pass; the point cube is biased in the shader.

The 2D maps are biased by `recordShadowDepth`:

```cpp
cmd.setDepthBias(ShadowDepthBiasConstant, 0.0f, ShadowDepthBiasSlope);
```

with constant `1.25` and slope `2.0`. The constant term shifts every depth value by a fixed amount. The slope term scales with the polygon's gradient relative to the light, which is what acne needs: a surface seen edge-on by the light spans more depth per texel and needs proportionally more bias. Because the bias is baked into the stored depth, the comparison in `pcfShadow` is a plain `SampleCmp` with no extra offset.

The point cube stores world distance rather than depth, so a rasterizer depth bias would carry the wrong units. It biases in the shader, in world-space distance: a fragment counts as lit when it falls within `PointShadowDistanceBias` (0.08 world units) of the nearest stored occluder.

## The acne–peter-panning trade

The two failure modes pull in opposite directions:

| Too little bias | Too much bias |
|---|---|
| surface shadows itself | shadow lifts off the contact point |
| dark speckle / moiré on lit faces | gap of light under the caster |

No single value is correct; bias lives in a tuning band. Saffron's constants are tuned on llvmpipe to remove acne without obvious peter-panning. Slope bias does most of the work, since acne is worst exactly where surfaces graze the light, and the constant handles the residual flat-surface case.

## Why these knobs

A normal-offset bias, which pushes the sample along the surface normal, is gentler on contact shadows, but it requires the normal in the shadow lookup and a per-light tuned distance. The rasterizer's built-in constant-plus-slope bias is free, since the hardware applies it during the depth pass, and it self-adjusts with polygon slope, covering the common case with two scalars. For the point cube, a flat world-space constant is the matching simple choice; its ideal value drifts with the light's range.

> [!TIP]
> If you see acne, raise `ShadowDepthBiasSlope` before the constant — acne is slope-driven. If shadows look detached, the constant is usually the culprit. For point lights there's only `PointShadowDistanceBias`, kept in sync between `mesh.slang` and `renderer_detail.cppm`.

## In the code

| What | File | Symbols |
|---|---|---|
| 2D rasterizer bias values | `renderer_detail.cppm` | `ShadowDepthBiasConstant`, `ShadowDepthBiasSlope` |
| Where the 2D bias is set | `renderer_drawlist.cpp` | `recordShadowDepth` (`setDepthBias`) |
| Point-cube world-space bias | `renderer_detail.cppm` | `PointShadowDistanceBias` |
| Where the point bias is applied | `mesh.slang` | `pointShadow` |

## Related

- [PCF filtering](../pcf-filtering/) — the comparison the 2D bias feeds into
- [Directional shadows](../directional-shadows/) — where `recordShadowDepth` sets the bias
- [Point shadows](../point-light-cube-shadows/) — the distance comparison the cube bias guards
