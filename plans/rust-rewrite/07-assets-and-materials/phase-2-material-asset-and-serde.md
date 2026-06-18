# Phase 2 — `MaterialAsset` and `.smat` serde

**Status:** COMPLETED

**Depends on:** 07-assets-and-materials:phase-1-crate-skeleton-and-asset-server, 10-protocol-codegen:dto-crate-and-derives (Uuid decimal-string wire)

## Goal

Port `MaterialAsset` (the native `.smat` property bag over the übershader), its byte-compatible
JSON serde (`material_asset_to_json` / `material_asset_from_json`), the instance model (parent + sparse
overrides via `apply_overrides` + `load_material_asset`'s depth-capped recursion), and `default_material_asset`.
Material file read/write (`save_material_asset`, `update_material_asset`, `load_material_asset_raw`).
This is pure CPU + JSON; no GPU, no graph folding (phase 5), no codegen (phase 6).

## Why this shape (NO LEGACY)

`MaterialAsset` ports as a plain struct. The `.smat` JSON is a frozen wire contract: the exact key
spellings (`factors`, `textures.ormOrMr`, `normalConvention`), the nested `factors`/`textures` objects,
named-array vectors (`baseColor` 4-elem, `emissive` 3-elem, `uvTiling`/`uvOffset` 2-elem), and uuid
fields emitted as **decimal strings** (`std::to_string(id.value)`) with a string-or-number read. The
`graph` and `overrides` fields are opaque author/editor JSON trees — they ride as `serde_json::Value`,
not a typed struct, exactly as the C++ holds `nlohmann::json`; trying to model them as typed structs
would be a second source of truth for the editor's node-graph schema (NO LEGACY: one shape, the editor's).
Instance resolution recurses to a fixed depth cap of 8 (the cycle/over-deep guard), keeping `parent` +
`overrides` on the resolved result so the editor still sees an instance.

## Grounding (real files/symbols)

- `engine-old/source/saffron/assets/assets.cppm`: `MaterialAsset` (fields `shader`, `blend`, `unlit`,
  `doubleSided`, `baseColor`, `metallic`, `roughness`, `emissive`, `emissiveStrength`, `normalStrength`,
  `alphaCutoff`, `heightScale`, `uvTiling`, `uvOffset`, `albedoTexture`, `ormTexture`, `normalTexture`,
  `emissiveTexture`, `heightTexture`, `normalConvention`, `features`, `graph`, `parent`, `overrides`),
  `defaultMaterialAsset`, `materialAssetToJson`, `materialAssetFromJson`, `loadMaterialAsset` (depth cap
  8), `loadMaterialAssetRaw`, `applyOverrides`, `saveMaterialAsset`, `updateMaterialAsset`.
- The AGENTS rule: "Material instances are parent + sparse overrides… `0` is a master material.
  `DefaultMaterialId{1}` short-circuits to `defaultMaterialAsset()`."
- Uuid decimal-string emit/read: the `uuid` lambda in `materialAssetFromJson` (`strtoull` on a string,
  or `get<u64>` on a number) and `std::to_string(id.value)` in `materialAssetToJson`.

## Acceptance gate

- `cargo build -p saffron-assets` + workspace green; clippy + fmt clean.
- A round-trip `#[test]`: a populated `MaterialAsset` → `material_asset_to_json` → `material_asset_from_json`
  reproduces every field (factors, textures, flags, `normalConvention`); a byte-equality test against a
  captured C++ `.smat` fixture (key order matching the C++ object-insertion order, uuid fields quoted as
  decimal strings, `version: 1`).
- A `#[test]` proving `apply_overrides` writes only the named fields and leaves the rest; an instance
  `#[test]`: a child with `parent != 0` resolves to the parent's params with overrides on top, keeps
  `parent`+`overrides`, and a cycle / depth-> 8 falls back to the child's own params without infinite
  recursion.
- `default_material_asset` equals `MaterialAsset::default()` (white albedo, roughness 1, metallic 0); a
  `#[test]` asserts no uuid field ever serializes as a JSON number.
