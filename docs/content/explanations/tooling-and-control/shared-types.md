+++
title = 'Shared types'
weight = 7
+++

# Shared types

Shared types are the data shapes carried over the control wire, defined once as a set of JSON Schemas. Every consumer of the protocol — the TypeScript client and the C++ engine — is derived from or checked against those schemas, so a field cannot mean one thing on the wire and another in a consumer. The schemas live in `schemas/control/*.schema.json` and are written to JSON Schema **draft 2020-12**.

## How it works

```
schemas/control/*.schema.json   (draft 2020-12 — the source of truth)
        │
        ├── json-schema-to-typescript ──▶ the TS protocol types   (phase 3)
        │
        └── tools/check-control-schema  ──▶ validates the C++ replies
```

The schema is authored first. From it, `json-schema-to-typescript` generates the TypeScript protocol the UI imports; the TS side is **generated**, never hand-maintained.

The C++ side goes the other way: it is a **validated consumer**, not a generator. There are **no named C++ DTO structs**, and every command builds its response as inline `nlohmann::json`. A contract test, `tools/check-control-schema`, drives the running editor, captures real replies, and validates them against the schemas. If a reply drifts from its schema, the test fails. That keeps the inline JSON honest without a parallel hierarchy of C++ types to maintain.

## Deferred forward seams

`dump-schema` (see [Scene commands](../scene-commands/)) and [reflect-cpp](https://github.com/getml/reflect-cpp) are seams for generating the schemas from C++ types directly. That direction needs C++26 static reflection, which is not in stock Clang 21 + libc++ yet, so the schemas are hand-written and the contract test guards them. `dump-schema` already emits the live runtime shapes, so when reflection lands the generation direction can flip without changing the wire.

## Wire invariants

These hold across the whole protocol, in both the schemas and every reply:

- **IDs are u64, carried as decimal strings.** Every `Uuid`/`id` is a 64-bit unsigned integer emitted as a decimal JSON **string** (`"id": "12884901889"`). An id can exceed 2^53, past what a JavaScript `number` holds exactly, so a string is the only form that survives `JSON.parse` losslessly. The `uuid` schema is `type: "string"` with pattern `^[0-9]+$`, so an id is typed `string` end-to-end. Reads accept a string or a number, but never round-trip an id through a plain JS `number`. The [id-encoding contract](../control-plane-architecture/#id-encoding-on-the-wire) covers this in full.
- **camelCase on the wire.** Every key is camelCase (`baseColor`, `albedoTexture`, `emissiveStrength`), matching the scene-file encoding and the generated TS field names.
- **`Transform.rotation` is Euler XYZ radians.** The wire value is radians; a UI that shows degrees converts at the edge. (This matches `set-transform`, which merges radians.)
- **Spot-light angles are degrees.** `SpotLightComponent.innerAngle` / `outerAngle` are in **degrees** on the wire, unlike the transform rotation — they are authored as degrees and stay degrees.
- **Camera uses `near`/`far`.** The camera near/far planes are the keys `near` and `far` (not `nearPlane`/`zNear`), for both ECS cameras and the editor fly-cam.

The schema is where these are pinned: the `uuid` type's string form, the per-field units, and the key casing are all stated once and inherited by everything generated from or checked against it.

## In the code

| What | File | Symbols |
|---|---|---|
| Schemas (source of truth) | `schemas/control/*.schema.json` | the `uuid`, component, environment, and render-stats schemas |
| TS generation | `json-schema-to-typescript` | the generated protocol types (phase 3) |
| C++ contract test | `tools/check-control-schema` | drives the editor, validates replies against the schemas |
| Live shapes | `control_commands_scene.cpp` | `dump-schema` |
| Replies (no DTOs) | `control_commands_*.cpp` | inline `nlohmann::json` per command |

## Related
- [se CLI](../se-cli-protocol/) — the request/response shape and token coercion these types describe
- [Scene commands](../scene-commands/) — `dump-schema` and the camelCase component bodies
- [Asset commands](../asset-commands/) — the base64-PNG thumbnail result shape
- [Control plane](../control-plane-architecture/) — how a reply is built and dispatched
