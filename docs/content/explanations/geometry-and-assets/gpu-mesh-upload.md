+++
title = 'Mesh upload'
weight = 5
+++

# Mesh upload

Mesh upload moves a CPU-side [`Mesh`](../mesh-and-vertex-layout/) into device-local Vulkan
buffers the renderer can draw. The result is a `GpuMesh`: vertex and index buffers in GPU
memory, plus the metadata a draw needs.

The fastest memory for the GPU to read is device-local, which the CPU cannot write directly.
Upload therefore copies the data through an intermediate host-visible buffer, and computes
the mesh's local-space bounding box along the way for picking and shadow fitting. `uploadMesh`
is the engine's single entry point for this.

## Stage, then copy

The data path is the standard Vulkan staging pattern, kept to one staging buffer. Vertices
and indices are packed back-to-back into a single host-visible, mapped buffer:

```cpp
std::memcpy(stagingMapped.pMappedData, mesh.vertices.data(), vertexBytes);
std::memcpy((char*)stagingMapped.pMappedData + vertexBytes, mesh.indices.data(), indexBytes);
vmaFlushAllocation(allocator, stagingAllocation, 0, VK_WHOLE_SIZE);
```

Two device-local buffers are created, one `VERTEX_BUFFER` and one `INDEX_BUFFER` (both
`TRANSFER_DST`), and two `copyBuffer` commands fan the staging buffer out into them. The
copy is recorded on a one-time-submit command buffer, submitted on the graphics queue, and
the upload blocks on `device.waitIdle()` before the staging buffer is destroyed.

That `waitIdle` is a deliberate simplification. Mesh upload is an import-time operation, not
a per-frame one, so a full device idle is acceptable and keeps the staging lifetime trivially
correct. There is no async transfer queue.

## What a GpuMesh holds

```cpp
struct GpuMesh
{
    VmaAllocator allocator;            // borrowed
    vk::Buffer vertexBuffer; VmaAllocation vertexAlloc;
    vk::Buffer indexBuffer;  VmaAllocation indexAlloc;
    u32 indexCount; u32 vertexCount;
    std::vector<Submesh> submeshes;
    glm::vec3 boundsMin, boundsMax;    // local-space AABB
    Ref<AccelerationStructure> blas;   // null when RT unsupported
};
```

It is a move-only RAII wrapper, like `Pipeline` and `Image`, handed around as a
`Ref<GpuMesh>` so several entities can share one upload. The destructor frees the VMA
buffers, and the renderer drops its `Ref`s before the allocator is destroyed. Submesh
ranges are copied straight off the source `Mesh`, so the [draw loop](../draw-list/) reads
them off the GPU mesh directly.

## The bounds

Upload sweeps every vertex position to find the local-space axis-aligned bounding box. This
is computed once and stored on the `GpuMesh`. [Picking](../../scene-and-ecs/picking/)
transforms the box into world space for its ray test, and `renderScene` accumulates it into
the scene AABB that fits the
[directional shadow](../../shadows-and-culling/directional-shadows/) frustum. Computing it at
upload time means neither caller re-scans vertices.

## The RT branch

When ray tracing is supported, the vertex and index buffers gain `SHADER_DEVICE_ADDRESS` and
`ACCELERATION_STRUCTURE_BUILD_INPUT` usage so they can feed a BLAS build, and once the buffers
are live `uploadMesh` builds the mesh's BLAS once and stores it. On hardware without RT support
none of this runs and `blas` stays null. The geometry buffers are identical either way.

## In the code

| What | File | Symbols |
|---|---|---|
| The upload | `renderer_drawlist.cpp` | `uploadMesh` |
| GPU mesh type | `renderer_types.cppm` | `GpuMesh` |
| BLAS build (RT) | `renderer_drawlist.cpp` | `buildBlas` (in `uploadMesh`) |
| Bounds consumers | `assets.cppm` | `renderScene`, `pickEntity` |

## Related

- [Vertex layout](../mesh-and-vertex-layout/) — the `Mesh` this consumes
- [Draw list](../draw-list/) — how the `GpuMesh` and its submeshes are drawn
- [Picking](../../scene-and-ecs/picking/) — uses the local AABB
- [Directional shadows](../../shadows-and-culling/directional-shadows/) — fits to the scene AABB
