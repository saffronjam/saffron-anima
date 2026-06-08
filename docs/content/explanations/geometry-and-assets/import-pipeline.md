+++
title = 'Import pipeline'
weight = 7
+++

# Import pipeline

The import pipeline is the write side of the asset system: it turns an external file into a
project asset. An import copies or bakes the source into the project's asset directory,
uploads it to the GPU, and adds a named entry to the [catalog](../asset-server-and-catalog/).

Resolution — turning a cached id back into a `Ref` — is the read side. This page covers
import alone.

## Importing a model

`importModel` is the full chain from a source file to a ready-to-draw asset:

```cpp
auto model = importModelWithMaterial(path);          // parse glTF/OBJ
const Uuid meshId = newUuid();
const std::string relativePath = "meshes/" + std::to_string(meshId.value) + ".smesh";
saveMesh(model->mesh, assets.root + "/" + relativePath);   // bake
auto meshRef = uploadMesh(renderer, model->mesh);          // upload
putAsset(assets.catalog, AssetEntry{ meshId, uniqueName(...), AssetType::Mesh, relativePath });
assets.meshRefByUuid[meshId.value] = *meshRef;             // seed the cache
```

The steps run in order:

1. Parse the source through the [importer](../gltf-and-obj-import/).
2. Mint a UUID.
3. Bake the mesh to a [`.smesh`](../smesh-format/) named by that UUID.
4. Upload it to the GPU.
5. Add a catalog entry named by the source filename stem, deduped by `uniqueName`.
6. Seed the GPU cache so the just-uploaded `Ref` is reused rather than reloaded.

The on-disk asset is the baked `.smesh`. The source glTF/OBJ is read once and never
referenced again.

Each imported material's albedo bytes run through `registerTextureBytes`, and the resulting
slots are reported on the `ImportResult` as a material table (slot 0 mirrored into the legacy
`baseColor`/`albedoTexture` fields):

```cpp
struct ImportResult
{
    Uuid mesh;
    glm::vec4 baseColor{ 1.0f };
    Uuid albedoTexture;            // 0 == none
    std::vector<MaterialSlot> materials;  // the imported table
};
```

`importModel` does not spawn an entity or save the project; it only populates the catalog.
Spawning is a separate step: `spawnModel` builds the entity with a `MeshComponent`, then
`applyImportedMaterials` attaches either a `MaterialComponent` (one material) or a
`MaterialSetComponent` (more than one) from the `ImportResult`.

## Importing a texture

A standalone texture follows the same shape through `registerTextureBytes`, which
`importModel` also reuses for embedded albedo. It decodes the bytes to confirm they are a
valid image, uploads, then writes the original encoded bytes to disk:

```cpp
auto decoded = decodeImageFromMemory(encoded);
auto texture = uploadTexture(renderer, decoded->rgba.data(), decoded->width, decoded->height, true);
const std::string relativePath = "textures/" + std::to_string(id.value) + "." + extension;
// write `encoded` (not `decoded`) to disk
putAsset(assets.catalog, AssetEntry{ id, uniqueName(...), AssetType::Texture, relativePath });
```

The disk copy is the encoded PNG/JPG, so reloading it re-runs [the decode](../image-decoding/)
rather than storing bulky raw RGBA. The `true` argument requests sRGB, since albedo is
authored in sRGB. `importTexture` is the thin wrapper that reads a file off disk into bytes
and calls `registerTextureBytes` with the filename stem as the name.

## Deduplication

Deduplication is per import, not per file. Each import mints a fresh `newUuid()`, so importing
`cube.gltf` twice produces two catalog entries (`cube`, `cube (2)`), two `.smesh` files, and two
uploads. Deduplication happens at
resolve time: multiple entities referencing the same UUID share one GPU upload via the
cache. There is no content hashing to collapse two imports of identical bytes into one
asset.

## In the code

| What | File | Symbols |
|---|---|---|
| Model import | `assets.cppm` | `importModel`, `ImportResult` |
| Texture import | `assets.cppm` | `importTexture`, `registerTextureBytes` |
| Spawn from an import | `assets.cppm` | `spawnModel`, `spawnMesh`, `applyImportedMaterials` |
| Bake + upload it calls | `geometry.cppm`, `renderer_drawlist.cpp` | `saveMesh`, `uploadMesh`, `uploadTexture` |

## Related

- [Model import](../gltf-and-obj-import/) — the parse step
- [.smesh format](../smesh-format/) — what the bake produces
- [Image decoding](../image-decoding/) — the texture decode
- [Asset catalog](../asset-server-and-catalog/) — the read side
- [Project files](../project-serialization/) — persisting the catalog the import filled
