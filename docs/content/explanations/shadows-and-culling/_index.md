+++
title = 'Shadows & culling'
weight = 10
bookCollapseSection = true
+++

# Shadows & culling

Shadows and culling are the two visibility computations a forward renderer performs each frame. A shadow-casting light renders depth or distance from its own viewpoint; the mesh fragment then compares its position against that map to decide whether the light reaches it. Clustered culling partitions the view frustum into cells and assigns each cell only the lights that touch it, narrowing the per-fragment light loop to nearby lights.

## Pages

| Page | Covers | Code |
|---|---|---|
| `directional-shadows` | orthographic light view, 2D depth map, 3×3 PCF | `lighting.rs` · `set_directional_shadow`; `lighting.slang` · `pcfShadow` |
| `spot-light-shadows` | perspective light view, one shadowed spot, same PCF path | `lighting.rs` · `set_spot_shadow`; `lighting.slang` · `pcfShadow` |
| `point-light-cube-shadows` | 6-face cube of distance-to-light, distance comparison | `point_shadow.slang`; `lighting.slang` · `pointShadow` |
| `pcf-filtering` | comparison sampler, 3×3 kernel, off-map and beyond-far handling | `lighting.slang` · `pcfShadow` |
| `shadow-bias` | constant + slope bias, acne vs. peter-panning | `scene_pass.rs` · `record_shadow_depth`; `lighting.slang` · `pointShadow` bias |
| `clustered-light-culling` | the froxel grid, exponential Z, sphere-vs-AABB cull dispatch | `light_cull.slang` · `computeMain` |
| `froxel-bounds` | screen-tile bounds → view-space AABB per froxel | `light_cull.slang` · `screenToView`/`rayToZ` |
