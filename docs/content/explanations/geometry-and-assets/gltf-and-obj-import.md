+++
title = 'Model import'
weight = 2
+++

# Model import

Model import reads a 3D model file and converts it into the engine's own geometry: a
[`Mesh`](../mesh-and-vertex-layout/) plus a table of `ImportedMaterial`s, one per source
material. Two source formats are supported — glTF and OBJ — and each has its own parser, but
both produce the same output.

The format is chosen by file extension, and the caller never sees which parser ran. glTF
goes through cgltf, OBJ through tinyobjloader; both run on their no-throw C-style surfaces,
so a parse failure becomes an `Err` rather than an exception, matching the engine's
[error-as-value rule](../../core-and-conventions/error-handling/).

## Dispatch by extension

`translateModel` parses a model file and returns a mesh plus its material table. It branches on a
case-insensitive suffix check; an unknown extension returns an `Err`.

## glTF through cgltf

cgltf parses the JSON and loads the buffers in two calls; either failing returns `Err`. The
importer walks every mesh's triangle primitives and reads each into a fresh submesh.
Attributes are looked up by type:

- `POSITION` is required; a primitive without it is skipped.
- `NORMAL` and `TEXCOORD_0` are optional.

Vertices are read one at a time through the accessor API, which handles whatever component
type and stride the file used. Each primitive gets a `vertexOffset` equal to the current
vertex count, so its indices stay zero-based against its own block. Indices are
bounds-checked against the primitive's vertex count, and an out-of-range index aborts with
an `Err`. A primitive with no index buffer gets a synthesized `0..vertexCount` sequence. One
source mesh with several primitives becomes several submeshes over the shared buffers, and
each submesh's `materialSlot` is set to the slot of its glTF material (deduplicated in
first-seen order).

## OBJ through tinyobjloader

`LoadObj` resolves the `.mtl` and its textures relative to the OBJ's own directory. OBJ
stores position, normal, and texcoord as three independent index streams, so the same
`(v, vn, vt)` triple can recur; a `std::map` keyed on that triple collapses duplicates into
unique vertices.

```cpp
const std::array<int, 3> key{ index.vertex_index, index.normal_index, index.texcoord_index };
auto it = uniqueVertices.find(key);
```

An OBJ shape can mix materials across its faces, so the importer groups faces by their
`material_id` (tinyobj triangulates by default, giving one id per triangle) and emits one
submesh per material, each tagged with its slot. Because the indices already point into the
shared array, OBJ submeshes leave `vertexOffset` at 0, the opposite choice from glTF. OBJ's
texture V origin is bottom-left while Vulkan samples top-left, so the importer flips V on
read (`1.0f - v`). glTF needs no flip.

## Missing normals

Both paths share a fallback. `anyNormalsPresent` scans the assembled mesh, and if every
normal is near-zero, `generateNormals` recomputes smooth per-vertex normals by summing the
cross-product face normals of each triangle and normalizing. A vertex with no contributing
face falls back to `+Y`.

## Skeletal clips

When a glTF declares a skin, the importer also walks `data->animations`. Each animation
becomes an `AnimClip`, and each of its channels a track: the channel's target node is matched
to a joint by its position in the skin's joint list, and its sampler's keyframe times and
values are read through the same accessor API into the flat arrays the sampler expects. A
track records both that joint index and the node's name, so a later reimport can re-resolve a
stale index. Channels that target a non-joint node, drive morph-target weights, or use a
sparse accessor are skipped in v1 (logged, not silently dropped).

The decoded clips ride along on `ImportedModel.animations`; the
[import pipeline](../import-pipeline/) bakes each to a [`.sanim`](../sanim-format/) sidecar and
registers it as an `AssetType::Animation` catalog entry. The mechanics of the clip and track
types are the [animation data model](../../animation/animation-data-model/).

## The material table

Both importers build a table of `ImportedMaterial`, one entry per distinct source material,
in first-seen order. Each `Submesh.materialSlot` indexes the table.

```cpp
struct ImportedMaterial
{
    glm::vec4 baseColor{ 1.0f };
    f32 metallic = 0.0f;
    f32 roughness = 1.0f;
    glm::vec3 emissive{ 0.0f };
    f32 emissiveStrength = 1.0f;
    std::vector<u8> albedoBytes;             // encoded png/jpg, not decoded here
    std::string albedoExt;
    bool hasAlbedo = false;
    std::vector<u8> metallicRoughnessBytes;  // glTF MR map (roughness=G, metalness=B); linear
    std::string metallicRoughnessExt;
    bool hasMetallicRoughness = false;
};
```

glTF reads each material's `base_color_factor`, `metallic_factor`, `roughness_factor`,
`emissive_factor` (with the `KHR_materials_emissive_strength` multiplier), the base-color
texture, and the **metallic-roughness texture** (`readGltfTextureBytes` handles both, from an
embedded buffer view or an external file resolved next to the glTF). OBJ reads `diffuse`,
`metallic`, `roughness`, `emission`, and `diffuse_texname` per material. The encoded texture
bytes are carried as-is; decoding happens later, in [image decoding](../image-decoding/).

The downstream [import pipeline](../import-pipeline/) registers each slot's textures — albedo
as sRGB, the metallic-roughness map as **linear** (an `eR8G8B8A8Unorm` upload, since those are
scalar values, not color) — and lowers the table into the scene: a single-material model
becomes one `MaterialComponent`, a multi-material model a
[`MaterialSetComponent`](../../scene-and-ecs/built-in-components/).

> [!NOTE]
> Normal and emissive *textures* are still not imported (the engine material has no slots for
> them), and OBJ's separate `map_Pm`/`map_Pr` maps are not combined into a metallic-roughness
> texture. Only glTF's packed metallic-roughness texture and the albedo texture cross over.

## In the code

| What | File | Symbols |
|---|---|---|
| Extension dispatch | `geometry.cppm` | `translateModel` |
| glTF parse + walk | `geometry.cppm` | `importGltfModel` |
| Skeletal clip decode | `geometry.cppm` | `importGltfModel`, `AnimClip`, `AnimTrack` |
| OBJ parse + dedup | `geometry.cppm` | `importObjModel` |
| Missing-normal fallback | `geometry.cppm` | `anyNormalsPresent`, `generateNormals` |
| Material extraction | `geometry.cppm` | `ImportedMaterial`, `extractGltfMaterial`, `readGltfTextureBytes` |

> [!NOTE]
> glTF albedo embedded as a `data:` URI is not yet decoded; the importer logs a warning and
> imports the geometry without that texture. Embedded buffer-view images and external files
> both work.

## Related

- [Vertex layout](../mesh-and-vertex-layout/) — the common output
- [Image decoding](../image-decoding/) — where the albedo bytes get decoded
- [Import pipeline](../import-pipeline/) — what calls these and bakes the result
- [Error handling](../../core-and-conventions/error-handling/) — the no-throw boundary
