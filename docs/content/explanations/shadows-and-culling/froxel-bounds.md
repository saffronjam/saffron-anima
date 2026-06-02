+++
title = 'Froxel bounds'
weight = 7
math = true
+++

# Froxel bounds

A froxel bound is the view-space axis-aligned bounding box of one cluster in the froxel grid.

A froxel is a frustum voxel: a screen tile extruded between two depth planes. Because it lives in a perspective frustum, its true shape is a truncated pyramid rather than a box. The bound is the box that encloses that volume. The [light culling](../clustered-light-culling/) sphere test uses that box to decide whether a light touches the cluster, and the cull shader builds it from the froxel's screen tile and its two exponential-Z planes.

## From cluster index to screen tile

The flat cluster index unpacks into 3D grid coordinates with the encoding `index = x + y·gridX + z·gridX·gridY`. The X/Y pair gives a screen tile. Tiles split the screen evenly, so the tile's min and max screen-pixel corners are the grid coordinate times the tile size:

```hlsl
float2 tileSize = float2(params.screenSize.xy) / float2(params.gridSize.xy);
float2 minSS = float2(float(gx),     float(gy))     * tileSize;
float2 maxSS = float2(float(gx + 1), float(gy + 1)) * tileSize;
```

## Screen pixels to view-space rays

`screenToView` maps a pixel to NDC, multiplies by `inverseProjection`, and does the perspective divide. Both tile corners are unprojected at the near plane (`ndcZ = 0`), giving two view-space points on the camera's near plane. The eye sits at the view-space origin, so the line from the origin through each point is the frustum edge ray for that screen corner.

## Slicing the rays at the Z planes

The cluster's depth extent comes from the exponential Z formula, which [light culling](../clustered-light-culling/) covers. Each near-plane corner ray is intersected with the two planes $z = z_\text{near}$ and $z = z_\text{far}$. `rayToZ` scales the point so its $z$ lands on the target plane: for an eye-origin ray through $P$, the point at depth $z_d$ is $P \cdot (z_d / P_z)$.

```hlsl
float3 rayToZ(float3 p, float zDist) { return p * (zDist / p.z); }
```

Two corner rays × two Z planes gives four view-space points (the near-quad and far-quad corners). The box is the component-wise min and max of those four.

```mermaid
flowchart TD
    A[cluster index] --> B[unpack gx, gy, gz]
    B --> C[tile min/max screen px]
    C --> D[screenToView at near plane<br/>two corner rays]
    B --> E[exponential tileNear / tileFar]
    D --> F[rayToZ: 4 froxel corners]
    E --> F
    F --> G[min / max = view-space AABB]
```

## Design and trade-offs

The box is a loose fit. The near quad is smaller than the far quad, so the axis-aligned box that encloses all four corners is slightly larger than the true truncated pyramid. The over-estimate means a light can be added to a froxel it does not strictly overlap, so the cull keeps a few extra lights and never misses one. For a visibility cull this is the right bias: a missed light is a visible artifact, while an extra light costs only a little wasted shading. Testing the actual froxel planes would trade more compute for shorter light lists; the AABB is the cheap choice that is correct by over-inclusion.

## In the code

| What | File | Symbols |
|---|---|---|
| Unpack index → grid coords | `light_cull.slang` | `computeMain` (`gx`/`gy`/`gz`) |
| Screen tile corners | `light_cull.slang` | `computeMain` (`tileSize`, `minSS`/`maxSS`) |
| Unproject to view space | `light_cull.slang` | `screenToView` |
| Slice rays at Z planes | `light_cull.slang` | `rayToZ`, `tileNear`/`tileFar` |
| The view-space AABB | `light_cull.slang` | `aabbMin`/`aabbMax` |
| view + inverseProjection inputs | `renderer_lighting.cpp` | `ClusterParams` |

## Related

- [Light culling](../clustered-light-culling/) — the sphere-vs-AABB test these bounds feed
- [Clustered forward](../../lighting-and-brdf/clustered-forward/) — the lighting model behind it
