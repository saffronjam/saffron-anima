+++
title = 'Vertex layout'
weight = 1
+++

# Vertex layout

A vertex layout is the fixed memory format of a single mesh vertex: which attributes it
carries, in what order, and at what total stride. Saffron uses one CPU-side mesh type,
`Mesh`, and one 32-byte vertex struct for every importer.

A single fixed layout lets one mesh pipeline, one `.smesh` on-disk stride, and one upload
path serve glTF and OBJ alike. The format is the same in memory, on disk, and on the GPU.

## The vertex

A vertex is position, normal, and one UV channel, nothing more:

```cpp
struct Vertex
{
    glm::vec3 position{ 0.0f };
    glm::vec3 normal{ 0.0f };
    glm::vec2 uv0{ 0.0f };
};
static_assert(sizeof(Vertex) == 32, "Vertex must stay 32 bytes (the .smesh on-disk stride)");
```

The `static_assert` is load-bearing. The [`.smesh` format](../smesh-format/) writes the
vertex array as a raw byte blob and the loader reads it straight back, so the in-memory
stride is the disk stride. Adding a member without bumping the format version would
misalign every baked mesh on disk.

Tangents are absent, deferred to material time. Normal-mapped PBR needs a tangent basis,
but that is a later phase; adding it now would widen the stride for geometry that does not
use it.

## Mesh and submeshes

A `Mesh` is three flat vectors: one shared vertex buffer, one shared index buffer, and a
list of `Submesh` ranges over them.

```cpp
struct Mesh
{
    std::vector<Vertex> vertices;
    std::vector<u32> indices;
    std::vector<Submesh> submeshes;
};
```

A `Submesh` is one `drawIndexed` call's worth of arguments:

```cpp
struct Submesh
{
    u32 firstIndex = 0;
    u32 indexCount = 0;
    i32 vertexOffset = 0;   // signed, matching vkCmdDrawIndexed
    u32 materialSlot = 0;   // reserved (0) until per-submesh materials
};
static_assert(sizeof(Submesh) == 16, "Submesh must stay 16 bytes (baked directly into .smesh)");
```

`vertexOffset` is signed because that is the type `vkCmdDrawIndexed` takes. The glTF
importer sets it per primitive so each primitive's indices stay zero-based against its own
vertex block; the OBJ importer leaves it at 0 and emits indices already relative to the
shared array. Indices are 32-bit throughout, and the loader rejects any file whose
`indexWidth` is not `sizeof(u32)`.

## Why submeshes

A submesh is one `drawIndexed` call's worth of arguments over the mesh's shared buffers, so
one logical model can carry several draw ranges. The draw path loops every batch's
`mesh->submeshes` and issues one `drawIndexed` per submesh. A model with three glTF
primitives becomes three draw calls against one bound buffer pair.

A submesh does not select a material. `materialSlot` is reserved at 0 and the draw path
ignores it; material comes from the per-entity
[`MaterialComponent`](../../scene-and-ecs/built-in-components/), applied to the whole mesh.

## In the code

| What | File | Symbols |
|---|---|---|
| Vertex + stride assert | `geometry.cppm` | `Vertex` |
| Mesh + submesh | `geometry.cppm` | `Mesh`, `Submesh` |
| Normal regeneration | `geometry.cppm` | `generateNormals` |
| GPU side | `renderer_types.cppm` | `GpuMesh` |
| Per-submesh draw loop | `renderer_drawlist.cpp` | `recordSceneDrawList` |

> [!NOTE]
> `materialSlot` is reserved but not wired anywhere. Both importers write
> `materialSlot = 0`, and the draw path keys material off the entity's `MaterialComponent`,
> not the submesh. Multi-material meshes are a data-model seam, not a working feature. See
> the [import](../gltf-and-obj-import/) and [draw-list](../draw-list/) pages for where it
> stops.

## Related

- [Model import](../gltf-and-obj-import/) — what fills these vectors
- [.smesh format](../smesh-format/) — why the stride asserts matter
- [Mesh upload](../gpu-mesh-upload/) — `Mesh` → `GpuMesh`
- [Built-in components](../../scene-and-ecs/built-in-components/) — where material actually lives
