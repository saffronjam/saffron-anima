# Phase 03 — `AssetType::Model` + catalog sub-asset rows

**Status:** COMPLETED
**Depends on:** 02

## Goal

Add `AssetType::Model` to the scene enum and extend `AssetEntry` with optional **container linkage** so a
sub-asset row knows the `.smodel` it lives in and which chunk it is. Update `assetTypeName` /
`assetTypeFromName`, `catalogToJson` / `catalogFromJson`, and regenerate the scene serde via `gen.ts`.
Existing standalone assets keep working unchanged. Defers: actually scanning containers into rows (09),
the writer that emits them (05). No `ProjectVersion` bump.

## Why

The catalog must be able to express "this row is the Sponza model" (one parent entry) and "this row is a
mesh/material/texture *inside* that model" (sub-asset entries that resolve to a chunk). Without the
linkage fields, a scan can't tell a standalone `materials/<uuid>.smat` from an embedded material chunk,
and the editor can't render a model as one expandable tile (phase 16). This is the catalog shape the rest
of the plan writes against.

## The data model changes

`AssetType` (`scene.cppm:331`) gains `Model`; forward-compat is already handled (unknown → `Other`).

`AssetEntry` (`scene.cppm:340`) gains optional container linkage. Today it is
`{id, name, type, path, folder, hdr, linear, duration}`:

```cpp
enum class AssetType { Mesh, Texture, Material, Animation, Model, Other };

struct AssetEntry {
    Uuid        id;                 // for a sub-asset, the stable sub-id (unique across the catalog)
    std::string name;
    AssetType   type;
    std::string path;               // for a sub-asset: the OWNING .smodel's path; for standalone: its own file
    std::string folder;
    bool        hdr   = false;
    bool        linear = false;     // superseded by colorspace provenance in phase 10, kept for now
    f32         duration = 0.0f;
    // --- new: container linkage (absent ⇒ standalone file) ---
    Uuid        container = 0;      // 0 = standalone; else the owning model's id
    i32         chunk     = -1;     // TOC chunk index inside the container (-1 = n/a)
    Colorspace  colorspace = Colorspace::Auto;  // (phase 10 fills it; reserve the field now)
};
```

`AssetCatalog{entries, folders, byId}` is unchanged structurally; `byId` keeps mapping **every** id
(parent models and sub-assets) → entry index, so existing uuid lookups (`loadMeshAsset` etc.) still work
once sub-assets carry their own ids.

## Serde

- `assetTypeName` / `assetTypeFromName` (`assets.cppm`) gain `"model"`.
- `catalogToJson` / `catalogFromJson` (`assets.cppm:289`/`338`) round-trip `container`/`chunk`/`colorspace`
  (omit when default, for compact `project.json`). Note: per the plan's decision, `project.json` will stop
  being the catalog source (phase 09) — but keep the serde correct in the interim so nothing breaks
  between phases.
- Regenerate via `bun run tools/gen-control-dto/gen.ts` if `AssetType`/`AssetEntry` surface in any DTO or
  scene-component serde; **never hand-edit** the `.generated` files. The contract test must stay green
  (uuids as decimal strings).

## Files to touch

- `engine/source/saffron/scene/scene.cppm` — add `Model` to `enum class AssetType`; add `container`,
  `chunk`, `colorspace` to `AssetEntry`; add a `Colorspace { Auto, Srgb, Linear, Hdr }` enum.
- `engine/source/saffron/assets/assets.cppm` — extend `assetTypeName`/`assetTypeFromName`,
  `catalogToJson`/`catalogFromJson`.
- `tools/gen-control-dto/gen.ts` + regenerated outputs — only if these types are reflected into serde.

## Steps

1. Add the `Model` enum value + name conversions (unknown still falls back to `Other`).
2. Add `Colorspace` and the three new `AssetEntry` fields with defaults that mean "standalone, no
   container" so existing rows deserialize unchanged.
3. Extend `catalogToJson`/`catalogFromJson` to write/read the new fields only when non-default.
4. Run `gen.ts`; confirm the contract test passes and the serde round-trips a mixed catalog (one model +
   two sub-assets + one standalone).
5. Unit/self-test: build an `AssetCatalog` with a model parent + sub-asset rows, `catalogToJson` →
   `catalogFromJson`, assert equality incl. linkage fields and `byId` resolution.

## Gate / done

- `make engine` clean; catalog round-trip self-test passes; if serde regenerated, `make e2e` + the
  control-schema contract test pass; `make prepare-for-commit` clean.
- No `ProjectVersion` bump (old projects still load).

## Risks

- **`byId` uniqueness:** sub-asset ids must be globally unique in the catalog (they are minted as real
  uuids at bake). If two containers ever shared a sub-id, `byId` would collide — assert uniqueness on
  catalog build and warn.
- **Interim `project.json` compatibility:** between this phase and phase 09 the catalog still serializes
  to `project.json`; keep the new fields optional so a project saved now and loaded after 09 (scan-based)
  doesn't fight. Since migration is out of scope this is low-risk, but don't make the fields required.
- **Serde regen scope creep:** only touch `gen.ts` if these types are actually reflected; a needless
  regen churns five generated files and the contract fixtures.
