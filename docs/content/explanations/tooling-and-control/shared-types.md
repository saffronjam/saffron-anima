+++
title = 'Shared types'
weight = 7
+++

# Shared types

Shared types are the data shapes carried over the control wire. They are authored as C++ DTO structs in `Saffron.Control` and generated outward to TypeScript, OpenRPC schemas, and the command manifest. The generated artifacts are committed and checked for freshness, so a command cannot drift silently between the engine, editor, CLI tooling, and docs.

## How it works

```
engine/source/saffron/control/control_dto.cppm   (DTO source of truth)
        |
        `-- tools/gen-control-dto/gen.ts
              |-- control_dto_serde.generated.cpp
              |-- editor/src/protocol/se-types.ts
              |-- schemas/control/openrpc.generated.json
              `-- schemas/control/command-manifest.generated.json
```

Handlers use `registerCommand<Params, Result>`. The overload erases the typed handler down to the same `Result<json>(EngineContext&, const json&)` row the dispatcher already uses: generated serde parses `Params`, the handler returns a `Result` DTO, and generated `dtoToJson` writes the reply payload.

The live contract test is now a manifest tripwire. It calls `help`, verifies the live registry and generated manifest contain the same command names, then exercises every non-skipped command with a manifest fixture and validates the result against `openrpc.generated.json`. The only remaining hand-authored schema is `schemas/control/envelope.schema.json`, because the `{ok,error,result,id}` wrapper is owned by dispatch rather than by a command DTO.

## Wire invariants

These hold across the whole protocol:

- **IDs are u64, carried as decimal strings.** Every `WireUuid` result is emitted as a decimal JSON string, so JavaScript never rounds it through `number`. Reads still accept a string or a number for CLI convenience. The raw-byte contract test keeps the quoted-decimal invariant after `uuid.schema.json` was retired.
- **camelCase on the wire.** DTO field names are the wire keys and the generated TypeScript field names.
- **`Transform.rotation` is Euler XYZ radians.** UIs that show degrees convert at the edge.
- **Spot-light angles are degrees.** `innerAngle` and `outerAngle` remain degrees on the wire.
- **Camera uses `near`/`far`.** ECS cameras and the editor fly-camera use the same key names.

Component bodies and the scene environment use generated scene serde as well. `inspect.components` is validated as a registry-keyed map of component DTOs, and the editor protocol exposes those component body types for reads while still accepting registry-shaped records for generic `set-component` writes.

## In the code

| What | File | Symbols |
|---|---|---|
| DTO source of truth | `engine/source/saffron/control/control_dto.cppm` | `WireUuid`, params DTOs, result DTOs |
| Typed command registration | `engine/source/saffron/control/command.cppm` | `registerCommand<Params, Result>` |
| Generated C++ serde | `engine/source/saffron/control/control_dto_serde.generated.cpp`, `engine/source/saffron/scene/scene_component_serde.generated.cpp` | `parseDto`, `dtoToJson`, component serde functions |
| Generator | `tools/gen-control-dto/gen.ts` | DTO parser, TS/OpenRPC/manifest emitters |
| Editor protocol types | `editor/src/protocol/se-types.ts`, `editor/src/protocol/index.ts` | `CommandParamsMap`, `CommandResultMap` |
| Live contract test | `tools/check-control-schema/check.ts` | manifest completeness, OpenRPC validation, raw u64 check |

## Related
- [se CLI](../se-cli-protocol/) — the request/response shape and token coercion these types describe
- [Scene commands](../scene-commands/) — component editing and selection counters
- [Asset commands](../asset-commands/) — project, catalog, and thumbnail commands
- [Control plane](../control-plane-architecture/) — how typed handlers are registered and dispatched
