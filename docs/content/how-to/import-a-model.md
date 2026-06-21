+++
title = 'Import a model'
weight = 2
math = false
+++

# Import a model

Bring a glTF or OBJ model into the project. Importing **bakes** the source into one
[`.smodel` container](../../explanations/geometry-and-assets/smodel-container/) asset — the mesh,
materials, textures, and animations as chunks of a single file — and adds the catalog rows. It does
not spawn an entity: placing the model in the scene is a separate, repeatable step, so one import
becomes many instances.

You need an active project first. The editor startup modal creates or opens one, and tests can
select one with `SAFFRON_PROJECT=<project-name>`.

## Import (bake the asset)

Any of these bakes one `.smodel` tile and nothing else:

1. **Drag-and-drop** — drop a `.gltf` / `.glb` / `.obj` onto the editor window.
2. **File ▸ Import** — the editor menu.
3. **From the CLI**:
   ```sh
   sa import-model /path/to/model.gltf
   ```

To import a standalone texture (a loose texture asset, e.g. to assign to a material later):
```sh
sa import-texture /path/to/albedo.png
```

## Place it in the scene

The baked model is a catalog asset; instantiate it to add entities:

1. **Drag the model tile onto the viewport** (or onto the Hierarchy) — instantiates it into the scene.
2. **Right-click the tile ▸ Add to scene**.
3. **From the CLI**:
   ```sh
   sa instantiate-model <model-id-or-name>
   ```

Each instantiate expands the container's stored hierarchy into fresh entities (the mesh, its
materials, and — for a rig — its bones and a stopped `AnimationPlayer`), and the new root is selected.

## Verify

- List the catalog: `sa list-assets` — the model appears as one `"type": "model"` row (its embedded
  mesh/material/texture sub-assets link back to it by `container`).
- Check the project folder: one `.smodel` under `assets/models`; no loose mesh or texture files for it.
- The **Assets** panel shows one tile, its thumbnail the textured model.
- After `instantiate-model` the new entity is selected. Screenshot it:
  ```sh
  sa screenshot viewport /tmp/import.png
  ```

## In the code

| What | File | Symbols |
|---|---|---|
| `sa import-model` / `import-texture` / `instantiate-model` | `control/src/commands_asset.rs` | `import-model`, `import-texture`, `instantiate-model` |
| Bake the `.smodel` | `assets/src/import.rs` | `import_model`, `bake_model` |
| Import a loose texture | `assets/src/scan.rs` | `import_texture`, `register_texture_bytes` |
| Place it in the scene | `assets/src/spawn.rs` | `instantiate_model`, `spawn_model`, `spawn_skinned_model` |
| Catalog listing | `control/src/commands_asset.rs` | `list-assets` |

## Related

- [Import pipeline](../../explanations/geometry-and-assets/import-pipeline/)
- [glTF and OBJ import](../../explanations/geometry-and-assets/gltf-and-obj-import/)
- [Asset catalog](../../explanations/geometry-and-assets/asset-server-and-catalog/)
- [Project files](../../explanations/geometry-and-assets/project-serialization/)
