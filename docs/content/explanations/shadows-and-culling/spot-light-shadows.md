+++
title = 'Spot shadows'
weight = 2
+++

# Spot shadows

A spot light shadow is a shadow map rendered from a perspective light view down the cone. A spot light has both a position and a direction, so its frustum is a true perspective view, unlike the orthographic frustum a directional light uses.

The mechanics reuse the directional path: the same depth-only draw and the same `pcfShadow` comparison sampler. Only the light view changes, from orthographic to perspective.

> [!NOTE]
> Only the first spot light in the scene is shadowed. Multiple shadowed spots would need a map (or atlas) per light plus per-light viewProj entries in the UBO.

## Light view and the depth pass

The light frustum is built for the first spot found. Its field of view is twice the cone's outer angle plus a few degrees of pad, so the penumbra at the cone edge stays inside the map. Aspect is 1, and the far plane is the light's range. An `up`-vector flip avoids a degenerate `lookAt` when the spot points straight up or down. The transform reaches the renderer through `setSpotShadow`, which also records which light index is shadowed so the fragment shader knows where to apply it.

The depth pass is a depth-only draw into a second 2048² map, declared in the graph next to the directional one. It calls the same `recordShadowDepth`; only the viewProj push constant differs. It shares the vertex-only pipeline, the instance set, and the depth bias.

## Sampling per light

The spot shadow is applied inside the punctual light loop, but only for the matching light index:

```hlsl
if (globals.spotShadow.y != 0 && lightIndex == globals.spotShadow.x)
{
    shadow = pcfShadow(spotShadowMap, globals.spotShadowViewProj, worldPos);
}
```

`spotShadow.y` is the enable flag and `spotShadow.x` is the shadowed light's index. Every other spot and every point light contributes unshadowed. Visibility folds into the light's radiance alongside cone falloff and distance attenuation before the BRDF.

## Design and trade-offs

Sharing the depth pass keeps the spot shadow inexpensive: one extra map, one pass declaration, one index check. A perspective frustum makes the bias behave differently across the cone than the directional orthographic frustum does, so both maps share the same tuned [bias](../shadow-bias/) constants rather than per-light values. The single-spot path covers the common case; an array generalization is a later step.

## In the code

| What | File | Symbols |
|---|---|---|
| Build the perspective frustum | `assets.cppm` | `renderScene` (spot gather) |
| Store transform + light index | `renderer.cppm` | `setSpotShadow` |
| Add the pass | `renderer.cppm` | `beginFrameGraph` (`doSpotShadow`) |
| Record depth (shared) | `renderer_drawlist.cpp` | `recordShadowDepth` |
| Per-light sample + index check | `mesh.slang` | `punctual` (spot branch), `pcfShadow` |
| UBO fields | `renderer_lighting.cpp` | `spotShadowViewProj`, `spotShadow` |

## Related

- [Directional shadows](../directional-shadows/) — the depth path this reuses
- [PCF filtering](../pcf-filtering/) — the shared comparison kernel
- [Punctual lights and attenuation](../../lighting-and-brdf/punctual-lights-and-attenuation/) — the cone + range the shadow rides on
- [Render graph](../../frame-and-render-graph/render-graph-overview/) — where the pass slots in
