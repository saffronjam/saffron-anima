+++
title = 'Asset catalog'
weight = 6
+++

# Asset catalog

An asset catalog maps each asset's stable id to a human name and a file path. Components reference
assets by [Uuid](../scene-serialization/) rather than by name, so the catalog is what turns a
stored id into something a person can read and a loader can open.

The mapping lets the registry-driven inspector show asset names instead of raw ids. The catalog is
defined in `Saffron.Scene` and owned by the
[AssetServer](../../geometry-and-assets/asset-server-and-catalog/); the `Scene` borrows a read-only
pointer to it.

## What the catalog holds

Each entry maps an asset's stable id to a display name and the relative path of its baked
`.smesh` or copied texture under the project's asset root. The catalog is a list of these entries
plus an id index.

```cpp
enum class AssetType { Mesh, Texture, Other };

struct AssetEntry
{
    Uuid id;
    std::string name;            // UTF-8, renameable
    AssetType type = AssetType::Mesh;
    std::string path;            // relative to the asset root
};

struct AssetCatalog
{
    std::vector<AssetEntry> entries;
    std::unordered_map<u64, std::size_t> byId;  // id -> index into entries
};
```

This is a catalog, not a filesystem view. Names are arbitrary, renameable UTF-8 labels, and two
assets can share a base name; `uniqueName` disambiguates with " (2)", " (3)", and so on.

## The helpers

The catalog has a small free-function API, the same Go-style shape as the rest of the scene:

```cpp
auto findAsset(const AssetCatalog&, Uuid id) -> const AssetEntry*;   // nullptr on miss
void putAsset(AssetCatalog&, AssetEntry entry);                      // insert or replace by id
auto renameAsset(AssetCatalog&, Uuid id, std::string name) -> bool;
auto uniqueName(const AssetCatalog&, const std::string& base) -> std::string;
```

`putAsset` is an upsert: an existing id overwrites that entry, otherwise it appends and records the
index. `findAsset` returns null on a miss, the usual optional-by-pointer convention.

## Why the scene only borrows it

The pointer is `const` and not owned. The `AssetServer` owns the real catalog and the GPU caches
keyed by the same ids; the editor sets `scene.catalog` to point at it each frame before drawing the
inspector. The inspector's mesh and material pickers read the catalog through this borrow to turn a
stored Uuid into a name in a combo box. Because the scene only borrows it, the catalog is not part
of scene serialization. It is serialized separately and travels with the scene inside the one
[`project.json`](../../geometry-and-assets/project-serialization/).

> [!NOTE]
> The borrow is set per-frame and can be null. Code that reads `scene.catalog` checks it first; a
> scene loaded headlessly (the serialization self-test) leaves it null and never touches it.
> Keeping it out of the registry is deliberate: the world is entity data, the catalog is project
> data, and conflating them would drag asset bookkeeping into every `forEach`.

## In the code

| What | File | Symbols |
|---|---|---|
| Catalog types | `scene.cppm` | `AssetEntry`, `AssetType`, `AssetCatalog` |
| Catalog helpers | `scene.cppm` | `findAsset`, `putAsset`, `renameAsset`, `uniqueName` |
| The borrow | `scene.cppm` | `Scene::catalog` |
| Who owns it | `assets.cppm` | `AssetServer::catalog` |
| To/from JSON | `assets.cppm` | `catalogToJson`, `catalogFromJson` |

## Related
- [Asset server and catalog](../../geometry-and-assets/asset-server-and-catalog/) — the owner and the GPU caches
- [Project serialization](../../geometry-and-assets/project-serialization/) — catalog + scene in one file
- [Asset pickers and drag-drop](../../ui-and-editor/asset-pickers-and-drag-drop/) — what reads the borrow
- [Components](../built-in-components/) — the components that reference catalog ids
