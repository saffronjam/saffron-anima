# schemas/control

This directory is no longer the source of truth for command payloads. The control wire contract is
DTO-first:

- source DTOs live in `engine/source/saffron/control/control_dto.cppm`;
- `tools/gen-control-dto/gen.ts` emits the generated OpenRPC document and command manifest here;
- `tools/check-control-schema/check.ts` validates live command results against the generated
  OpenRPC schemas and compares live `help` with the generated manifest.

## Hand-authored file

Only `envelope.schema.json` is hand-authored here. It describes the dispatch wrapper:
`{id?, ok, result? | error?}`. Command result payloads are generated from DTO structs, not
authored as sibling schema files.

## Editing rules

- Do not add new hand-authored payload schemas here.
- Add or change command params/results in `control_dto.cppm`, then run
  `bun run tools/gen-control-dto/gen.ts`.
- Commit regenerated `openrpc.generated.json` and `command-manifest.generated.json`.
- If a command cannot be exercised by the live contract test, add its skip reason or fixture in the
  generator manifest source, not in the test body.
