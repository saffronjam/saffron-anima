+++
title = 'Acceleration structures'
weight = 6
+++

# Acceleration structures

An acceleration structure is a GPU spatial index that lets a ray find the triangle it hits without
testing every triangle in the scene. Hardware ray tracing traces real triangles rather than a voxel
proxy, and a ray query needs this index to run in sublinear time.

The structure is two-level. A per-mesh **BLAS** (bottom-level) holds a mesh's triangles and is built
once on upload. A per-frame **TLAS** (top-level) holds the scene's instances and is rebuilt every
frame. A ray query enters the TLAS, walks each instance down into its mesh, and tests the triangles
in that mesh's BLAS. Both structures are owned as move-only RAII resource wrappers and reference
their data by buffer device address.

> [!NOTE]
> The RT path is feature-gated (see [device gating](../raytracing-device-gating/)). On the
> software dev GPU it runs at roughly 1 FPS, so it is correctness-validated and waits on real
> ray-tracing hardware.

## The acceleration-structure resource

A BLAS and a TLAS share one `AccelerationStructure` type: a move-only wrapper owning the
`vk::AccelerationStructureKHR` handle, its backing device buffer, and its device address. Like
every RAII GPU resource it frees itself in `Drop` — handle then buffer, before the allocator. It
destroys through a cloned `ash::khr::acceleration_structure::Device` dispatch, present only when RT
is enabled, because the destroy entry point is not part of core Vulkan.

`AccelerationStructure::create` is the shared constructor. It allocates the storage buffer with
`ACCELERATION_STRUCTURE_STORAGE_KHR` and `SHADER_DEVICE_ADDRESS` usage, calls
`create_acceleration_structure`, then fetches the AS device address with
`get_acceleration_structure_device_address`. The caller records the build separately.

## One BLAS per mesh, built on upload

`record_mesh_blas_build` builds a bottom-level AS over a `GpuMesh`'s whole vertex/index buffer as a
single triangles geometry. The vertex and index data are passed by device address, which is why the
allocator always carries the buffer-device-address flag.

The build queries its sizes, allocates the AS plus a scratch buffer, and records the build on a
one-off command buffer with an idle wait. This is the same synchronous shape as mesh upload, since
it happens once at load. It uses `PREFER_FAST_TRACE` and no compaction, favouring correctness for
v1. The result is stored as `GpuMesh::blas`, an `Option<Arc<AccelerationStructure>>` that is `None`
when RT is unsupported.

## One TLAS per frame, over the instances

The TLAS references the BLASes by their device address, one `vk::AccelerationStructureInstanceKHR`
per drawn mesh instance. `render_scene` hands the frame's model matrices and meshes to
`set_rt_scene`, and a graph compute pass replays a build plan from `prepare_tlas_build`, which packs
the instances and records the build into the frame's command buffer. `make_instance` builds each
instance, and `transform_rows` transposes the model matrix into the row-major 3×4 the
`vk::TransformMatrixKHR` expects:

```rust
// vk::TransformMatrixKHR is row-major 3x4; glam Mat4 is column-major — transpose into rows.
let rows = transform_rows(&model);
let instance = make_instance(rows, custom_index, mask, blas_address);
```

The instance buffer is host-visible, ping-ponged per in-flight frame, and grown to the next power
of two when the instance count outgrows it (`ensure_tlas_capacity`). The TLAS and its scratch are
recreated only when capacity changes; otherwise the same TLAS is rebuilt in place. After the build,
`write_mesh_set` writes the new TLAS into the frame's descriptor set (set 6) and the recorded plan
emits the AS-build to fragment-shader barrier itself.

## The empty-TLAS seed

The mesh fragment statically references the TLAS in set 6 regardless of the runtime flag, so the
descriptor must always point at a valid AS. `seed_empty_tlas` builds a zero-instance TLAS at init
and writes it into every frame's set. The first real per-frame build overwrites a slot on demand;
until then, ray queries against the empty TLAS miss.

## In the code

| What | File | Symbols |
|---|---|---|
| The AS resource | `rendering/src/resources.rs` | `AccelerationStructure`, `AccelerationStructure::create` |
| RT sub-state + scene capture | `rendering/src/rt.rs` | `Rt`, `RtScene`, `Rt::set_rt_scene` |
| Per-mesh BLAS | `rendering/src/rt.rs` | `record_mesh_blas_build`, `MeshBlasBuild`; `upload.rs` (call site) |
| Per-frame TLAS | `rendering/src/rt.rs` | `prepare_tlas_build`, `record_tlas_build_plan`, `ensure_tlas_capacity`, `seed_empty_tlas` |
| Instance packing | `rendering/src/rt.rs` | `make_instance`, `transform_rows` |
| Frame instance capture | `assets/src/render_scene.rs` | `render_scene` (the `set_rt_scene` call) |
| TLAS-build graph pass | `rendering/src/renderer.rs` | `tlas-build` pass |

> [!WARNING]
> `vk::TransformMatrixKHR` is a row-major 3×4; glam's `Mat4` is column-major. `transform_rows`
> transposes each model matrix into rows when packing instances; without that transpose, instances
> render at the wrong transform or mirrored. The BLAS build is also a synchronous idle wait per mesh,
> fine for load-time upload but a stall if ever called per frame.

## Related

- [RT device gating](../raytracing-device-gating/) — how RT support is detected and the entry points resolved
- [Ray-query shadows](../ray-query-shadows/) — the first consumer of the TLAS
- [ReSTIR](../restir-overview/) — the second, for its one visibility ray
- [Software ray trace](../software-ray-trace/) — the DDGI path that needs none of this
