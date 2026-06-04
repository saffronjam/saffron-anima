+++
title = 'Tooling & control'
weight = 15
+++

# Tooling & control

The control plane is a JSON-over-unix-socket protocol that drives a running editor from outside the process. The socket is non-blocking and drained once per frame on the main thread, so commands apply between frames without stalling the render loop. Through it the `se` CLI creates entities, sets components, imports assets, toggles render features, and grabs screenshots. Each engine feature ships a matching command, which keeps the editor scriptable and visually debuggable from a shell.

## Pages

| Page | Covers | Code |
|---|---|---|
| `control-plane-architecture` | the socket, `registerCommand`, the command itable, per-frame drain | `control.cppm`; `control_server.cpp` |
| `se-cli-protocol` | the JSON request/response shape, argument coercion, no engine dependency | `tools/se` |
| `scene-commands` | list/create/destroy/select, add/copy/rename entity, set component(-field), camera, gizmo, dump-schema, pick, focus, inspect | `control_commands_scene.cpp` |
| `render-commands` | set-aa / set-clustered / set-ibl / set-ssao / set-ssgi / set-shadows / set-gi / set-exposure / set-depth-prepass, render-stats | `control_commands_render.cpp` |
| `asset-commands` | import-model/texture, list/rename/assign-asset, get-thumbnail/view-asset, save/load project | `control_commands_asset.cpp` |
| `screenshots-and-capture` | viewport vs. window PNG, deferred swapchain capture | `control_commands_asset.cpp`; `renderer_capture.cpp` |
| `shared-types` | schema-first wire contract: schemas → TS codegen + C++ contract test, wire invariants | `schemas/control/*`; `tools/check-control-schema` |
