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
in that mesh's BLAS. Both structures are owned as RAII meta-layer resources and reference their data
by buffer device address.

> [!NOTE]
> The RT path is feature-gated (see [device gating](../raytracing-device-gating/)). On the
> software dev GPU it runs at roughly 1 FPS, so it is correctness-validated and waits on real
> ray-tracing hardware.

## The acceleration-structure resource

A BLAS and a TLAS share one `AccelerationStructure` type: a move-only wrapper owning the
`vk::AccelerationStructureKHR` handle, its backing device buffer, and its device address. Like
every [meta-layer resource](../../vulkan-foundation/) it frees itself in order, handle then
buffer, before the allocator. It destroys through a resolved function pointer because the destroy
entry point is not statically exported.

`createAccelStructure` is the shared constructor. It allocates the storage buffer with
`ACCELERATION_STRUCTURE_STORAGE` and `SHADER_DEVICE_ADDRESS` usage, calls
`vkCreateAccelerationStructureKHR`, then fetches the AS device address. The caller records the
build separately.

## One BLAS per mesh, built on upload

`buildBlas` builds a bottom-level AS over a `GpuMesh`'s whole vertex/index buffer as a single
triangles geometry. The vertex and index data are passed by device address, which is why those
buffers, and BDA on the allocator, are enabled when RT is supported.

The build queries its sizes with `getBuildSizes`, allocates the AS plus a scratch buffer, and
records the build on a one-off command buffer with a `waitIdle`. This is the same synchronous shape
as mesh upload, since it happens once at load. It uses `PREFER_FAST_TRACE` and no compaction,
favouring correctness for v1. The result is stored as `GpuMesh::blas`, a `Ref` that is null when RT
is unsupported.

## One TLAS per frame, over the instances

The TLAS references the BLASes by their device address, one `VkAccelerationStructureInstanceKHR`
per drawn mesh instance. `renderScene` hands the frame's model matrices and meshes to `setRtScene`,
and a graph compute pass calls `buildTlas`, which packs the instances and records the build into
the frame's command buffer:

```cpp
VkAccelerationStructureInstanceKHR inst{};
// VkTransformMatrixKHR is row-major 3x4; glm is column-major — transpose into rows.
for (u32 r = 0; r < 3; r++) for (u32 c = 0; c < 4; c++)
    inst.transform.matrix[r][c] = m[c][r];
inst.mask = 0xFF;
inst.accelerationStructureReference = meshes[i]->blas->address;
```

The instance buffer is host-visible, ping-ponged per in-flight frame, and grown to the next power
of two when the instance count outgrows it. The TLAS and its scratch are recreated only when
capacity changes; otherwise the same TLAS is rebuilt in place. After the build, `recordTlasBuild`
writes the new TLAS into the frame's descriptor set (set 6) and emits the AS-build to
fragment-shader barrier itself.

## The empty-TLAS seed

The mesh fragment statically references the TLAS in set 6 regardless of the runtime flag, so the
descriptor must always point at a valid AS. `seedEmptyTlas` builds a zero-instance TLAS at init and
writes it into every frame's set. The first real per-frame build overwrites a slot on demand; until
then, ray queries against the empty TLAS miss.

## In the code

| What | File | Symbols |
|---|---|---|
| The AS resource | `renderer_types.cppm` | `AccelerationStructure`, `Rt` |
| Create / size / address | `renderer_detail.cppm` | `createAccelStructure`, `makeRtBuffer`, `bufferDeviceAddress` |
| Per-mesh BLAS | `renderer_detail.cppm` | `buildBlas` |
| Per-frame TLAS | `renderer_detail.cppm` | `recordTlasBuild`, `ensureTlasCapacity`, `seedEmptyTlas` |
| Frame instance capture + build | `renderer.cppm` | `setRtScene`, `buildTlas`; `assets.cppm` · `renderScene` |
| TLAS-build graph pass | `renderer.cppm` | `tlas-build` pass |

> [!WARNING]
> `VkTransformMatrixKHR` is a row-major 3×4; GLM is column-major. `buildTlas` transposes each model
> matrix into rows when packing instances; without that transpose, instances render at the wrong
> transform or mirrored. The BLAS build is also a synchronous `waitIdle` per mesh, fine for load-time
> upload but a stall if ever called per frame.

## Related

- [RT device gating](../raytracing-device-gating/) — how RT support is detected and the entry points resolved
- [Ray-query shadows](../ray-query-shadows/) — the first consumer of the TLAS
- [ReSTIR](../restir-overview/) — the second, for its one visibility ray
- [Software ray trace](../software-ray-trace/) — the DDGI path that needs none of this
