+++
title = 'Asset commands'
weight = 5
+++

# Asset commands

The asset commands are the control-plane verbs that manage a project's assets: importing models and textures, browsing and organizing the catalog, binding assets onto entities, and saving or loading the project. They act on both the `AssetServer` — the catalog and its GPU caches — and the scene. The `screenshot` and `quit` commands sit alongside them to complete a scriptable session.

## Import and catalog

| Command | Params | Effect |
|---|---|---|
| `import-model` | `{path}` | Imports + bakes a model, spawns an entity carrying it (selected). Returns `{id, name, mesh, albedoTexture}`. |
| `import-texture` | `{path}` | Imports an image into the asset dir; returns `{texture: id}` to assign later. |
| `list-assets` | — | Returns every catalog entry as `{id, name, type, path, folder?}` plus `folders`. |
| `rename-asset` | `{asset, name}` | Renames a catalog entry (selected by id or current name). |
| `create-asset-folder` | `{folder}` | Creates a project-saved virtual folder. |
| `rename-asset-folder` | `{folder, name}` | Renames a virtual folder and updates assets assigned to it. |
| `delete-asset-folder` | `{folder}` | Deletes a virtual folder and moves assigned assets back to root. |
| `move-asset` | `{asset, folder?}` | Moves an asset into a virtual folder, or back to root when `folder` is omitted. |
| `asset-usages` | `{asset}` | Lists scene/environment slots that reference an asset. |
| `delete-asset` | `{asset}` | Deletes the catalog entry and imported file, clears usages, and returns what was cleared. |
| `assign-asset` | `{entity, slot, asset}` | Sets the entity's mesh, albedo, or metallic-roughness slot to a catalog asset. |

`import-model` is the only command that also spawns. It imports, bakes the `.smesh`, then `spawnModel`s an entity and selects it. `import-texture` adds to the catalog alone; the result is attached later with `assign-asset` or `set-material --albedoTexture`. `assign-asset` takes `slot: mesh|albedo|metallic-roughness`, resolves the asset by id or name, adds the target component if the entity lacks it, and writes the asset id into the slot.

Folders are catalog metadata, not filesystem directories. They are saved in `project.json` next to the asset list so empty folders survive a reload. Renaming a folder updates the folder list and each catalog entry assigned to the old name. Deleting a folder only removes that virtual folder; assigned assets move back to root. `delete-asset` clears `Mesh.mesh`, `Material.albedoTexture`, and `SceneEnvironment.skyTexture` references before removing the entry and cache records.

## Thumbnails and previews

| Command | Params | Effect |
|---|---|---|
| `get-thumbnail` | `{asset, size=128}` | Renders a small preview of a catalog asset; returns the PNG as base64. |
| `view-asset` | `{asset, size=512}` | Same as `get-thumbnail` at a larger default size, for a full-asset look. |

Both resolve the `asset` by id or name and return `{format: "png", size, base64}`: the encoded image bytes inline in the JSON result, so a remote UI can show a preview without sharing a filesystem. The asset's type selects the path. A **mesh** is drawn as a framed 3D render through `renderMeshThumbnail`, the same preview the Assets panel tiles use. A **texture** is the image itself, read straight back from the GPU.

The work is a synchronous GPU→CPU readback. The command records its own command buffer, renders or copies into a host-visible staging buffer, then `waitIdle`s before encoding the PNG to memory. That mirrors [`captureViewport`](../screenshots-and-capture/): it runs between frames on the command-drain step, never on the present path, so it never stalls or tears a frame in flight. Because it blocks for a GPU round-trip, it is heavier than the list and rename commands. A UI should request a thumbnail once and cache the result, keying off the catalog version rather than re-fetching every frame.

## Save and load

| Command | Params | Effect |
|---|---|---|
| `save-scene` | `{path}` | Writes the scene (entities + components) to `path`. |
| `load-scene` | `{path}` | Reads a scene file; clears selection. |
| `save-project` | `{path=project.json}` | Writes the asset catalog + scene as one file. |
| `load-project` | `{path=project.json}` | Reads catalog + scene; clears selection. |

The project commands are the whole-project pair. One `project.json` holds the catalog and the scene together, which is what `load-project` needs so that mesh and texture UUIDs in the scene resolve against the catalog it just loaded. All four set `ctx.editor.scenePath` so the editor knows the active file.

## Session control

| Command | Params | Effect |
|---|---|---|
| `screenshot` | `{target: viewport\|window, path}` | Writes a PNG. `viewport` is captured immediately; `window` is deferred to end-of-frame. |
| `quit` | — | Sets `window.shouldClose`, ending the run loop. |

`screenshot` reports `pending`: `false` for a viewport grab, done synchronously, and `true` for a window grab, written when the current frame presents. The [capture](../screenshots-and-capture/) path has its own page.

## In the code

| What | File | Symbols |
|---|---|---|
| Registration | `control_commands_asset.cpp` | `registerAssetCommands` |
| Import | `control_commands_asset.cpp` | `import-model` (`importModel`, `spawnModel`), `import-texture` (`importTexture`) |
| Catalog | `control_commands_asset.cpp` | `list-assets`, `rename-asset`, `create-asset-folder`, `rename-asset-folder`, `delete-asset-folder`, `move-asset`, `asset-usages`, `delete-asset`, `assign-asset`; `ctx.assets.catalog.entries` |
| Thumbnails | `control_commands_asset.cpp` | `get-thumbnail`, `view-asset` (`renderMeshThumbnail`, base64 PNG-to-memory) |
| Project IO | `control_commands_asset.cpp` | `save-project`/`load-project`, `save-scene`/`load-scene` |
| Capture + quit | `control_commands_asset.cpp` | `screenshot` (`captureViewport`, `requestWindowCapture`), `quit` |

## Related
- [Capture](../screenshots-and-capture/) — the PNG capture path behind `screenshot` and the thumbnail readback
- [Shared types](../shared-types/) — the base64-PNG result shape and the wire contract
- [Scene commands](../scene-commands/) — `set-material` is the other way to set albedo
- [Geometry & assets](../../geometry-and-assets/) — import and the asset catalog
