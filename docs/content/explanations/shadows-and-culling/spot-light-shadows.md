+++
title = 'Spot shadows'
weight = 2
+++

# Spot shadows

A spot light shadow is a shadow map rendered from a perspective light view down the cone. A spot light has both a position and a direction, so its frustum is a true perspective view, unlike the orthographic frustum a directional light uses.

The mechanics reuse the directional path: the same depth-only draw and the same `pcfShadow` comparison sampler. Only the light view changes, from orthographic to perspective.

> [!NOTE]
> Only the first spot light in the scene is shadowed. Multiple shadowed spots would need a map (or atlas) per light plus per-light view-projection entries in the UBO.

## Light view and the depth pass

The light frustum is built for the first spot found. Its field of view is twice the cone's outer angle plus a few degrees of pad, so the penumbra at the cone edge stays inside the map. Aspect is 1, and the far plane is the light's range. An `up`-vector flip avoids a degenerate look-at when the spot points straight up or down. The transform reaches the renderer through `set_spot_shadow`, which also records which light index is shadowed so the fragment shader knows where to apply it.

The depth pass is a depth-only draw into a second 2048² map, declared in the graph next to the directional one as the `"spot-shadow"` pass. It calls the same `record_shadow_depth`; only the view-projection push constant differs. It shares the vertex-only pipeline, the instance set, and the depth bias.

## Sampling per light

The spot shadow is applied inside the punctual light loop, but only for the matching light index:

```hlsl
if (globals.pointShadowMeta.z == 0 && globals.spotShadow.y != 0 && lightIndex == globals.spotShadow.x)
{
    shadow = pcfShadow(spotShadowMap, globals.spotShadowViewProj, worldPos);
}
```

`spotShadow.y` is the enable flag and `spotShadow.x` is the shadowed light's index. Every other spot and every point light contributes unshadowed (unless ray-traced shadows are on, gated by `pointShadowMeta.z`). Visibility folds into the light's radiance alongside cone falloff and distance attenuation before the BRDF.

## Design and trade-offs

Sharing the depth pass keeps the spot shadow inexpensive: one extra map, one pass declaration, one index check. A perspective frustum makes the bias behave differently across the cone than the directional orthographic frustum does, so both maps share the same tuned [bias](../shadow-bias/) constants rather than per-light values. The single-spot path covers the common case; an array generalization is a later step.

## In the code

| What | File | Symbols |
|---|---|---|
| Build the perspective frustum | `assets/src/render_scene.rs` | `render_scene` (spot gather) |
| Store transform + light index | `crates/rendering/src/lighting.rs` | `Lighting::set_spot_shadow`, `spot_shadow_view_proj` |
| Add the pass | `crates/rendering/src/renderer.rs` | `add_shadow_pass`, `"spot-shadow"` pass |
| Record depth (shared) | `crates/rendering/src/scene_pass.rs` | `record_shadow_depth` |
| Per-light sample + index check | `assets/shaders/lighting.slang` | `punctual` (spot branch), `pcfShadow` |
| UBO fields | `crates/rendering/src/lighting.rs` | `LightUbo` (`spot_shadow_view_proj`, `spot_shadow`) |

## Related

- [Directional shadows](../directional-shadows/) — the depth path this reuses
- [PCF filtering](../pcf-filtering/) — the shared comparison kernel
- [Punctual lights and attenuation](../../lighting-and-brdf/punctual-lights-and-attenuation/) — the cone + range the shadow rides on
- [Render graph](../../frame-and-render-graph/render-graph-overview/) — where the pass slots in
