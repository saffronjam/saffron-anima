+++
title = 'Control commands'
weight = 7
math = false
+++

# Control commands

Every command registered in `Saffron.Control`, driven by the `se` CLI over the unix socket. Grouped by registering file. Params are positional unless named; `?` marks optional. Each returns a JSON result.

## Scene commands
*(`control_commands_scene.cpp`)*

| Command | Params | Effect |
|---|---|---|
| `list-entities` | — | all entities `{id, name}` |
| `list-components` | — | registered component type names |
| `create-entity` | `{name=Entity}` | create an entity, return its ref |
| `destroy-entity` | `{entity}` | destroy it (deselects if selected) |
| `add-component` | `{entity, component}` | add a default-constructed component |
| `remove-component` | `{entity, component}` | remove it (errors if not removable) |
| `set-component` | `{entity, component, json}` | apply a serialized component body |
| `set-transform` | `{entity, translation?, rotation?, scale?}` | merge over current; rotation is Euler radians `{x,y,z}` |
| `set-material` | `{entity, baseColor?:{x,y,z,w}, albedoTexture?:uuid, metallic?, roughness?, emissive?:{x,y,z}, emissiveStrength?, unlit?:0\|1}` | add/merge the Material |
| `set-light` | `{entity?, direction?, color?, intensity?, ambient?}` | set the given (else first) directional light |
| `select` | `{entity}` | set the editor selection |
| `pick` | `{u=0.5, v=0.5}` | ray-pick at viewport UV (0,0 = top-left); selects the hit |
| `inspect` | `{entity}` | dump all the entity's components as JSON |
| `focus` | `{entity}` | aim the editor camera at it |

## Render commands
*(`control_commands_render.cpp`)*

| Command | Params | Effect |
|---|---|---|
| `ping` | — | liveness + engine name/version/pid |
| `help` | — | list available commands |
| `render-stats` | — | draw counters + every feature flag (clustered, shadows, ibl, ssao, contactShadows, ssgi, ddgi, rtSupported, rtShadows, restir, blasCount, pipelines, hdr, exposureEv, aa) |
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
| `set-exposure` | `{ev}` | tonemap exposure in stops (`exp2(ev)`) |
| `set-depth-prepass` | `{0\|1}` | depth pre-pass |

## Asset commands
*(`control_commands_asset.cpp`)*

| Command | Params | Effect |
|---|---|---|
| `import-model` | `{path}` | import + bake a model, spawn an entity carrying it (selected) |
| `import-texture` | `{path}` | import an image into the asset dir; returns its texture id |
| `list-assets` | — | the project catalog `{id, name, type, path}` |
| `rename-asset` | `{id\|name, newName}` | rename a catalog entry |
| `assign-asset` | `{entity, slot:mesh\|albedo, id\|name}` | assign a catalog asset to the entity's Mesh/Material |
| `save-scene` | `{path}` | write the scene JSON |
| `load-scene` | `{path}` | read a scene JSON (deselects) |
| `save-project` | `{path=project.json}` | assets catalog + scene in one file |
| `load-project` | `{path=project.json}` | load catalog + scene (deselects) |
| `screenshot` | `{target:viewport\|window, path}` | PNG; `viewport` is synchronous, `window` is written at end of frame |
| `quit` | — | close the running app |

## Related
- [Control plane](../../explanations/tooling-and-control/) — the socket and how commands dispatch
