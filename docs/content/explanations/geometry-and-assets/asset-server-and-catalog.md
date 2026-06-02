+++
title = 'Asset catalog'
weight = 6
+++

# Asset catalog

An asset catalog is a named registry that maps stable UUIDs to files on disk, decoupling
the assets a project references from where those files live. Components reference an asset
by `Uuid`; the catalog resolves that id to a name, a type, and a relative path.

The catalog gives every asset an identity that survives renaming and moving its file. A
scene stores a `Uuid` rather than a path, so editing the asset's location or display name
never breaks the reference. Around the catalog sits the asset server, which turns a `Uuid`
into a live GPU resource.

## The server

The `AssetServer` is the project's single owner of assets: the catalog plus two UUID-keyed
GPU caches, so entities sharing an asset upload it once.

```cpp
struct AssetServer
{
    std::string root;
    AssetCatalog catalog;                                       // id -> {name, type, path}
    std::unordered_map<u64, Ref<GpuMesh>> meshRefByUuid;        // GPU cache
    std::unordered_map<u64, Ref<GpuTexture>> textureRefByUuid;  // GPU cache
};
```

The catalog is the source of truth; the two maps are caches over it. The catalog records
that asset 42 is a mesh at `meshes/42.smesh`; the cache records that asset 42 is already
uploaded and holds its `Ref`. `newAssetServer` creates the asset root with its `meshes/`
and `textures/` subdirectories and migrates any legacy `asset_registry.json`. The catalog
is normally populated by [loading a project](../project-serialization/).

## The catalog

The catalog is a named registry, not a filesystem view. Each entry pairs a human-facing
name the user can rename with a separate on-disk path:

```cpp
enum class AssetType { Mesh, Texture, Other };

struct AssetEntry
{
    Uuid id;
    std::string name;     // UTF-8, renameable, user-facing
    AssetType type;
    std::string path;     // relative to the asset root
};

struct AssetCatalog
{
    std::vector<AssetEntry> entries;
    std::unordered_map<u64, std::size_t> byId;  // id -> index into entries
};
```

`putAsset` inserts or replaces by id and keeps `byId` in sync; `findAsset` resolves an id
to a `const AssetEntry*`; `uniqueName` appends ` (2)`, ` (3)`, … so two imports of
`cube.gltf` get distinct names. The catalog lives in `Saffron.Scene`, not `Saffron.Assets`,
so the registry-driven inspector can read it without depending on the renderer; the `Scene`
borrows a `const AssetCatalog*` for inspector pickers.

## Resolving an id to a GPU resource

`loadMeshAsset` and `loadTextureAsset` are the resolve-on-demand front doors. Both share a
three-step shape: check the cache; on a miss, look the id up in the catalog and load and
upload the file; cache the result.

```cpp
auto cached = assets.meshRefByUuid.find(id.value);
if (cached != end) return cached->second;          // hit (may be a null Ref)
const AssetEntry* entry = findAsset(assets.catalog, id);
if (!entry || entry->type != AssetType::Mesh) return nullptr;
auto mesh = loadMesh(assets.root + "/" + entry->path);
// ... uploadMesh, cache, return ...
```

The cache means many entities referencing the same mesh trigger one `loadMesh` and one
[`uploadMesh`](../gpu-mesh-upload/); the rest are map hits. `renderScene` calls these
resolvers once per entity per frame, so the caching keeps that cheap.

## Negative caching

A failed load stores a null `Ref` in the cache rather than returning null and forgetting:

```cpp
assets.meshRefByUuid[id.value] = nullptr;  // a broken asset isn't retried + re-logged each frame
```

On the next frame the lookup hits that null `Ref` and returns it immediately, without
re-reading the broken file or re-logging. The entry's presence, even holding null, marks
the asset as tried. Because `renderScene` runs every frame, this keeps a missing or corrupt
asset from flooding the log and re-hitting the disk many times a second.

## In the code

| What | File | Symbols |
|---|---|---|
| The server | `assets.cppm` | `AssetServer`, `newAssetServer` |
| Catalog type | `scene.cppm` | `AssetCatalog`, `AssetEntry`, `AssetType` |
| Catalog ops | `scene.cppm` | `putAsset`, `findAsset`, `renameAsset`, `uniqueName` |
| Resolve + cache | `assets.cppm` | `loadMeshAsset`, `loadTextureAsset` |

## Related

- [Import pipeline](../import-pipeline/) — how entries get into the catalog
- [Project files](../project-serialization/) — how the catalog persists
- [Draw list](../draw-list/) — calls the resolvers per entity per frame
- [Asset catalog in the scene](../../scene-and-ecs/asset-catalog-in-scene/) — why it lives in `Saffron.Scene`
- [Asset commands](../../tooling-and-control/asset-commands/) — driving the catalog from the CLI
