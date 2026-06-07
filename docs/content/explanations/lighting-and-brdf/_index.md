+++
title = 'Lighting & BRDF'
weight = 8
bookCollapseSection = true
+++

# Lighting & BRDF

Direct lighting is the radiance reaching a surface straight from the scene's light sources,
shaded through a physically based reflectance model. Lights are fully dynamic ECS components
with nothing baked. A compute pass culls punctual lights into a froxel grid, so each fragment
loops only the lights touching its cluster. Shading is a Cook-Torrance metallic-roughness BRDF
accumulated in linear HDR.

## Pages

| Page | Covers | Code |
|---|---|---|
| [light-components](light-components/) | directional / point / spot components and their packed GPU form | `scene.cppm`; `renderer_lighting.cpp` |
| [cook-torrance-brdf](cook-torrance-brdf/) | Fresnel, GGX, Smith, the diffuse/specular split | `mesh.slang` · `brdf` |
| [directional-light](directional-light/) | the shadowed sun, through the shared BRDF | `mesh.slang` · `fragmentMain` |
| [punctual-lights-and-attenuation](punctual-lights-and-attenuation/) | inverse-square + range window, spot cone | `mesh.slang` · `punctual` |
| [clustered-forward](clustered-forward/) | the 16×9×24 froxel grid, exponential Z, the compute cull | `light_cull.slang`; `renderer_lighting.cpp` |
| [cluster-indexing](cluster-indexing/) | mapping a fragment's pixel + view-Z to a cluster | `mesh.slang` · `clusterIndexFor` |
| [brute-force-fallback](brute-force-fallback/) | `set-clustered 0`, pixel-identical to the clustered path | `mesh.slang`; `renderer_lighting.cpp` |
| [hdr-and-exposure](hdr-and-exposure/) | the rgba16f offscreen, linear radiance, EV-stop exposure | `renderer_types.cppm`; `tonemap.slang` |
| [ibl-ambient-term](ibl-ambient-term/) | the split-sum ambient that replaces flat ambient | `mesh.slang` · `fragmentMain` |
| [per-cluster-cap](per-cluster-cap/) | the 64-light cap and its silent-drop behaviour | `mesh.slang` · `MAX_LIGHTS_PER_CLUSTER` |
