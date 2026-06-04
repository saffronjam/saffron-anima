+++
title = 'Scene commands'
weight = 3
+++

# Scene commands

The scene commands are the control-plane verbs that list, create, and edit entities in the running editor's scene. Each edit routes through the target component's registered `serialize`/`deserialize`, so the wire shape is identical to a scene file. There is no separate code path for editing from the CLI.

Most commands take an `{entity}` argument. `resolveEntity` accepts a UUID (number or numeric string) or a name, and tries the UUID first because it is stable across reloads. A miss returns an error, not a null entity.

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
| `get-selection` | — | Returns the current selection as `{id, name}`, or `{id: null}` when nothing is selected. |
| `deselect` | — | Clears the editor selection. Returns `{id: null}`. |
| `add-entity` | `{preset?}` | Creates an entity from a preset (default `empty`); selects it; returns `{id, name}`. |
| `copy-entity` | `{entity}` | Deep-duplicates the entity (all components, new UUID); selects the copy; returns `{id, name}`. |
| `rename-entity` | `{entity, name}` | Sets the entity's Name component; returns its `{id, name}`. |
| `set-component-field` | `{entity, component, field, value}` | Merges a single field into a component (generic; adds the component if missing). |
| `pick` | `{u=0.5, v=0.5}` | Ray-picks at a viewport UV (`0,0` = top-left) and selects the hit. Returns `{hit, id?, name?}`. |
| `inspect` | `{entity}` | Dumps every present component as JSON under `components`. |
| `focus` | `{entity}` | Moves the editor camera to look at the entity's transform. |

## Presets

`add-entity` takes a `preset` naming what to spawn, matching the editor's **Create** menu:

| Preset | Spawns |
|---|---|
| `empty` | A bare entity (Transform only). |
| `cube` | The built-in cube mesh + default material. |
| `model` | An empty model entity (mesh slot left unassigned for a later `assign-asset`). |
| `point-light` | A `PointLightComponent`. |
| `spot-light` | A `SpotLightComponent`. |
| `directional-light` | A `DirectionalLightComponent`. |
| `camera` | A `CameraComponent`. |

An unknown preset is an error, not a silent fall-through to `empty`.

## The editor camera and gizmo

| Command | Params | Effect |
|---|---|---|
| `get-camera` | — | Returns the editor fly-cam as `{position, yaw, pitch, fov, near, far}`. |
| `set-camera` | `{position?, yaw?, pitch?, fov?, near?, far?}` | Merges the given fields into the editor fly-cam. |
| `get-gizmo` | — | Returns the shared gizmo state `{op, space}`. |
| `set-gizmo` | `{op?, space?}` | Sets the gizmo `op` (`translate\|rotate\|scale`) and/or `space` (`world\|local`). |
| `dump-schema` | — | Returns the live shapes of every component, the scene environment, and render-stats. |

`get-camera`/`set-camera` drive the same editor [fly-camera](../../ui-and-editor/) the viewport uses — the scene-view eye, not an ECS `CameraComponent`. `set-camera` merges fields the same way the transform commands do.

`get-gizmo`/`set-gizmo` read and write a single gizmo state. The in-viewport ImGuizmo path and the W/E/R shortcut both read it, and any future native manipulation path reads the same state, so the gizmo mode stays consistent regardless of who set it. `op` selects translate, rotate, or scale; `space` selects world or local.

`dump-schema` returns the live runtime shapes: it serializes a default of each registered component, the `SceneEnvironment`, and the render-stats block, so a tool can discover the wire shape without parsing C++. It is the **codegen seam** — see [Shared types](../shared-types/) for where the schema-first pipeline takes over.

## Polling counters

`SceneEditContext` carries two monotonically increasing counters a UI can poll to diff cheaply instead of re-listing the whole scene each frame:

| Counter | Bumped when |
|---|---|
| `sceneVersion` | every scene-mutating command: `create-entity`, `destroy-entity`, `add-component`, `remove-component`, `set-component`, `set-component-field`, `set-transform`, `set-material`, `set-light`, `set-environment`, `add-entity`, `copy-entity`, `rename-entity`, plus the [asset commands](../asset-commands/) that touch the scene (`import-model`, `assign-asset`, `load-scene`/`load-project`, `new-project`/`open-project`). |
| `selectionVersion` | every `setSelection` (including `select`, `deselect`, `pick`, the auto-select on `add-entity`/`copy-entity`/`import-model`, and an entity destroy or scene/project load that clears it). |

A client reads a counter once, then re-fetches the entity list or the selection only when the number changes. The counters live on the context, not the wire, so any command that mutates the scene bumps the right one regardless of who invoked it.

## Merge, don't reset

`set-transform`, `set-material`, and `set-light` first `serialize` the current value, copy the provided fields over it, then `deserialize` the merged body. Setting only the translation therefore leaves scale untouched. Vectors are `{x,y,z}` objects (`baseColor` is `{x,y,z,w}`), matching the scene-file encoding. `set-material --unlit` and the render toggles coerce strings like `0`/`false`/`off` to the right type so a CLI-supplied string does not abort the no-throw JSON path.

## Picking and focus

`pick` builds a ray from the editor camera through the given viewport UV (converted to NDC `u*2-1, v*2-1`) and calls `pickEntity`, which hits the nearest entity by world-space mesh AABB. Empty space returns `{hit:false}` and deselects. `focus` reads the entity's `TransformComponent.translation` and pulls the editor camera back along its forward axis so the target sits in view. Both use the same editor [fly-camera](../../ui-and-editor/) the viewport uses.

## In the code

| What | File | Symbols |
|---|---|---|
| Registration | `control_commands_scene.cpp` | `registerSceneCommands` |
| Entity resolution | `command.cppm` | `resolveEntity`, `entityRef` |
| Component edits | `control_commands_scene.cpp` | `set-component`, `set-transform`, `set-material`, `set-light`, `set-component-field` |
| Presets + duplicate + rename | `control_commands_scene.cpp` | `add-entity`, `copy-entity`, `rename-entity` |
| Selection + picking | `control_commands_scene.cpp` | `select`, `get-selection`, `deselect`, `pick`, `focus`; `pickEntity`, `editorCameraView` |
| Editor camera + gizmo | `control_commands_scene.cpp` | `get-camera`/`set-camera`, `get-gizmo`/`set-gizmo` |
| Schema dump | `control_commands_scene.cpp` | `dump-schema` |
| Poll counters | `scene_edit_context.cppm` | `SceneEditContext.sceneVersion`, `SceneEditContext.selectionVersion`, `setSelection` |
| The registry behind the edits | `editor.cppm` / `scene.cppm` | `ComponentTraits.serialize`/`deserialize`, `findByName` |

## Related
- [Asset commands](../asset-commands/) — assigning meshes and textures to entities
- [Shared types](../shared-types/) — the schema-first wire contract `dump-schema` feeds
- [Scene & ECS](../../scene-and-ecs/) — the component registry these commands drive
- [Control plane](../control-plane-architecture/) — how a command is registered and dispatched
