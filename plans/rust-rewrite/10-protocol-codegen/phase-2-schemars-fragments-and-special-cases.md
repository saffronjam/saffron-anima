# Phase 2 — schemars draft-2020-12 fragments + the OpenRPC special-cases

**Status:** COMPLETED

**Depends on:** 10-protocol-codegen:phase-1-dto-crate-and-derives

## Goal

Make every DTO emit a JSON Schema fragment (draft 2020-12) shaped exactly like `gen.ts`'s `schemaFor`
output, port the hand-authored `componentSchemas()` block (the 21 component shapes + `Vec3`/`Vec4`/`BVec3`
+ `Components`/`ComponentBody`), and encode the four OpenRPC special-case overrides as named emitter
rules. This phase produces the schema *fragments* and proves their shape with unit tests; phase-5
assembles them into the OpenRPC document.

## Why this shape (NO LEGACY)

- **`schemars` is the per-DTO fragment source, not a hand-written `jsonSchemaFor`.** `gen.ts` hand-maps
  each type to a schema (`jsonSchemaFor`, `gen.ts:2115`) and each struct to
  `{ type:"object", additionalProperties:false, properties, required }` (`schemaFor`, `gen.ts:2152`).
  `schemars` (draft 2020-12 — the exact dialect the engine declares) generates that from the derive. The
  emitter consumes `schema_for!(T)` per DTO instead of regex-deriving it.
- **`additionalProperties: false` + `required` = non-`Option`.** The C++ shape sets
  `additionalProperties:false` and `required` to the non-optional fields (`gen.ts:2174`). `schemars` does
  this natively (a struct with no `#[serde(flatten)]` is a closed object; `Option<T>` fields drop out of
  `required`). A test asserts a sample DTO's fragment matches the C++-shaped object byte-for-byte after a
  canonical key sort.
- **The four special-cases are wire-shape facts crossing DTO boundaries**, so they are emitter overrides,
  not schemars attributes: `SelectionResult.entity` → `oneOf:[EntityRef, {type:null}]` (`gen.ts:2162`),
  `InspectResult.components` → `$ref:Components` (`:2165`), `SetComponentParams.json` → `$ref:ComponentBody`
  (`:2168`), `EnvironmentDto` → `$ref:Environment` (`:2153`). They are kept as a small override table keyed
  by `(struct, field)` the emitter applies after `schema_for!`.
- **`componentSchemas()` is hand-authored in C++ and stays hand-authored in Rust.** The 21 component
  shapes + `Vec3`/`Vec4`/`BVec3` + the `Components` aggregate + the `ComponentBody` union (`gen.ts:2178`)
  describe the *opaque blobs* the contract test validates — they are not protocol DTOs, so they have no
  derive to read from. They port as a static schema block (a `serde_json::json!` literal or a typed
  builder) the emitter merges into `components.schemas`. This is the one place the protocol crate carries
  schema knowledge the scene component registry (area 03) actually owns; phase-6 ties the component
  *shapes* here to the registered component DTO set so they cannot drift.
- **`Uuid` reports `"type": "string"`, not `integer`.** `jsonSchemaFor` returns `{type:"string"}` for
  `WireUuid` (`gen.ts:2134`); the `serde_with` schemars integration on the `Uuid` newtype must do the same.
  A bare `u64` would report `integer` and the contract test's type check would pass on a wrong-typed wire
  value. This phase asserts the `Uuid` fragment is `{"type":"string"}`.

## Grounding (real files / symbols)

- `tools/gen-control-dto/gen.ts`: `jsonSchemaFor` (2115, the scalar→schema map), `schemaFor` (2152, the
  struct→object shape + the 4 special-cases at 2153/2162/2165/2168), `componentSchemas` (2178, the 21
  hand-authored component shapes + `Vec3`/`Vec4`/`BVec3`/`Components`/`ComponentBody`).
- `engine-old/source/saffron/control/control_dto.cppm`: `SelectionResult.entity`, `InspectResult.components`,
  `SetComponentParams.json`, `EnvironmentDto` (the four special-cased fields).
- `tools/check-control-schema/check.ts`: `resolveRef`/`typeOk` (the schema validator the fragments feed —
  the consumer this phase's output must satisfy in phase-5).
- `schemas/control/openrpc.generated.json`: the committed `components.schemas` block (the byte-equivalence
  target).

## Acceptance gate

- `cargo build --workspace` succeeds; clippy + fmt clean; `#![deny(unsafe_code)]` holds.
- A `#[test]` emits the schema fragment for a sample plain DTO (e.g. `RaycastParams`) and asserts it equals
  the C++-shaped `{ type:"object", additionalProperties:false, properties:{...}, required:[...] }` (after a
  canonical key sort), with `required` listing exactly the non-`Option` fields in declaration order.
- The `Uuid` fragment is `{ "type": "string" }`; an `f32` field fragment is `{ "type": "number" }`; a
  `u64`/`i32` field fragment is `{ "type": "integer" }` (matching `jsonSchemaFor`).
- The four special-cases are applied: a `#[test]` asserts `SelectionResult.entity` emits
  `oneOf:[$ref EntityRef, {type:null}]`, `InspectResult.components` emits `$ref Components`,
  `SetComponentParams.json` emits `$ref ComponentBody`, and the `EnvironmentDto` schema is `$ref Environment`.
- The `componentSchemas()` block is present and a `#[test]` asserts it carries all 21 component shapes +
  `Vec3`/`Vec4`/`BVec3`/`Components`/`ComponentBody` with the same `properties`/`required` as the committed
  `openrpc.generated.json`.
