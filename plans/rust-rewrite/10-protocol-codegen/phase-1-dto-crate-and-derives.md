# Phase 1 — `saffron-protocol`: the DTO crate, derives, and the `Uuid` wire newtype

**Status:** COMPLETED

**Depends on:** 00-foundations:phase-2-core-crate

## Goal

Create the `saffron-protocol` crate and transcribe the **236 structs + 17 enums** from
`control_dto.cppm` into idiomatic Rust types deriving `Serialize`/`Deserialize`/`JsonSchema`/`TS`, with
the `Uuid(u64)` newtype carrying the `serde_with::PickFirst<(DisplayFromStr, _)>` attribute that
reproduces the decimal-string wire exactly. This is the single source of truth every later phase, the
`sa` CLI, and `saffron-control` read. No emitters yet — this phase delivers the typed model and proves
the wire encoding byte-for-byte.

## Why this shape (NO LEGACY)

- **The DTO is the model; there is no parser.** `gen.ts` exists only because C++ has no reflection — it
  regex-parses `control_dto.cppm` into a `StructDef[]` (`gen.ts:1168` `parseStructs`). In Rust the struct
  *is* the `StructDef`, and `derive` reads it at compile time. The 3504-line parser and the 167 KB
  `control_dto_serde.generated.cpp` are deleted with no replacement (PP-3 subtraction).
- **One crate, engine-free, shared by three consumers.** The foundations contract puts the DTOs in their
  own crate (pulled out of control) depending only on `saffron-core`, so the engine handlers, the `sa`
  CLI, and `xtask` all link the same types — drift is structurally impossible. `#![deny(unsafe_code)]`
  (not an FFI seam).
- **The `Uuid` newtype is the byte-frozen seam, expressed once.** The C++ `WireUuid` emits a decimal
  string (`uuidToJson`) and reads string-or-number (`readWireUuid`). The whole behavior collapses to one
  `#[serde_as(as = "PickFirst<(DisplayFromStr, _)>")]` attribute — the foundations open-question and
  feasibility §4.6 both pin this exact derive. A bare `u64` would emit a JSON number and silently corrupt
  ids; this phase's contract test is the only automated detector.
- **Field declaration order is preserved verbatim.** Order = positional CLI arg order = OpenRPC `required`
  order (`AGENTS.md` rule). Rust struct field order is the derive emit order, so transcribing fields in
  `control_dto.cppm` order keeps it; `serde_json` is pinned with `preserve_order` so result maps emit in
  field order (matching `nlohmann::ordered_json`).
- **The 17 enums are kebab-case strings, one attribute each.** The C++ `enumWireNames` three-table
  hand-sync (`gen.ts:71`) collapses to `#[serde(rename_all = "kebab-case")]` per enum, with explicit
  `#[serde(rename = "...")]` on the irregular variants (`Msaa2 → "msaa2"`, `MetallicRoughness →
  "metallic-roughness"`). Unknown value is a `Deserialize` error (the C++ `read*` behavior).
- **Opaque blobs stay `serde_json::Value`.** `EntitySelector`/`AssetSelector`/every `Json` field map to
  `Value` (catalog §"Wire-helper types"); they are not typed sub-DTOs. `DtoTag<T>` is dropped.

## Grounding (real files / symbols)

- `engine-old/source/saffron/control/control_dto.cppm`: `WireUuid` (21), `EntitySelector` (26),
  `AssetSelector` (31), `Vec3` (42), `Vec4` (49), `DtoTag` (17, dropped), the 236 structs + 17 enums
  (the full list is `09-control-plane/catalog.md` §"DTO inventory").
- `engine-old/source/saffron/control/control_dto_serde.generated.cpp`: `uuidToJson` (645, the emit),
  `readWireUuid` (157, the accept), `readF32` (87, the f64→f32 narrowing for `f32` fields).
- `tools/gen-control-dto/gen.ts`: `enumWireNames` (71, the 17 enums' kebab spellings to reproduce),
  `scalarTypes` (57, the scalar→Rust mapping), `parseStructs` (1168, the model being subsumed),
  `tsType` (1746, the TS mapping ts-rs replaces).
- `editor/src/protocol/sa-types.ts`: `export type WireUuid = string` (7) — the ts-rs target for `Uuid`.
- Foundations contract: `saffron-core` owns `Uuid(u64)` with the `<1024` reservation; this phase pins
  whether `saffron-protocol` re-exports it with wire derives or wraps it.

## Acceptance gate

- `cargo build -p saffron-protocol` and `cargo build --workspace` succeed; `#![deny(unsafe_code)]` holds;
  `cargo clippy -p saffron-protocol` and `cargo fmt --check` clean.
- All 236 structs + 17 enums + the 4 wire-helpers + `Vec3`/`Vec4` exist and derive
  `Serialize`/`Deserialize`/`JsonSchema`/`TS` (a `#[test]` counts them against the catalog's inventory list
  so a dropped DTO fails the build, not silently).
- **`Uuid` byte-identity tests** (the decimal-string seam):
  - `serde_json::to_string(&Uuid(42))` == `"\"42\""` (a quoted decimal string, **not** `42`).
  - `serde_json::from_str::<Uuid>("\"42\"")` and `from_str::<Uuid>("42")` both yield `Uuid(42)`
    (string-or-number accept).
  - Round-trip for a `>2^53` value (e.g. `18446744073709551615`) preserves the full `u64`.
  - **Cross-encoder identity**: the `Uuid` derive output == `saffron_json::uuid_to_json(42)` output
    byte-for-byte, and `json_u64` parses the `Uuid` emit back (the PP-7/PP-13 contract test the
    foundations open-question names; if `saffron-json` lands after this phase, the test is stubbed against
    a literal `"\"42\""` and tightened to call `uuid_to_json` once that crate exists).
- Enum round-trips: `to_string(&AaModeDto::Msaa4)` == `"\"msaa4\""`, `GizmoOpDto::Translate` ==
  `"\"translate\""`, `AssetSlotDto::MetallicRoughness` == `"\"metallic-roughness\""`; an unknown value
  (`from_str::<AaModeDto>("\"bogus\"")`) returns an `Err`, not a default.
- A representative `*Params` with an `Option<T>` field (`RaycastParams.maxDist`) serializes the absent
  case as a **missing key**, not `null` (`#[serde(skip_serializing_if = "Option::is_none")]`), matching the
  C++ `optionalField` behavior.
