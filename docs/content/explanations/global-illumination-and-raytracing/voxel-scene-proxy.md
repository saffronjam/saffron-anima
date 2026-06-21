+++
title = 'Voxel proxy'
weight = 2
+++

# Voxel proxy

A voxel proxy is a coarse 3D grid that stands in for the scene's real geometry during ray tracing.
DDGI rays never trace triangles; they march through this grid, where each occupied voxel stores the
albedo of whatever fills it. Because the grid is a software image, the whole DDGI trace runs on any
GPU without ray-tracing hardware.

The proxy is rebuilt from scratch every frame, so it is never stale. It is a `32³` volume — small
enough to voxelize cheaply, coarse enough that it captures only the low-frequency information a
diffuse bounce needs.

## The 3D image

The proxy is an `Image3D` — a VMA-allocated 3D image, kept as a distinct RAII wrapper from the 2D
`Image` type. It is created once at `DDGI_VOXEL_RES`³ (`32³`) in `rgba16f`, with storage *and*
sampled usage so the same image can be written by the voxelize pass and read by the trace
(`Image3D::new`, `vk::ImageType::TYPE_3D`, `Storage | Sampled` usage).

## Rasterizing AABBs, not triangles

`ddgi_voxelize.slang` runs one thread per voxel, a `4×4×4` thread group over the `32³` grid. Each
thread computes its voxel's world-space center and tests it against a small SSBO of per-draw world
AABBs. Inside any box, the voxel stores that box's albedo with occupancy `a = 1`; otherwise it
clears to `a = 0`.

The trade is exactness for cost. A box voxelizes perfectly, but an arbitrary mesh gets a coarse AABB
fill that is too solid — a sphere becomes a cube. For a *global* diffuse GI proxy that is acceptable:
the indirect bounce is low-frequency, and the surface itself is shaded with real geometry.
Conservative triangle rasterization into the grid would cost far more for a term that is blurred
across probes.

## Where the boxes come from

`render_scene` walks the scene's `Transform` + `Mesh` component entities (queried through the
`hecs` registry). For each, it transforms the mesh's local AABB corners into world space to get a
world AABB plus the material base color. Those values go into three parallel arrays handed to
`set_ddgi_scene`, which interleaves them as `[min, max, albedo]` into the mapped box SSBO.

## Fitting the volume

`set_ddgi_scene` also receives a volume placement, computed in `render_scene` from the scene's
overall world AABB padded by one unit:

```rust
let pad = Vec3::ONE;
let vol_min = scene_min - pad;
let vol_ext = (scene_max + pad) - vol_min;
```

The `32³` voxels and the `8×4×8` probes both span this volume, so a voxel's world size is
`volumeExtent / 32` and probe spacing is `volumeExtent / probeCount`. When the scene moves the volume
re-fits next frame, because the proxy is never cached.

## Layout in the graph

The voxelize pass declares the 3D image as `RgUsage::StorageImageRwCompute`, written in `GENERAL`.
The trace pass reads it through the *same* RW-storage usage rather than a sampled read. This is
deliberate: the graph's `RgUsage::StorageReadCompute` usage is modeled for buffers (layout
`Undefined`) and would mis-transition a 3D image, so the voxels stay in `GENERAL` across both passes
and the
[render graph](../../frame-and-render-graph/render-graph-overview/) inserts a plain write→read memory
barrier.

## In the code

| What | File | Symbols |
|---|---|---|
| Voxelize shader | `ddgi_voxelize.slang` | `computeMain`, `Box`, the AABB test |
| 3D image type | `rendering/src/resources.rs` | `Image3D` |
| 3D image allocation | `rendering/src/resources.rs` | `Image3D::new` |
| Box upload + volume fit | `rendering/src/ddgi.rs` | `Ddgi::set_scene`; `renderer.rs` · `Renderer::set_ddgi_scene` |
| World AABBs from the scene | `assets/src/render_scene.rs` | `render_scene`, `gather_static_draw_list` (corner-transform loop) |
| Voxelize graph pass | `rendering/src/renderer.rs` | `ddgi-voxelize` pass (`Renderer::add_ddgi_passes`) |

> [!WARNING]
> The voxelize loop is `O(voxels × boxes)` — every voxel tests every box linearly, with no spatial
> acceleration. At `32³` voxels that is fine for a few dozen draws. The box SSBO is capped at
> `Ddgi::box_capacity` (`DDGI_MAX_BOXES`); extra draws past the cap are dropped from the proxy (they still shade normally,
> they just don't contribute indirect bounce).

## Related

- [DDGI overview](../ddgi-overview/) — where the proxy sits in the per-frame pipeline
- [Software ray trace](../software-ray-trace/) — what marches through these voxels
- [Render graph](../../frame-and-render-graph/render-graph-overview/) — how the 3D-image barriers are derived
