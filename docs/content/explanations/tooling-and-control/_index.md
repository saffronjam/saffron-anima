+++
title = 'Tooling & control'
weight = 15
+++

# Tooling & control

A running editor is scriptable. A non-blocking unix socket, drained once per frame on the main thread, lets the `se` CLI speak JSON to create entities, set components, import assets, toggle render features, and grab screenshots. Every new engine feature is expected to ship a matching command so the editor stays drivable from a script.

## Pages

| Page | Covers | Code |
|---|---|---|
| `control-plane-architecture` | the socket, `registerCommand`, the command itable, per-frame drain | `control.cppm`; `control_server.cpp` |
| `se-cli-protocol` | the JSON request/response shape, argument coercion, no engine dependency | `tools/se` |
| `scene-commands` | list/create/destroy/select, add/set component, set-transform, pick, focus, inspect | `control_commands_scene.cpp` |
| `render-commands` | set-aa / set-clustered / set-ibl / set-ssao / set-ssgi / set-shadows / set-gi / set-exposure / set-depth-prepass, render-stats | `control_commands_render.cpp` |
| `asset-commands` | import-model/texture, list/rename/assign-asset, save/load project | `control_commands_asset.cpp` |
| `screenshots-and-capture` | viewport vs. window PNG, deferred swapchain capture | `control_commands_asset.cpp`; `renderer_capture.cpp` |
