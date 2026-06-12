+++
title = 'Import pipeline'
weight = 7
+++

# Import pipeline

The import pipeline is the write side of the asset system: it turns an external file into a
project asset. Importing a model bakes the source into one [`.smodel` container](../smodel-container/)
under the project's asset directory and adds the catalog entries it contributes. It does not touch
the GPU and does not spawn anything; placing the asset in a scene is a separate step.

Resolution — turning a cached id back into a `Ref` — is the read side. This page covers
import alone.

## Importing a model

`importModel` is the full chain from a source file to a stored asset:

```cpp
auto graph = translateModel(path);                       // parse glTF/OBJ into a format-neutral graph
auto bake = bakeModel(assets, *graph, options, path, Uuid{ 0 });   // write one .smodel
for (const AssetEntry& row : bake->rows) putAsset(assets.catalog, row);   // catalog the container + sub-assets
```

The steps run in order:

1. Parse the source through the [importer](../gltf-and-obj-import/) into a format-neutral graph.
2. Bake the graph into one `assets/models/<uuid>.smodel`: the mesh as a [`.smesh`](../smesh-format/)
   chunk, each material as a `.smat`-JSON chunk, each texture as a raw chunk (colorspace in the chunk
   flags), each animation clip as a `.sanim` chunk, and a metadata chunk holding the node hierarchy,
   skin, and the deterministic reimport recipe.
3. Add the catalog rows the container contributes: one `Model` row plus one row per embedded
   sub-asset, each linked back to the container by id and chunk index.

The on-disk asset is the single `.smodel`. The source glTF/OBJ is read once and never referenced
again, and nothing loose lands beside the container. This is the read→decide→build split — the
translator knows formats, `ImportOptions` owns the decisions, and the bake is the build.

## Placing a model

Import populates the catalog; it never spawns. `instantiateModel` (the `instantiate-model` command)
reconstructs the spawn input from the container's metadata and expands the stored hierarchy into
entities: `spawnModel` builds the mesh entity (`spawnSkinnedModel` for a rig, with its bone entities,
joints, and a stopped `AnimationPlayer`), and `applyImportedMaterials` attaches a `MaterialComponent`
(one material) or a `MaterialSetComponent` (several). The root carries a `ModelInstanceComponent`
naming its source asset. One `.smodel` instantiates into many independent entity trees, so the
`add-entity cube` preset is just an instantiate of the built-in cube model.

## Importing a texture

A standalone texture import is its own path: `importTexture` reads a file into bytes and calls
`registerTextureBytes`, which decodes to confirm a valid image, uploads, then writes the original
encoded bytes to a loose `textures/<uuid>.<ext>`:

```cpp
auto decoded = decodeImageFromMemory(encoded);
auto texture = uploadTexture(renderer, decoded->rgba.data(), decoded->width, decoded->height, true);
const std::string relativePath = "textures/" + std::to_string(id.value) + "." + extension;
// write `encoded` (not `decoded`) to disk
putAsset(assets.catalog, AssetEntry{ id, uniqueName(...), AssetType::Texture, relativePath });
```

The disk copy is the encoded PNG/JPG, so reloading it re-runs [the decode](../image-decoding/)
rather than storing bulky raw RGBA. The `true` argument requests sRGB, since albedo is authored in
sRGB. A model's textures, by contrast, ride inside the `.smodel` as chunks rather than loose files.

## Deduplication

A fresh `importModel` mints a new model id, so importing `cube.gltf` twice writes two `.smodel`
containers and two catalog entries (`cube`, `cube (2)`). Within a container the sub-asset ids are
stable (`subIdFor`, keyed by source name), and `reimportModel` reuses the model id and skips a
byte-identical source by its content hash. Otherwise there is no cross-import content dedup;
GPU-side sharing happens at resolve time, where entities referencing the same sub-id share one
upload through the cache.

## In the code

| What | File | Symbols |
|---|---|---|
| Model import → `.smodel` | `geometry.cppm`; `assets.cppm` | `translateModel`, `bakeModel`, `importModel` |
| Catalog rows a container contributes | `assets.cppm` | `catalogRowsForModel` |
| Reimport (content-hash skip) | `assets.cppm` | `reimportModel` |
| Texture import | `assets.cppm` | `importTexture`, `registerTextureBytes` |
| Place a model in the scene | `assets.cppm` | `instantiateModel`, `spawnModel`, `spawnSkinnedModel`, `applyImportedMaterials` |

## Related

- [Model import](../gltf-and-obj-import/) — the parse step
- [The .smodel container](../smodel-container/) — the one-file asset the bake produces
- [.smesh format](../smesh-format/) — the mesh chunk format
- [Image decoding](../image-decoding/) — the texture decode
- [Asset catalog](../asset-server-and-catalog/) — the read side
- [Project files](../project-serialization/) — persisting the catalog the import filled
