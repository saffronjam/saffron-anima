+++
title = 'Model import'
weight = 2
+++

# Model import

Two source formats, two third-party parsers, one output. `importGltf` drives cgltf and
`importObj` drives tinyobjloader, and both funnel into the same
[`Mesh`](../mesh-and-vertex-layout/) plus an `ImportedMaterial`. The format is picked by
file extension; the caller never sees which parser ran.

## Dispatch by extension

`importModelFile` (mesh only) and `importModelWithMaterial` (mesh + primary material) both
branch on a case-insensitive suffix check. An unknown extension is an `Err`, not a guess.
Both parsers run through their no-throw C-style surfaces, so a parse failure becomes an
`Err`, matching the engine's [error-as-value rule](../../core-and-conventions/error-handling/).

## glTF through cgltf

cgltf parses the JSON and loads the buffers in two calls; either failing returns `Err`. The
importer walks every mesh's triangle primitives and reads each into a fresh submesh.
Attributes are looked up by type: `POSITION` is required (a primitive without it is
skipped), `NORMAL` and `TEXCOORD_0` are optional. Vertices are read one at a time through
the accessor API, which handles whatever component type and stride the file used.

Each primitive gets a `vertexOffset` equal to the current vertex count, so its indices stay
zero-based against its own block. Indices are bounds-checked against the primitive's vertex
count, and an out-of-range index aborts with an `Err`. A primitive with no index buffer
gets a synthesized `0..vertexCount` sequence. One source mesh with several primitives
becomes several submeshes over the shared buffers.

## OBJ through tinyobjloader

`LoadObj` resolves the `.mtl` and its textures relative to the OBJ's own directory. OBJ
stores position, normal, and texcoord as three independent index streams, so the same
`(v, vn, vt)` triple can recur; a `std::map` keyed on that triple collapses duplicates into
unique vertices.

```cpp
const std::array<int, 3> key{ index.vertex_index, index.normal_index, index.texcoord_index };
auto it = uniqueVertices.find(key);
```

One OBJ shape becomes one submesh. Because the indices already point into the shared array,
OBJ submeshes leave `vertexOffset` at 0, the opposite choice from glTF. One correctness fix
lives here: OBJ's texture V origin is bottom-left while Vulkan samples top-left, so the
importer flips V on read (`1.0f - v`). glTF needs no flip.

## Missing normals

Both paths share a fallback. `anyNormalsPresent` scans the assembled mesh, and if every
normal is near-zero, `generateNormals` recomputes smooth per-vertex normals by summing the
cross-product face normals of each triangle and normalizing. A vertex with no contributing
face falls back to `+Y`.

## The primary material

Both importers extract one material, the primary one, into an `ImportedMaterial`:

```cpp
struct ImportedMaterial
{
    glm::vec4 baseColor{ 1.0f };
    std::vector<u8> albedoBytes;   // encoded png/jpg, not decoded here
    std::string albedoExt;
    bool hasAlbedo = false;
};
```

glTF reads `pbr_metallic_roughness.base_color_factor` and the base-color texture, which can
come from an embedded buffer view or an external file resolved next to the glTF. OBJ reads
the first non-negative material id, taking `diffuse` as the base color and
`diffuse_texname` as the albedo file. The encoded bytes are carried as-is; decoding happens
later in [image decoding](../image-decoding/).

## In the code

| What | File | Symbols |
|---|---|---|
| Extension dispatch | `geometry.cppm` | `importModelFile`, `importModelWithMaterial` |
| glTF parse + walk | `geometry.cppm` | `importGltfModel`, `importGltf` |
| OBJ parse + dedup | `geometry.cppm` | `importObjModel`, `importObj` |
| Missing-normal fallback | `geometry.cppm` | `anyNormalsPresent`, `generateNormals` |
| Material extraction | `geometry.cppm` | `ImportedMaterial`, `extensionFromMime` |

> [!NOTE]
> Only the primary material is extracted, and `materialSlot` stays 0 on every submesh. A
> glTF with three differently-textured primitives imports as three submeshes that all share
> one material. Per-submesh multi-material is reserved, not wired; see
> [vertex layout](../mesh-and-vertex-layout/).

> [!NOTE]
> glTF albedo embedded as a `data:` URI is not yet decoded; the importer logs a warning and
> imports the geometry without that texture. Embedded buffer-view images and external files
> both work.

## Related

- [Vertex layout](../mesh-and-vertex-layout/) — the common output
- [Image decoding](../image-decoding/) — where the albedo bytes get decoded
- [Import pipeline](../import-pipeline/) — what calls these and bakes the result
- [Error handling](../../core-and-conventions/error-handling/) — the no-throw boundary
