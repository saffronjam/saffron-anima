+++
title = 'Asset commands'
weight = 5
+++

# Asset commands

The asset commands import models and textures, browse and rename the project asset catalog, wire assets onto entities, and save or load the project. They touch both the `AssetServer` (catalog and GPU caches) and the scene. `screenshot` and `quit` live here too, rounding out a scriptable session.

## Import and catalog

| Command | Params | Effect |
|---|---|---|
| `import-model` | `{path}` | Imports + bakes a model, spawns an entity carrying it (selected). Returns `{id, name, mesh, albedoTexture}`. |
| `import-texture` | `{path}` | Imports an image into the asset dir; returns `{texture: id}` to assign later. |
| `list-assets` | — | Returns every catalog entry as `{id, name, type, path}`. |
| `rename-asset` | `{asset, name}` | Renames a catalog entry (selected by id or current name). |
| `assign-asset` | `{entity, slot, asset}` | Sets the entity's mesh or albedo slot to a catalog asset. |

`import-model` is the one command that also spawns: it imports, bakes the `.smesh`, then `spawnModel`s an entity and selects it. `import-texture` only adds to the catalog; you attach the result with `assign-asset` or `set-material --albedoTexture`. `assign-asset` takes `slot: mesh|albedo`, resolves the asset by id or name, adds the target component if the entity lacks it, and writes the asset id into the slot.

## Save and load

| Command | Params | Effect |
|---|---|---|
| `save-scene` | `{path}` | Writes the scene (entities + components) to `path`. |
| `load-scene` | `{path}` | Reads a scene file; clears selection. |
| `save-project` | `{path=project.json}` | Writes the asset catalog + scene as one file. |
| `load-project` | `{path=project.json}` | Reads catalog + scene; clears selection. |

The project commands are the whole-project pair: one `project.json` holds the catalog and the scene together, which is what `load-project` needs so mesh and texture UUIDs in the scene resolve against the catalog it just loaded. All four set `ctx.editor.scenePath` so the editor knows the active file.

## Session control

| Command | Params | Effect |
|---|---|---|
| `screenshot` | `{target: viewport\|window, path}` | Writes a PNG. `viewport` is captured immediately; `window` is deferred to end-of-frame. |
| `quit` | — | Sets `window.shouldClose`, ending the run loop. |

`screenshot` reports `pending`: `false` for a viewport grab (done synchronously), `true` for a window grab (written when the current frame presents). The [capture](../screenshots-and-capture/) path is its own page.

## In the code

| What | File | Symbols |
|---|---|---|
| Registration | `control_commands_asset.cpp` | `registerAssetCommands` |
| Import | `control_commands_asset.cpp` | `import-model` (`importModel`, `spawnModel`), `import-texture` (`importTexture`) |
| Catalog | `control_commands_asset.cpp` | `list-assets`, `rename-asset`, `assign-asset`; `ctx.assets.catalog.entries` |
| Project IO | `control_commands_asset.cpp` | `save-project`/`load-project`, `save-scene`/`load-scene` |
| Capture + quit | `control_commands_asset.cpp` | `screenshot` (`captureViewport`, `requestWindowCapture`), `quit` |

## Related
- [Capture](../screenshots-and-capture/) — the PNG capture path behind `screenshot`
- [Scene commands](../scene-commands/) — `set-material` is the other way to set albedo
- [Geometry & assets](../../geometry-and-assets/) — import and the asset catalog
