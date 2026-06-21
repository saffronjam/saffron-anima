+++
title = 'Asset catalog'
weight = 6
+++

# Asset catalog

An asset catalog maps each asset's stable id to a human name and a file path. Components reference
assets by [`Uuid`](../scene-serialization/) rather than by name, so the catalog is what turns a
stored id into something a person can read and a loader can open.

The mapping lets the registry-driven inspector show asset names instead of raw ids. The catalog type
is defined in the scene crate and owned by the
[asset server](../../geometry-and-assets/asset-server-and-catalog/); the `Scene` holds a shared,
read-only handle to it.

## What the catalog holds

Each entry maps an asset's stable id to a display name, a kind, and the relative path of its baked
`.smesh` or copied texture under the project's asset root. The catalog is a list of these entries
plus an id index.

```rust
pub enum AssetType { Mesh, Texture, Other, Animation, Material, Model }

pub struct AssetEntry {
    pub id: Uuid,
    pub name: String,            // UTF-8, renameable
    pub asset_type: AssetType,
    pub path: String,            // relative to the asset root
    pub folder: String,
    // ... texture colorspace, animation duration/tracks, container/chunk bookkeeping
}

pub struct AssetCatalog {
    pub entries: Vec<AssetEntry>,
    pub folders: Vec<String>,
    pub by_id: HashMap<u64, usize>,  // id -> index into entries
}
```

This is a catalog, not a filesystem view. Names are arbitrary, renameable UTF-8 labels, and two
assets can share a base name; `unique_name` disambiguates with " (2)", " (3)", and so on.

## The helpers

The catalog has a small method surface, the same shape as the rest of the scene:

```rust
impl AssetCatalog {
    pub fn find(&self, id: Uuid) -> Option<&AssetEntry>;     // None on miss
    pub fn put(&mut self, entry: AssetEntry);                // insert or replace by id
    pub fn rename(&mut self, id: Uuid, name: impl Into<String>) -> bool;
    pub fn unique_name(&self, base: &str) -> String;
}
```

`put` is an upsert: an existing id overwrites that entry in place, otherwise it appends and records
the index in `by_id`. `find` returns `None` on a miss, the usual optional-lookup convention.

## Why the scene only borrows it

The scene holds an `Option<Arc<AssetCatalog>>` — a shared, read-only handle, not the owned catalog.
The `AssetServer` owns the real catalog and the GPU caches keyed by the same ids; the editor sets
`scene.catalog` to point at it before drawing the inspector. The inspector's mesh and material
pickers read the catalog through this handle to turn a stored `Uuid` into a name in a combo box.
Because the scene only borrows it, the catalog is not part of scene serialization. It is serialized
separately and travels with the scene inside the one
[`project.json`](../../geometry-and-assets/project-serialization/).

> [!NOTE]
> The handle is set per-frame and can be `None`. A scene loaded headlessly (the serialization tests)
> leaves it unset and never touches it. Keeping it out of the ECS is deliberate: the world is entity
> data, the catalog is project data, and conflating them would drag asset bookkeeping into every
> `for_each`.

## In the code

| What | File | Symbols |
|---|---|---|
| Catalog types | `scene/src/environment.rs` | `AssetEntry`, `AssetType`, `AssetCatalog` |
| Catalog helpers | `scene/src/environment.rs` | `AssetCatalog::find`, `put`, `rename`, `unique_name` |
| The handle | `scene/src/scene.rs` | `Scene::catalog` |
| Who owns it | `assets/src/lib.rs` | `AssetServer::catalog` |
| To/from JSON | `assets/src/catalog.rs` | `catalog_to_json`, `catalog_from_json` |

## Related
- [Asset server and catalog](../../geometry-and-assets/asset-server-and-catalog/) — the owner and the GPU caches
- [Project serialization](../../geometry-and-assets/project-serialization/) — catalog + scene in one file
- [Asset pickers and drag-drop](../../ui-and-editor/asset-pickers-and-drag-drop/) — what reads the handle
- [Components](../built-in-components/) — the components that reference catalog ids
