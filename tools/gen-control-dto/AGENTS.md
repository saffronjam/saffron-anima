# tools/gen-control-dto

The control-plane code generator. One Bun/TypeScript file, `gen.ts`, is the single source that fans the
DTOs in `engine/source/saffron/control/control_dto.cppm` out to **five** generated artifacts. The editor
reaches it through `editor/scripts/gen-protocol.ts` (a spawn wrapper); CI (`tools/ci/check.sh`) runs it
and then `git diff --exit-code` on the outputs, so a stale generated file fails the gate.

## Outputs

| What | File | Emitter |
|---|---|---|
| C++ DTO serde | `engine/source/saffron/control/control_dto_serde.generated.cpp` | `emitCpp` |
| Scene-component serde | `engine/source/saffron/scene/scene_component_serde.generated.cpp` | `emitSceneSerde` |
| TS protocol types | `editor/src/protocol/se-types.ts` | `emitTs` |
| OpenRPC schema | `schemas/control/openrpc.generated.json` | `emitOpenRpc` |
| Contract manifest | `schemas/control/command-manifest.generated.json` | `emitManifest` |

Regenerate + commit all five with `bun run tools/gen-control-dto/gen.ts`. `make format` deliberately
skips `*.generated.cpp` (the generator owns their style); never hand-edit a generated file.

## How it reads the source

`control_dto.cppm` is parsed with restrictive regex, not a real C++ parser: a struct member containing
`(`, `)`, or `=` makes it throw (no methods, defaults, or inline initializers in a DTO). A DTO field's
**declaration order is the positional CLI argument order**. `DtoTag` markers are stripped from the map.

## Rules that are easy to break

- **`emitSceneSerde` is hand-written, not derived.** Despite the `Produced … from the scene component
  DTO catalog` header it stamps, its ~600-line body is a static template literal — it is *not* generated
  from the parsed DTOs. The TS `componentInterfaces` block and `componentSchemas()` are hand-written too.
  A single scene-component field lives in four places that must move together: the `.cppm` DTO, the TS
  interface, the OpenRPC component schema, and the hand-written `*ToJson`/`*FromJson` — the generator
  will **not** catch drift between them.
- **A new enum needs three hand-maintained tables.** The C++ enum is parsed, but its wire strings are
  not: add it to `enumWireNames` (C++ + OpenRPC), the `tsType` switch (TS), and `jsonSchemaFor` (schema),
  or it silently mis-emits or throws at codegen.
- **Several struct/field names are special-cased inline** across the emitters — `EnvironmentDto`,
  `SelectionResult.entity`, `InspectResult.components`, `SetComponentParams.json`. Renaming one breaks
  emission invisibly; grep the emitters before renaming a referenced DTO type or field.
- **A DTO only emits if a command transitively reaches it.** Per-target root sets differ and inject a few
  types by hand (`Vec3`/`Vec4` for C++; `Vec3`/`Vec4`/`ProbeRef` for TS).
- **Every command needs a fixture or a skip reason.** `emitManifest` throws if a command is absent from
  both `commandFixtures` and `commandSkips`.
- **IDs cross the wire as decimal strings.** `WireUuid` serializes via `std::to_string`, never a JSON
  number; the contract test checks the raw bytes so this never regresses.

See `engine/source/saffron/control/AGENTS.md` for the per-command authoring workflow and the root
`AGENTS.md` for the keep-the-`se`-CLI-current rule.
