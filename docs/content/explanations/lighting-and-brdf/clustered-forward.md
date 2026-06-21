+++
title = 'Clustered forward'
weight = 5
math = true
+++

# Clustered forward

Clustered forward (Forward+) is a forward rendering technique that bounds per-fragment lighting
cost by limiting each fragment to the lights that can reach it. It dices the view frustum into a
3D grid of froxels, a compute pass assigns each light to the froxels it touches, and the fragment
shader loops only the lights in its froxel.

A plain forward renderer loops every light per fragment, so a thousand lights cost a thousand
iterations per pixel even though most contribute nothing. Clustering replaces that loop with a
short per-froxel one.

## The froxel grid

The grid is $16 \times 9 \times 24 = 3456$ clusters. The $16 \times 9$ tiles the screen; the
24 slices it in depth. Depth slicing is exponential in view space, not linear. Perspective packs
near geometry into a thin band of screen depth, so equal-thickness slices would waste resolution
far away and starve it up close. Slice $i$ spans the view-space Z planes

$$
z_i = -n\left(\frac{f}{n}\right)^{i/N}, \qquad i = 0 \dots N
$$

where $n$ and $f$ are the near and far planes and $N$ is the slice count. Z is negative
because the camera looks down $-Z$. The cull shader builds each slice with `pow(far/near, ...)`;
the fragment shader inverts the same mapping with a `log` — see [cluster indexing](../cluster-indexing/).

## The cull pass

`light_cull.slang` runs one invocation per cluster (a flat `[numthreads(64,1,1)]` dispatch of
`ceil(CLUSTER_COUNT / 64)` groups). Each invocation unpacks its `(x, y, z)` grid coordinate and
builds the cluster's view-space AABB by back-projecting the screen tile's corners onto the near
plane, then intersecting those eye rays with the slice's two Z planes. It then tests every light
as a sphere-vs-box check:

```hlsl
float3 closest = clamp(posView, aabbMin, aabbMax);   // nearest box point to the light
float3 delta = posView - closest;
if (dot(delta, delta) <= radius * radius)            // sphere overlaps box
{
    if (count < MAX_LIGHTS_PER_CLUSTER)
    {
        clusters[clusterIndex].indices[count] = i;
        count = count + 1;
    }
}
```

The light's bounding radius is its `range`. This is exact because punctual
[attenuation](../punctual-lights-and-attenuation/) is windowed to reach zero at `range`, so a
light contributes nothing outside its sphere. The result per froxel is a `Cluster`: a `count`
plus a fixed array of light indices. The same froxel-assignment math is mirrored as pure CPU
functions in `lighting.rs` (`cluster_aabb`, `light_intersects_cluster`, `cull_clusters_cpu`) so
the cull is unit-testable with no device.

```mermaid
flowchart LR
    A[light SSBO<br/>set 1, binding 1] --> C[light-cull<br/>compute pass]
    P[ClusterParams<br/>view + invProj + zPlanes] --> C
    C -->|StorageWriteCompute| B[cluster buffer<br/>per-froxel index lists]
    B -->|StorageReadFragment| S[scene fragment<br/>loops its cluster only]
```

## How it slots into the frame

The cull pass is added to the [render graph](../../frame-and-render-graph/render-graph-overview/)
in `record_scene_graph`, before the scene pass, when a cull dispatch is armed (clustered mode on
and at least one light — `take_cluster_dispatch_pending`). It declares the cluster buffer with
`RgUsage::StorageWriteCompute`; the scene pass declares it as a sampled storage read. The graph
derives the compute→fragment barrier from those two declarations, no hand-written pipeline
barrier. The same light SSBO is bound into both the cull set and the fragment lighting set, so
growing it rewrites both.

## Why it stays correct

The cull is an optimization, not a different lighting model. It changes which lights a fragment
iterates, never how a light is shaded; both this loop and the
[brute-force loop](../brute-force-fallback/) call the same `punctual`/`brdf` functions. A light
is added to a cluster only when its `range` sphere overlaps the froxel, and that same `range`
makes its contribution zero everywhere else, so a fragment never misses a light that would have
lit it. The two paths are therefore pixel-identical, and `sa set-clustered 0` is a verified A/B.
A GPU-runtime test cross-checks the dispatch against the `cull_clusters_cpu` oracle.

## In the code

| What | File | Symbols |
|---|---|---|
| Cull kernel | `engine/assets/shaders/light_cull.slang` | `computeMain`, `screenToView`, `rayToZ` |
| Grid + cap constants | `engine/crates/rendering/src/lighting.rs` | `CLUSTER_GRID_X`/`_Y`/`_Z`, `CLUSTER_COUNT`, `MAX_LIGHTS_PER_CLUSTER` |
| CPU mirror of the cull | `engine/crates/rendering/src/lighting.rs` | `cluster_aabb`, `light_intersects_cluster`, `cull_clusters_cpu` |
| Cluster params upload | `engine/crates/rendering/src/lighting.rs` | `Lighting::set_cluster_camera`, `ClusterParams`, `take_cluster_dispatch_pending` |
| Pass scheduling + barrier | `engine/crates/rendering/src/renderer.rs` | `Renderer::record_scene_graph` — the `light-cull` `RgPass::compute` |
| Fragment-side loop | `engine/assets/shaders/lighting.slang` | `evalLighting` — `clusterParams.screenSize.z` branch |

> [!TIP]
> The grid dims and `MAX_LIGHTS_PER_CLUSTER` are duplicated in `light_cull.slang`,
> `lighting.slang`, and `lighting.rs`. They must stay in lockstep — the cluster index
> encoding $x + y\,G_x + z\,G_x G_y$ only matches across passes if all three agree. A unit
> test (`cluster_grid_matches_shader`) pins the Rust constants to the shader.

## Related

- [Cluster indexing](../cluster-indexing/) — how a fragment finds its froxel
- [Per-cluster cap](../per-cluster-cap/) — the 64-light ceiling per froxel
- [Brute-force fallback](../brute-force-fallback/) — the pixel-identical reference path
- [Render graph](../../frame-and-render-graph/render-graph-overview/) — how the barrier is derived
