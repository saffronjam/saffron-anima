+++
title = 'Control commands'
weight = 7
math = false
+++

# Control commands

Every command registered in `Saffron.Control` and driven by the `se` CLI over the unix socket. Commands are grouped by registering file. Params are positional unless named, and `?` marks an optional param. Each command returns a JSON result.

Entity and asset ids are u64, carried on the wire as decimal JSON strings (see [the id-encoding contract](../../explanations/tooling-and-control/control-plane-architecture/#id-encoding-on-the-wire)). Entity selectors resolve a string id, a number, or an exact name. Every scene-mutating command bumps `sceneVersion`; selection changes bump `selectionVersion` — both are read back by `get-selection`, which also carries `playState`/`playVersion`.

During play (see `play`/`pause`/`stop`/`step`), scene commands address the *running* scene — a throwaway duplicate of the authored one — so every read and write is discarded on `stop`. Project and scene swaps (`load-scene`, `load-project`, `reload-project`, `new-project`, `open-project`, `delete-asset`) error with "stop play first"; `save-scene`/`save-project` always serialize the authored scene; the gizmo (`set-gizmo`, `gizmo-pointer`) is hidden during play.

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
| `set-transform` | `{entity, translation?, rotation?, scale?, smooth?:0\|1}` | merge over current; rotation is Euler radians `{x,y,z}`; `smooth` animates toward the values (~25ms, exact under preserve-children) |
| `set-material` | `{entity, baseColor?:{x,y,z,w}, albedoTexture?:uuid, metallicRoughnessTexture?:uuid, metallic?, roughness?, emissive?:{x,y,z}, emissiveStrength?, unlit?:0\|1, slot?, smooth?:0\|1}` | add/merge the Material; with `slot`, edit that slot of the entity's MaterialSet instead (direct write); with `smooth`, numeric fields animate toward the values (~25ms) instead of snapping, texture/unlit apply immediately |
| `set-light` | `{entity?, direction?, color?, intensity?, ambient?}` | set the given (else first) directional light |
| `select` | `{entity}` | set the editor selection |
| `pick` | `{u=0.5, v=0.5}` | pick at viewport UV (0,0 = top-left): tests light/camera billboards first, then mesh ray-AABB; selects the hit. Returns `{hit, kind:"billboard"\|"mesh", id?, name?}` |
| `inspect` | `{entity}` | dump all the entity's components as JSON |
| `focus` | `{entity}` | aim the editor camera at it |
| `get-selection` | — | current selection + `{selectionVersion, sceneVersion, playState, playVersion}` (entity may be null) |
| `deselect` | — | clear the editor selection |
| `play` | — | enter play mode (from edit) or resume (from paused): duplicate the scene, cut to its primary camera. Returns `{state, playVersion, sceneVersion, hasPrimaryCamera}` |
| `pause` | — | freeze the runtime tick (rendering + control keep running); `playing` only |
| `step` | `{frames=1}` | advance exactly `frames` fixed ticks; `paused` only |
| `stop` | — | discard the play duplicate and restore the authored scene; idempotent in edit |
| `get-play-state` | — | the current `{state, playVersion, sceneVersion, hasPrimaryCamera}` |
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
| `script-input` | `{keys:[...]}` | set the normalized key names visible to Lua through `se.is_key_down(key)` (and the derived `se.is_key_pressed`/`se.is_key_up` edges) |

## Animation commands

Drive a rig's `AnimationPlayerComponent`. `play`/`seek` set `previewInEdit`, so they animate in Edit
without entering Play; every mutation bumps `animationVersion` (carried on `get-animation-state`,
`get-play-state`, and `get-selection`). The state result is `{clip, clipName, duration, time, playing,
wrap, speed, animationVersion}`.

| Command | Params | Effect |
|---|---|---|
| `list-clips` | `{entity}` | the animation clips in the project catalog `{id, name, duration}` |
| `get-animation-state` | `{entity}` | the rig's playhead, clip, wrap, and speed (errors if no player) |
| `play-animation` | `{entity, clip, speed=1, loop=true, blend=0}` | play a clip (previews in Edit); `blend>0` cross-fades/inertializes from the current clip |
| `set-animation-playing` | `{entity, playing}` | resume (`true`) or pause (`false`) without moving the playhead |
| `seek-animation` | `{entity, time}` | set the playhead (previews in Edit; works in Play, Paused, and Edit) |
| `set-animation-loop` | `{entity, wrap}` | set the wrap mode (`once` \| `loop` \| `pingpong`) |
| `stop-preview` | `{entity}` | clear the Edit preview and stop, reverting the rig to its rest pose |

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
| `reload-project` | — | close and re-open the active project from its own path (deselects) |
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
| `assign-asset` | `{entity, slot:mesh\|albedo\|metallic-roughness, id\|name}` | assign a catalog asset to the entity's Mesh/Material slot |
| `save-scene` | `{path}` | write the scene JSON |
| `load-scene` | `{path}` | read a scene JSON (deselects) |
| `save-project` | `{path?}` | save the active project, or save to `path` |
| `load-project` | `{path=project.json}` | compatibility alias for opening a project (deselects) |
| `get-thumbnail` | `{asset:id\|name, size=128}` | base64 PNG preview (mesh = 3D render, texture = the image); a disk-cache hit returns it, a cold miss replies `pending` while a worker generates it (retry) |
| `view-asset` | `{asset:id\|name, size=512}` | larger base64 PNG preview (same body as `get-thumbnail`) |
| `thumbnail-cache` | `{action:stats\|clear}` | inspect (`{entries, bytes}`) or empty the project's thumbnail disk cache |
| `screenshot` | `{target:viewport\|window, path}` | PNG; `viewport` is synchronous, `window` is written at end of frame |
| `quit` | — | close the running app |

## Related
- [Control plane](../../explanations/tooling-and-control/) — the socket and how commands dispatch
