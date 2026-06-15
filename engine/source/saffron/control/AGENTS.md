# Saffron.Control

The control plane: a JSON-over-unix-socket server that lets the Tauri editor and the `sa` CLI
drive and inspect the running host. Module `Saffron.Control`, partitions `:Command` and `:Dto`,
namespace `sa`. Uses classic `#include` in the global module fragment and does **not** `import std`.

## Files

| File | Role |
|---|---|
| `command.cppm` | Partition `:Command`: command registry, `EngineContext`, typed `registerCommand<Params, Result>`, selectors, control context handles. |
| `control_dto.cppm` | Partition `:Dto`: DTO source of truth for command params/results and wire helper types. |
| `control_dto_serde.generated.cpp` | Generated parse/serialize functions for DTOs. Do not edit by hand. |
| `control.cppm` | Outer module, re-exports partitions. |
| `control_server.cpp` | Socket setup, dispatch, the per-frame non-blocking drain. |
| `control_commands_render.cpp` | Render stats, AA/clustering/GI toggles, native viewport bridge commands. |
| `control_commands_scene.cpp` | Entity lifecycle, components, selection, picking, camera, gizmo, environment. |
| `control_commands_asset.cpp` | Import, catalog, thumbnails, project/scene save and load. |
| `control_commands_animation.cpp` | Animation playback, clip/skeleton, and foot-IK commands. |

## Protocol

Newline-delimited JSON over a unix socket. Path resolution: `SAFFRON_CONTROL_SOCK` if set, else
`$XDG_RUNTIME_DIR/saffron-control.sock`, else `/tmp/saffron-control-<uid>.sock` (mode 0600).

```json
request:  { "id": <opt>, "cmd": "<name>", "params": { ... } }
response: { "id": <echoed>, "ok": true|false, "result": { ... } | "error": "<msg>" }
```

The envelope is defined by `schemas/control/envelope.schema.json`. Command params/results are
defined by DTOs and generated outward to C++ serde, TypeScript, OpenRPC, and the manifest.

## Adding a command

1. Declare params and result DTO structs in `control_dto.cppm`.
2. Add the command to `tools/gen-control-dto/gen.ts`, including its fixture or skip reason for the
   manifest-driven contract test.
3. Register it with the typed overload:

```cpp
registerCommand<MyParams, MyResult>(registry, "my-command", "one-line help",
    [](EngineContext& ctx, const MyParams& params) -> Result<MyResult>
    {
        return MyResult{ ... };
    });
```

4. Run `bun run tools/gen-control-dto/gen.ts` and commit all five generated outputs: the control DTO
   serde (`control_dto_serde.generated.cpp`), the scene-component serde
   (`engine/source/saffron/scene/scene_component_serde.generated.cpp`), the TypeScript
   (`editor/src/protocol/sa-types.ts`), the OpenRPC (`schemas/control/openrpc.generated.json`), and the
   manifest (`schemas/control/command-manifest.generated.json`).

Conventions:

- DTO field declaration order is the positional CLI argument order.
- Use `EntitySelector` / `AssetSelector` for id-or-name inputs.
- IDs are `WireUuid` at the DTO boundary and are emitted as decimal JSON strings. The contract test
  checks raw bytes so ids never regress to JSON numbers.
- Keep the `sa` CLI usable. A command that adds drivable or inspectable state should also be
  reachable from `tools/sa`.
