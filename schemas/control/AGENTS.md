# schemas/control — the wire contract

These hand-authored JSON Schemas (draft 2020-12) are the **single source of truth** for the
control-plane wire format between the engine and the editor. They are not documentation —
they are the origin that downstream artifacts are generated from and validated against.

## What flows from here

- **Editor types:** `editor/scripts/gen-protocol.ts` bundles every `*.schema.json` into one
  `$defs` map, rewrites cross-file `$ref`s to internal refs, and compiles to
  `editor/src/protocol/index.ts` via `json-schema-to-typescript`. Run with
  `bun run gen:protocol` (also part of `bun run check` / `bun run build`).
- **Contract test:** `tools/check-control-schema/` spawns live `se` commands and validates
  the engine's actual output against these schemas. It is a stage of the `tools/ci/check.sh`
  gate, so a schema that drifts from the engine fails the build.

## Editing rules

- **Edit the schema, then regenerate — never hand-edit `editor/src/protocol/index.ts`.**
- `title` is the generated TypeScript type name; keep it PascalCase and stable.
- Cross-schema references use `$ref` to the sibling file (e.g. `"uuid.schema.json"`); the
  codegen rewrites them to `#/$defs/<Title>`.
- Prefer `additionalProperties: false` (strict) so drift is caught.
- u64 IDs are represented as **decimal strings** (`uuid.schema.json`: `type: "string"`,
  pattern `^[0-9]+$`). Ids span the full u64 range, past JS's 2^53, so a string is the only
  form that survives `JSON.parse` without precision loss.
- When you add or change an engine DTO, update its schema here in the same change, and make
  sure the matching `Saffron.Control` command (and `dump-schema`, where relevant) still
  produces conforming output.

## Files

Shared types: `uuid`, `vec3`, `vec4`, `transform`, `entity-ref`, `envelope`.
Components: `name`, `mesh`, `material`, `camera`, `directional-light`, `point-light`,
`spot-light`, `environment`.
Results: `entity-list`, `selection`, `inspect-result`, `components`, `asset-entry`,
`asset-list`, `thumbnail`, `render-stats`, `gizmo-state`, `editor-camera`.
