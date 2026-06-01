+++
title = 'Scene commands'
weight = 3
+++

# Scene commands

The scene commands list, create, and edit entities in the running editor's scene. Anything that edits a component routes through that component's registered `serialize`/`deserialize`, so the wire shape is identical to a scene file — there is no second code path for "set this from the CLI".

Most commands take an `{entity}` argument. `resolveEntity` accepts a UUID (number or numeric string) or a name; the UUID is tried first because it is stable across reloads. A miss returns an error, not a null entity.

## The commands

| Command | Params | Effect |
|---|---|---|
| `list-entities` | — | Returns every entity as `{id, name}`. |
| `list-components` | — | Returns the names of all registered component types. |
| `create-entity` | `{name?}` | Creates an entity (default name `Entity`); returns its `{id, name}`. |
| `destroy-entity` | `{entity}` | Destroys the entity; clears selection if it was selected. Returns `{destroyed: id}`. |
| `add-component` | `{entity, component}` | Adds the named component with its default value. Errors if unknown or already present. |
| `remove-component` | `{entity, component}` | Removes the named component. Errors if unknown or marked non-removable. |
| `set-component` | `{entity, component, json}` | Applies a serialized component body via the registry's `deserialize`. |
| `set-transform` | `{entity, translation?, rotation?, scale?}` | Merges the given fields over the current transform. Rotation is Euler XYZ radians. |
| `set-material` | `{entity, baseColor?, albedoTexture?, metallic?, roughness?, emissive?, emissiveStrength?, unlit?}` | Adds Material if missing, then merges the given fields. |
| `set-light` | `{entity?, direction?, color?, intensity?, ambient?}` | Edits the directional light (the given entity, else the first one found). |
| `select` | `{entity}` | Sets editor selection; returns `{id, name}`. |
| `pick` | `{u=0.5, v=0.5}` | Ray-picks at a viewport UV (`0,0` = top-left) and selects the hit. Returns `{hit, id?, name?}`. |
| `inspect` | `{entity}` | Dumps every present component as JSON under `components`. |
| `focus` | `{entity}` | Moves the editor camera to look at the entity's transform. |

## Merge, don't reset

`set-transform`, `set-material`, and `set-light` first `serialize` the current value, copy the provided fields over it, then `deserialize` the merged body. That is why setting only the translation leaves scale untouched. Vectors are `{x,y,z}` objects (`baseColor` is `{x,y,z,w}`), matching the scene-file encoding. `set-material --unlit` and the render toggles coerce strings like `0`/`false`/`off` to the right type so a CLI-supplied string does not abort the no-throw JSON path.

## Picking and focus

`pick` builds a ray from the editor camera through the given viewport UV (converted to NDC `u*2-1, v*2-1`) and calls `pickEntity`, which hits the nearest entity by world-space mesh AABB; empty space returns `{hit:false}` and deselects. `focus` reads the entity's `TransformComponent.translation` and pulls the editor camera back along its forward axis so the target sits in view. Both use the same editor [fly-camera](../../ui-and-editor/) the viewport uses.

## In the code

| What | File | Symbols |
|---|---|---|
| Registration | `control_commands_scene.cpp` | `registerSceneCommands` |
| Entity resolution | `command.cppm` | `resolveEntity`, `entityRef` |
| Component edits | `control_commands_scene.cpp` | `set-component`, `set-transform`, `set-material`, `set-light` |
| Selection + picking | `control_commands_scene.cpp` | `select`, `pick`, `focus`; `pickEntity`, `editorCameraView` |
| The registry behind the edits | `editor.cppm` / `scene.cppm` | `ComponentTraits.serialize`/`deserialize`, `findByName` |

## Related
- [Asset commands](../asset-commands/) — assigning meshes and textures to entities
- [Scene & ECS](../../scene-and-ecs/) — the component registry these commands drive
- [Control plane](../control-plane-architecture/) — how a command is registered and dispatched
