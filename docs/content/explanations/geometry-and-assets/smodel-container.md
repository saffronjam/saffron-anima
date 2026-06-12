+++
title = 'The .smodel container'
weight = 11
+++

# The .smodel container

A `.smodel` is one self-contained file per imported model. It bakes the meshes, materials,
textures, animations, and the node hierarchy of a glTF or OBJ import into a single binary
container, so an import that used to scatter dozens of loose files writes exactly one. The
file carries its own identity and a recipe for rebuilding itself, which lets the filesystem
be the source of truth for the asset catalog.

Importing Sponza touches around fifty textures. The old path wrote a `.smesh`, fifty
`textures/<uuid>.<ext>` files, and inline material components on a spawned entity, then
relied on `project.json` recording every uuid. Forget to save and those files became dead
orphans the catalog never knew about. One container closes that gap: it is the record, so a
scan rediscovers it whether or not the project was saved.

## Layout

A fixed 64-byte `SMDL` header, a chunk table, the front-loaded metadata chunk, then the
payload chunks (16-byte aligned). It mirrors the `.smesh` discipline — fixed magic, a
version gate, 64-bit offsets, a total-length check on read.

```cpp
struct SModelHeader            // 64 bytes, little-endian
{
    char magic[4];             // 'S','M','D','L'
    u32 containerVersion;      // ContainerFormatVersion
    u32 schemaVersion;         // metadata-chunk schema version
    u32 flags;
    u32 tocCount;
    u32 reserved0;
    u64 tocOffset;
    u64 metaOffset;            // the META chunk, placed first so a prefix read reaches it
    u64 metaLength;
    u64 totalLength;           // validated against the file size
    u32 reserved[2];
};

enum class ChunkKind : u32 { Meta, Mesh, Texture, Material, Animation, Thumbnail };  // fourcc tags
```

Each payload chunk is wrapped, never reinvented: a `MESH` chunk is a `.smesh` image, a
`SANM` chunk is a `.sanim` image, an `SMAT` chunk is the material's `.smat` JSON, and an
`STEX` chunk is the texture's encoded bytes with its colorspace in the chunk flags. Because
those images are self-relative, the same `loadMeshFromBytes` reads a chunk slice exactly as
it reads a whole file. Imported materials become first-class `.smat` chunks here — they used
to live only as inline components on the spawned entity and were never written to disk.

## The metadata chunk

The `META` chunk is JSON, placed first after the table. A *prefix read* — the 64-byte header
plus that one chunk — is enough to build a catalog entry without touching a single payload
byte. It carries the model id and name, the import recipe, the flat list of sub-assets, a
material factor summary, the node hierarchy, an optional skin, and the extraction remap
table.

```jsonc
{
  "model":  { "id": "<decimal-uuid>", "name": "town", "sourceFormat": "gltf" },
  "import": { "sourcePath": "raw/town.glb", "sourceHash": "<content hash>", "options": { … } },
  "subAssets": [ { "subId": "<uuid>", "type": "mesh", "name": "town_mesh", "chunk": 1 }, … ],
  "nodes": [ … ], "skin": { … }, "remap": { }
}
```

Every reference is by a stable 64-bit `Uuid`, never a path or an array index. A sub-asset's
id is derived from a source key (`subIdFor(modelKey, kind, sourceName, dupIndex)`), so a
reimport that reorders meshes still resolves the same sub-id. The `nodes`/`skin` block is
exactly the rig payload, which is why `.smodel` supersedes the editor-view `.srig` sidecar.

## Identity, scanning, and the cache

The filesystem is the source of truth. `scanAssets` walks `assets/`, prefix-reads every
`.smodel` into one `Model` row plus a row per sub-asset, and identifies engine-written
standalone files by their uuid filename. A foreign file dropped in (a raw `.png`) gets a
`.smeta` sidecar holding its id and colorspace. `loadProject` reconciles the loaded catalog
against the scan, so a never-saved import can never become an orphan. A regenerable
`assets/.cache/catalog.json` is a latency shortcut keyed by a signature of the tree — delete
it and a cold scan yields the identical catalog.

## Instantiate, extract, reimport

Import produces an asset and does **not** spawn. `instantiateModel` expands the stored node
hierarchy into entt entities on demand, so one asset becomes many instances (or none). The
entities hold soft `(modelId, subId)` references resolved at draw time, so changes to the
container flow through without re-instantiation.

`extractSubAsset` slices a chunk to a standalone file keeping its sub-id and writes a
`remap` entry, so resolution prefers the external file — the workflow for editing or sharing
one embedded material. `reimportModel` skips when the source bytes are unchanged
(content-addressed), otherwise re-bakes with the *stored* options, diffs by sub-id, and never
clobbers a remapped (extracted) sub-asset.

## In the code

| What | File | Symbols |
|---|---|---|
| Header + table + framing | `geometry.cppm` | `SModelHeader`, `TocEntry`, `writeContainer`, `readContainer` |
| Metadata + prefix read | `assets.cppm` | `ContainerMetadata`, `readContainerMetadata` |
| Bake | `assets.cppm` | `bakeModel`, `importModel` |
| Chunk-slice load | `assets.cppm`; `geometry.cppm` | `loadModelAsset`, `resolveMesh`, `loadMeshFromBytes` |
| Instantiate | `assets.cppm` | `instantiateModel`, `ModelInstanceComponent` |
| Scan + cache | `assets.cppm` | `scanAssets`, `loadCatalog`, `readSmeta` |
| Extract + reimport | `assets.cppm` | `extractSubAsset`, `reimportModel` |

> [!NOTE]
> `.smodel` is a new magic and version; `.smesh` and `.sanim` are unchanged and are reused
> verbatim as chunk payloads. Re-importing the *same* source file as a second model collides
> on the source-derived sub-ids — the path for updating a model from its source is
> `reimport-model`, not a second import.

## Related

- [.smesh format](../smesh-format/) — the mesh image embedded as a `MESH` chunk
- [Asset server & catalog](../asset-server-and-catalog/) — the scan-derived, UUID-keyed catalog
- [Import pipeline](../import-pipeline/) — translate → bake, and why import no longer spawns
- [Clean unused assets](../../../how-to/clean-unused-assets/) — the deliberate cleanup workflow
- [Asset editor](../../ui-and-editor/asset-editor/) — reads the node hierarchy + skin + clips back out of this container via `get-asset-model`
