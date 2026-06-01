# Part 3 — split `control/control.cppm` (1071 lines)

> Read `README.md` in this folder first for the validated mechanism + build/gate rules.

## Context
`engine/source/saffron/control/control.cppm` is `module Saffron.Control`, imports
`{Core, Json, Window, Rendering, Scene, Editor, Assets}`. It packs the command-registry
infrastructure, ~24 built-in commands (registered in one `registerBuiltinCommands`), and a
non-blocking unix-socket server, all in one file. Goal: split into one interface partition
(shared types + decls) + `.cpp` impl units, same pattern as the renderer. Pure reorg.

## Current surface
Types: `EngineContext`, `CommandTraits`, `CommandRegistry`, `ControlClient`,
`ControlServer`, `ControlContext`, and `using json = nlohmann::json;`.
Infra fns: `registerCommand`, `findCommand`, `positionalOr`, `asString`, `resolveEntity`,
`entityRef`. Server/API fns: `controlSocketPath`, `startControlServer`, `stopControlServer`,
`dispatch`, `drainControlServer`, `newControlContext`, `destroyControlContext`, `pollControl`,
`registerBuiltinCommands`. Plus the 24 command lambdas inside `registerBuiltinCommands`.

## Target files (under `engine/source/saffron/control/`)
| File | kind | contents |
|---|---|---|
| `command.cppm` | **`:Command` interface partition** | `EngineContext`, `CommandTraits`, `CommandRegistry`, `using json` (exported here, ONCE), `registerCommand`, `findCommand`, `positionalOr`, `asString`, `resolveEntity`, `entityRef` (defs can live here — small), + **declarations** for `registerRenderCommands`/`registerSceneCommands`/`registerAssetCommands(CommandRegistry&)` and the public API (`controlSocketPath`, `newControlContext`, `destroyControlContext`, `pollControl`, `registerBuiltinCommands`, `ControlServer`/`ControlContext`/`ControlClient` structs). GMF: nlohmann/json, entt, glm. |
| `control_commands_render.cpp` | impl unit | `registerRenderCommands(reg)` = ping, help, render-stats, set-aa, set-clustered, set-postprocess, set-depth-prepass |
| `control_commands_scene.cpp` | impl unit | `registerSceneCommands(reg)` = list-entities/-components, create/destroy-entity, add/remove/set-component, set-transform/-material/-light, select, pick, inspect, focus |
| `control_commands_asset.cpp` | impl unit | `registerAssetCommands(reg)` = import-model/-texture, list/rename/assign-asset, save/load-scene, save/load-project, screenshot, quit |
| `control_server.cpp` | impl unit | the socket layer (`controlSocketPath`, `startControlServer`, `stopControlServer`, `dispatch`, `drainControlServer`, `ControlContext` lifecycle, `pollControl`) + `registerBuiltinCommands` (now just calls the three `registerX`). Owns the `<sys/socket.h>`/`<sys/un.h>`/`<unistd.h>` GMF includes. |
| `control.cppm` (primary, edit in place) | primary interface | GMF + `export module Saffron.Control;` + `export import :Command;` + the module imports. No definitions. |

`registerBuiltinCommands` is split: the 24 registrations move into the three
`registerXCommands(CommandRegistry&)` helpers (declared in `:Command`, defined in the
command impl units). The surviving `registerBuiltinCommands` body becomes:
```cpp
void registerBuiltinCommands(CommandRegistry& reg)
{ registerRenderCommands(reg); registerSceneCommands(reg); registerAssetCommands(reg); }
```
**Keep the render → scene → asset order** — `help`/`list-components` iterate the registry in
insertion order. Each command impl unit `import :Command;` + `import Saffron.Core; Json;` +
the sibling modules its commands touch (Rendering / Scene / Editor / Assets / Window).

## Steps (build `-j1` + gate after each)
1. Create `command.cppm` (`:Command`) with the shared types + infra fns + the `registerX`
   and public-API decls. Gut `control.cppm` to GMF + `export module Saffron.Control;` +
   `export import :Command;` + imports. Add `:Command` to `FILE_SET CXX_MODULES` before
   `control.cppm`. Build `-j1` (the command bodies + server still in… no — see step 2).
   *(Tip: move the bodies in the same step, or temporarily keep them in the primary and
   migrate next — either builds. Migrating the bodies as you go avoids a giant intermediate.)*
2. Create `control_commands_{render,scene,asset}.cpp` + `control_server.cpp`, moving the
   command lambdas + server fns out of `control.cppm`. Add each `.cpp` to
   `target_sources(SaffronEngine PRIVATE …)`. Build `-j1` after each.

## Verify
Build `-j1` green. Bounded headless run; the log shows `control socket listening on …`.
With the editor running: `se ping`, `se render-stats`, `se list-entities`, `se list-components`
(order unchanged: render → scene → asset), `se screenshot viewport`, `se set-aa msaa4`,
`se quit`. All succeed exactly as before.

## Risks
- Export `using json = nlohmann::json;` from **exactly one** partition (`:Command`) — a
  double-export is an error. Every command impl unit then sees `se::json` via `:Command`.
- Each command impl unit needs its own GMF includes (`<entt/entt.hpp>` for `entt::null`/
  `forEach`/`getComponent`, glm, nlohmann/json). A missing include = compile error; mirror
  the current `control.cppm` include set.
- `dispatch`/`drainControlServer` use `parseJson`/`dumpJson`/`jsonStringOr` (Saffron.Json) +
  `findCommand` (`:Command`) — `control_server.cpp` imports both `:Command` and `Saffron.Json`.

## Critical files
`engine/source/saffron/control/control.cppm` (+ new `command.cppm` + 4 `.cpp`),
`engine/CMakeLists.txt`.
