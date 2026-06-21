+++
title = 'Mesh upload'
weight = 5
+++

# Mesh upload

Mesh upload moves a CPU-side [`Mesh`](../mesh-and-vertex-layout/) into device-local Vulkan
buffers the renderer can draw. The result is a `GpuMesh`: vertex and index buffers in GPU
memory, an optional skin-stream buffer, plus the metadata a draw and a ray pick need.

The fastest memory for the GPU to read is device-local, which the CPU cannot write directly.
Upload therefore copies the data through an intermediate host-visible staging buffer, and
computes the mesh's local-space bounding box along the way for picking and shadow fitting.
`Uploader::upload_mesh` is the engine's single entry point for this.

## Stage, then copy

The data path is the standard Vulkan staging pattern, kept to one staging buffer. The
vertex, index, and (optional) skin streams are packed back-to-back into a single
host-visible, mapped buffer through `bytemuck::cast_slice`:

```rust
let bytes = staging.mapped_slice();
bytes[..vb].copy_from_slice(bytemuck::cast_slice(&mesh.vertices));
bytes[vb..vb + ib].copy_from_slice(bytemuck::cast_slice(&mesh.indices));
if !skin.is_empty() {
    bytes[vb + ib..].copy_from_slice(bytemuck::cast_slice(skin));
}
staging.flush();
```

Device-local buffers are created — one `VERTEX_BUFFER`, one `INDEX_BUFFER`, and the optional
skin buffer (all `TRANSFER_DST`) — and `cmd_copy_buffer` fans the staging buffer out into
them. The copies record on a one-off command buffer through `with_one_off_commands`, which
submits on the graphics queue and blocks on a per-submit fence (`wait_for_fences`) before the
staging buffer is dropped. Mesh upload is an import-time operation, not a per-frame one, so
the synchronous wait is acceptable and keeps the staging lifetime trivially correct. There is
no async transfer queue.

The `vk::` calls here are `ash` (Rust Vulkan bindings) plus `vk_mem` (the VMA allocator
binding); the `unsafe` block is confined to the ash command-recording seam.

## What a GpuMesh holds

```rust
pub struct GpuMesh {
    // device-local vertex + index buffers (+ allocations), optional skin buffer
    pub index_count: u32,
    pub vertex_count: u32,
    pub submeshes: Vec<Submesh>,
    pub bounds_min: Vec3,            // local-space AABB
    pub bounds_max: Vec3,
    pub cpu_positions: Vec<Vec3>,    // retained for triangle-precise picking
    pub cpu_indices: Vec<u32>,
    pub cpu_skin: Vec<VertexSkin>,   // empty when unskinned
    pub blas: Option<Arc<AccelerationStructure>>,  // None when RT unsupported
}
```

It is shared as an `Arc<GpuMesh>` so several entities can share one upload, and its `Drop`
frees the VMA allocations and returns any bindless slots. The asset server drops its `Arc`s
under an idle GPU before the allocator is destroyed. Submesh ranges are copied straight off
the source `Mesh`, so the [draw loop](../draw-list/) reads them off the GPU mesh directly,
and the CPU position/index copies stay resident for triangle-precise picking.

## The bounds

Upload sweeps every vertex position to find the local-space axis-aligned bounding box, stored
once on the `GpuMesh`. [Picking](../../scene-and-ecs/picking/) transforms the box into world
space for its ray test, and `render_scene` accumulates it into the scene AABB that fits the
[directional shadow](../../shadows-and-culling/directional-shadows/) frustum. Computing it at
upload time means neither caller re-scans vertices.

## The RT branch

When ray tracing is supported, the vertex and index buffers gain `SHADER_DEVICE_ADDRESS` and
`ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_KHR` usage so they can feed a BLAS build, and
`build_mesh_blas` builds the mesh's BLAS once and stores it. A BLAS-build failure is logged,
not fatal — the mesh renders without RT shadows. On hardware without RT support none of this
runs and `blas` stays `None`. The geometry buffers are identical either way. A skinned mesh
additionally gives its vertex and skin streams `STORAGE_BUFFER` usage, since the compute
skinning prepass reads them as storage buffers.

## In the code

| What | File | Symbols |
|---|---|---|
| The upload | `rendering/src/upload.rs` | `Uploader::upload_mesh` |
| GPU mesh type | `rendering/src/resources.rs` | `GpuMesh`, `GpuMeshParts` |
| BLAS build (RT) | `rendering/src/upload.rs` | `build_mesh_blas` |
| Upload seam | `assets/src/gpu.rs` | `GpuUploader::upload_mesh` |
| Bounds consumers | `assets/src/render_scene.rs` | `render_scene`, `pick_entity` |

## Related

- [Vertex layout](../mesh-and-vertex-layout/) — the `Mesh` this consumes
- [Draw list](../draw-list/) — how the `GpuMesh` and its submeshes are drawn
- [Picking](../../scene-and-ecs/picking/) — uses the local AABB and CPU copies
- [Directional shadows](../../shadows-and-culling/directional-shadows/) — fits to the scene AABB
