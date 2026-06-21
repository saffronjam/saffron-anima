+++
title = 'Lighting & BRDF'
weight = 8
bookCollapseSection = true
+++

# Lighting & BRDF

Direct lighting is the radiance reaching a surface straight from the scene's light sources,
shaded through a physically based reflectance model. Lights are fully dynamic hecs components
with nothing baked. A compute pass culls punctual lights into a froxel grid, so each fragment
loops only the lights touching its cluster. Shading is a Cook-Torrance metallic-roughness BRDF
accumulated in linear HDR.

## Pages

| Page | Covers | Code |
|---|---|---|
| [light-components](light-components/) | directional / point / spot components and their packed GPU form | `scene/component.rs`; `rendering/lighting.rs` |
| [cook-torrance-brdf](cook-torrance-brdf/) | Fresnel, GGX, Smith, the diffuse/specular split | `lighting.slang` · `brdf` |
| [directional-light](directional-light/) | the shadowed sun, through the shared BRDF | `lighting.slang` · `evalLighting` |
| [punctual-lights-and-attenuation](punctual-lights-and-attenuation/) | inverse-square + range window, spot cone | `lighting.slang` · `punctual` |
| [clustered-forward](clustered-forward/) | the 16×9×24 froxel grid, exponential Z, the compute cull | `light_cull.slang`; `rendering/lighting.rs` |
| [cluster-indexing](cluster-indexing/) | mapping a fragment's pixel + view-Z to a cluster | `lighting.slang` · `clusterIndexFor` |
| [brute-force-fallback](brute-force-fallback/) | `set-clustered 0`, pixel-identical to the clustered path | `lighting.slang`; `rendering/renderer.rs` |
| [hdr-and-exposure](hdr-and-exposure/) | the rgba16f offscreen, linear radiance, EV-stop exposure | `rendering/pipelines.rs`; `tonemap.slang` |
| [ibl-ambient-term](ibl-ambient-term/) | the split-sum ambient that replaces flat ambient | `lighting.slang` · `evalLighting` |
| [per-cluster-cap](per-cluster-cap/) | the 64-light cap and its silent-drop behaviour | `lighting.slang` · `MAX_LIGHTS_PER_CLUSTER` |
