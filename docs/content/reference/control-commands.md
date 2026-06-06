+++
title = 'Control commands'
weight = 7
math = false
+++

# Control commands

Every command registered in `Saffron.Control` and driven by the `se` CLI over the unix socket. Commands are grouped by registering file. Params are positional unless named, and `?` marks an optional param. Each command returns a JSON result.

Entity and asset ids are u64, carried on the wire as decimal JSON strings (see [the id-encoding contract](../../explanations/tooling-and-control/control-plane-architecture/#id-encoding-on-the-wire)). Entity selectors resolve a string id, a number, or an exact name. Every scene-mutating command bumps `sceneVersion`; selection changes bump `selectionVersion` — both are read back by `get-selection`.

## Scene commands
*(`control_commands_scene.cpp`)*

| Command | Params | Effect |
|---|---|---|
| `list-entities` | — | all entities `{id, name, parentId?, bone?}` (`parentId` absent for roots; `bone` only on skeleton joints) |
| `list-components` | — | registered component type names |
| `create-entity` | `{name=Entity}` | create an entity, return its ref |
| `destroy-entity` | `{entity}` | destroy it and its subtree (deselects if the selection was inside) |
| `set-parent` | `{entity, parent?}` | reparent, keeping the world transform (cycle-guarded); absent/`0` parent detaches to root |
| `add-component` | `{entity, component}` | add a default-constructed component |
| `remove-component` | `{entity, component}` | remove it (errors if not removable) |
| `set-component` | `{entity, component, json}` | apply a serialized component body |
| `set-transform` | `{entity, translation?, rotation?, scale?}` | merge over current; rotation is Euler radians `{x,y,z}` |
| `set-material` | `{entity, baseColor?:{x,y,z,w}, albedoTexture?:uuid, metallic?, roughness?, emissive?:{x,y,z}, emissiveStrength?, unlit?:0\|1}` | add/merge the Material |
| `set-light` | `{entity?, direction?, color?, intensity?, ambient?}` | set the given (else first) directional light |
| `select` | `{entity}` | set the editor selection |
| `pick` | `{u=0.5, v=0.5}` | pick at viewport UV (0,0 = top-left): tests light/camera billboards first, then mesh ray-AABB; selects the hit. Returns `{hit, kind:"billboard"\|"mesh", id?, name?}` |
| `inspect` | `{entity}` | dump all the entity's components as JSON |
| `focus` | `{entity}` | aim the editor camera at it |
| `get-selection` | — | current selection + `{selectionVersion, sceneVersion}` (entity may be null) |
| `deselect` | — | clear the editor selection |
| `add-entity` | `{preset=empty\|cube\|model\|point-light\|spot-light\|directional-light\|camera}` | spawn a preset, select it |
| `copy-entity` | `{entity}` | deep-duplicate it, select the copy |
| `rename-entity` | `{entity, name}` | set its Name component, return its ref |
| `set-component-field` | `{entity, component, field, value}` | merge one field (a uuid string is coerced to u64) |
| `get-camera` | — | the editor fly-camera state |
| `set-camera` | `{position?, yaw?, pitch?, fov?, near?, far?, moveSpeed?, lookSpeed?}` | merge editor-camera fields |
| `get-gizmo` | — | the gizmo `{op, space}` |
| `set-gizmo` | `{op?:translate\|rotate\|scale, space?:world\|local}` | set the gizmo op/space |
| `gizmo-pointer` | `{phase:hover\|begin\|drag\|end, x, y}` | drive the native overlay gizmo from NDC `x,y∈[-1,1]`; returns `{hovered, dragging}` |
| `fly-input` | `{active, lookDx, lookDy, forward, back, left, right, up, down}` | stream editor fly-cam input; look deltas (pixels) accumulate until the engine drains them each frame |
## Render commands
*(`control_commands_render.cpp`)*

| Command | Params | Effect |
|---|---|---|
| `ping` | — | liveness + engine name/version/pid |
| `help` | — | list available commands |
| `render-stats` | — | draw counters + frame timing (`frameMs`/`fps` CPU loop EMA, `gpuMs` from the timestamp ring, 0 when unsupported) + every feature flag (clustered, shadows, ibl, ssao, contactShadows, ssgi, ddgi, rtSupported, rtShadows, restir, blasCount, pipelines, hdr, exposureEv, aa) |
| `set-aa` | `{off\|fxaa\|taa\|msaa2\|msaa4\|msaa8}` | anti-aliasing mode |
| `set-clustered` | `{0\|1}` | toggle clustered light culling |
| `set-ibl` | `{0\|1}` | image-based ambient vs flat |
| `set-ssao` | `{0\|1}` | screen-space AO (GTAO) |
| `set-contact-shadows` | `{0\|1}` | screen-space contact shadows |
| `set-ssgi` | `{0\|1}` | screen-space one-bounce GI |
| `set-rt-shadows` | `{0\|1}` | hardware ray-query shadows (errors if RT unsupported) |
| `set-restir` | `{0\|1}` | ReSTIR many-light direct (errors if RT unsupported) |
| `set-gi` | `{off\|ddgi}` | DDGI probe GI (multi-bounce) |
| `set-shadows` | `{0\|1}` | directional shadow map |
| `set-skinning` | `{0\|1}` | the GPU skinning path (off = skinned meshes do not gather) |
| `set-exposure` | `{ev}` | tonemap exposure in stops (`exp2(ev)`) |
| `set-depth-prepass` | `{0\|1}` | depth pre-pass |
| `viewport-native-info` | — | viewport bridge status `{platform, transport, status, controlSocket, width, height, message}`; the editor polls it as its readiness probe (`transport` is `wayland-subsurface`) |
| `set-viewport-size` | `{width, height}` | desired offscreen render size in device pixels (clamped ≥ 1); the editor sends it from the viewport panel's rect |

> Under present-only mode `screenshot target=window` is disabled (the swapchain is never in a
> capturable layout) — use `screenshot target=viewport` instead.

## Asset commands
*(`control_commands_asset.cpp`)*

| Command | Params | Effect |
|---|---|---|
| `get-project` | — | active project metadata `{loaded, root, path, name, displayName}` |
| `new-project` | `{name, displayName?, root?}` | create and open a project |
| `open-project` | `{path}` | open a project name, directory, or `project.json` |
| `import-model` | `{path}` | import + bake a model, spawn an entity carrying it (selected) |
| `import-texture` | `{path}` | import an image into the asset dir; returns its texture id |
| `list-assets` | — | the project catalog `{assets:[{id, name, type, path, folder?}], folders}` |
| `rename-asset` | `{id\|name, newName}` | rename a catalog entry |
| `create-asset-folder` | `{folder}` | create a project-saved virtual asset folder |
| `rename-asset-folder` | `{folder, name}` | rename a virtual folder and update assigned assets |
| `delete-asset-folder` | `{folder}` | delete a virtual folder and move assigned assets to root |
| `move-asset` | `{asset:id\|name, folder?}` | move an asset into a virtual folder, or root when omitted |
| `asset-usages` | `{asset:id\|name}` | list scene/environment slots that reference an asset |
| `delete-asset` | `{asset:id\|name}` | delete the catalog entry and imported file, clearing references |
| `assign-asset` | `{entity, slot:mesh\|albedo, id\|name}` | assign a catalog asset to the entity's Mesh/Material |
| `save-scene` | `{path}` | write the scene JSON |
| `load-scene` | `{path}` | read a scene JSON (deselects) |
| `save-project` | `{path?}` | save the active project, or save to `path` |
| `load-project` | `{path=project.json}` | compatibility alias for opening a project (deselects) |
| `get-thumbnail` | `{asset:id\|name, size=128}` | base64 PNG preview (mesh = 3D render, texture = the image) |
| `view-asset` | `{asset:id\|name, size=512}` | larger base64 PNG preview (same body as `get-thumbnail`) |
| `screenshot` | `{target:viewport\|window, path}` | PNG; `viewport` is synchronous, `window` is written at end of frame |
| `quit` | — | close the running app |

## Related
- [Control plane](../../explanations/tooling-and-control/) — the socket and how commands dispatch
