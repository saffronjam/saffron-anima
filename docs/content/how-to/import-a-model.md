+++
title = 'Import a model'
weight = 2
math = false
+++

# Import a model

Bring a glTF or OBJ model into the project. Three methods are available.

You need an active project first. The editor startup modal creates or opens one, and tests can
select one with `SAFFRON_PROJECT=<project-name>`.

## Steps

Choose the one that fits:

1. **Drag-and-drop** — drop a `.gltf` / `.glb` / `.obj` onto the editor window. Imported into the catalog, no entity spawned.
2. **File ▸ Import** — the editor menu, same catalog-only import.
3. **From the CLI** — import and spawn an entity in one step:
   ```sh
   se import-model /path/to/model.gltf
   ```
   `import-model` bakes the mesh to a project-local `.smesh`, uploads it, imports each material's albedo, adds catalog entries, then spawns an entity with the mesh + its material(s) — a `MaterialComponent` for one material, a `MaterialSet` for several.

To import just a texture (assign it to a material later):
```sh
se import-texture /path/to/albedo.png
```

## Verify

- List the catalog: `se list-assets` — the mesh (and any albedo texture) appears with name and id.
- Check the project folder: meshes are under `assets/models`, textures under `assets/textures`.
- The **Assets** panel shows a tile (3D thumbnail for meshes, image for textures).
- After `import-model` the new entity is selected. Screenshot it:
  ```sh
  se screenshot viewport /tmp/import.png
  ```

## In the code

| What | File | Symbols |
|---|---|---|
| `se import-model` / `import-texture` | `control_commands_asset.cpp` | `import-model`, `import-texture` |
| Import + bake + spawn | `assets.cppm` | `importModel`, `importTexture`, `spawnModel` |
| Catalog listing | `control_commands_asset.cpp` | `list-assets` |

## Related

- [Import pipeline](../../explanations/geometry-and-assets/import-pipeline/)
- [glTF and OBJ import](../../explanations/geometry-and-assets/gltf-and-obj-import/)
- [Asset catalog](../../explanations/geometry-and-assets/asset-server-and-catalog/)
- [Project files](../../explanations/geometry-and-assets/project-serialization/)
