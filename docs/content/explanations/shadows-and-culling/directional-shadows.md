+++
title = 'Directional shadows'
weight = 1
math = true
+++

# Directional shadows

A directional shadow is a shadow cast by a light treated as parallel rays from infinity, like the sun. The scene is rendered once into a single 2D depth map from the light's point of view. Each mesh fragment then tests that map to find whether a nearer surface stood between it and the light.

A shadow map records the distance to the nearest surface along each direction the light sees. For a directional source the whole scene fits one orthographic frustum, so there is no cascade split — a single depth map covers everything.

## Light view and the depth pass

A directional light has a direction but no position, so its view is an orthographic projection looking down that direction. `render_scene` fits the frustum to the scene's world-space AABB each frame, building the light transform from a bounding sphere of that box so the fit stays stable as the light rotates. The `orthographic` helper emits Vulkan's $[0, 1]$ clip depth directly, with no remap. The transform reaches the renderer through `set_directional_shadow`, which stores it and flags the caster; `Lighting` uploads it as `shadow_view_proj` in the light UBO.

The pass itself is a depth-only draw. The graph adds it before the scene pass when a caster is present, with the 2048² `D32` shadow map as its sole depth attachment. `add_shadow_pass` records the body through `record_shadow_depth`, which reuses the depth-pre-pass machinery — the vertex-only shadow pipeline and the per-frame instance set — but pushes the light's view-projection instead of the camera's, and applies a [depth bias](../shadow-bias/) per batch.

```mermaid
flowchart LR
    A[shadow pass<br/>depth from light] -->|DepthWrite| B[shadow map]
    B -->|SampledRead| C[scene pass<br/>pcfShadow]
```

The [render graph](../../frame-and-render-graph/render-graph-overview/) derives both transitions across that arrow from the declared usages (`RgUsage::DepthWrite`, then `RgUsage::SampledRead`); no barrier is hand-written.

## Sampling in the scene pass

The mesh fragment evaluates the directional light through the same BRDF as every other light, then multiplies the result by visibility from `pcfShadow`. The `globals.counts.y` flag is the directional-shadow toggle, so the map is sampled only when a caster ran this frame. `pcfShadow` projects the world position into the light's clip space and runs a 3×3 comparison filter — see [PCF filtering](../pcf-filtering/) for the kernel.

## Design and trade-offs

One orthographic frustum fit to the whole scene is the simplest correct approach and stays correct as the scene grows. The cost is resolution: a single 2048² map spread over a large world gives coarse texels far from the camera. Cascaded shadow maps are the standard fix and a clean future addition, since the pass already slots into the graph by declaration. The map's layout is tracked in an external-layout slot, so its descriptor stays valid as `ShaderReadOnly` on frames where no caster runs.

## In the code

| What | File | Symbols |
|---|---|---|
| Fit the ortho frustum | `assets/src/render_scene.rs` | `render_scene` (shadow-fit block), `orthographic` |
| Store + flag the transform | `crates/rendering/src/lighting.rs` | `Lighting::set_directional_shadow`, `shadow_view_proj` |
| Add the pass | `crates/rendering/src/renderer.rs` | `add_shadow_pass`, `"shadow"` pass |
| Record depth from the light | `crates/rendering/src/scene_pass.rs` | `record_shadow_depth` |
| Map size + bias constants | `crates/rendering/src/lighting.rs` | `SHADOW_MAP_SIZE`, `SHADOW_DEPTH_BIAS_CONSTANT`, `SHADOW_DEPTH_BIAS_SLOPE` |
| Sample + compare | `assets/shaders/lighting.slang` | `pcfShadow`, `evalLighting` (`counts.y` branch) |

## Related

- [PCF filtering](../pcf-filtering/) — the 3×3 comparison kernel that samples the map
- [Shadow bias](../shadow-bias/) — the constant + slope bias that fights acne
- [Spot-light shadows](../spot-light-shadows/) — the same depth path with a perspective frustum
- [Render graph](../../frame-and-render-graph/render-graph-overview/) — where the shadow pass slots in
