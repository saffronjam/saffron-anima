+++
title = 'Asset catalog'
weight = 6
+++

# Asset catalog

A `MeshComponent` or `MaterialComponent` references an asset by [Uuid](../scene-serialization/),
not by name. The mapping from id to a human name and a file path is the `AssetCatalog`. It is
defined in `Saffron.Scene` and owned by the
[AssetServer](../../geometry-and-assets/asset-server-and-catalog/), but the `Scene` borrows a
read-only pointer so the registry-driven inspector can show names instead of raw ids.

## What the catalog holds

Each entry maps an asset's stable id to a display name and the relative path of its baked
`.smesh` or copied texture under the project's asset root. The catalog is a list of these plus an
id index.

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

This is a catalog, not a filesystem view — names are arbitrary, renameable UTF-8 labels, and two
assets can share a base name (`uniqueName` disambiguates with " (2)", " (3)", …).

## The helpers

The catalog has a small free-function API, the same Go-style shape as the rest of the scene:

```cpp
auto findAsset(const AssetCatalog&, Uuid id) -> const AssetEntry*;   // nullptr on miss
void putAsset(AssetCatalog&, AssetEntry entry);                      // insert or replace by id
auto renameAsset(AssetCatalog&, Uuid id, std::string name) -> bool;
auto uniqueName(const AssetCatalog&, const std::string& base) -> std::string;
```

`putAsset` is upsert: an existing id overwrites that entry, otherwise it appends and records the
index. `findAsset` returns null on a miss, the usual optional-by-pointer convention.

## Why the scene only borrows it

The pointer is `const` and not owned. The `AssetServer` owns the real catalog (it also owns the
GPU caches keyed by the same ids); the editor sets `scene.catalog` to point at it each frame
before drawing the inspector. The inspector's mesh and material pickers read the catalog through
this borrow to turn a stored Uuid into a name in a combo box. Because the scene only borrows, the
catalog is not part of scene serialization — it is serialized separately and travels with the
scene inside the one [`project.json`](../../geometry-and-assets/project-serialization/).

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
