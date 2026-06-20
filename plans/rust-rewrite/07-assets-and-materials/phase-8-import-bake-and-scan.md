# Phase 8 — import, bake, and scan

**Status:** COMPLETED

**Depends on:** 07-assets-and-materials:phase-3-container-metadata-and-model-open, 02-math-and-geometry:phase-5-gltf-import, 02-math-and-geometry:phase-6-obj-import-image-decode-subid, 02-math-and-geometry:phase-7-smodel-container

> **Substrate added later (asset-command unblock):** the original pass landed `bake_model` /
> `import_model` / `scan_assets` / texture register but left the management surface the asset
> commands call unimplemented. `crates/assets/src/manage.rs` now fills it: `reimport_model`
> (`ReimportDelta`), `import_material_folder` (`MaterialImportResult`), `extract_sub_asset` /
> `clear_extraction` (the sub-asset extraction + remap round-trip, with the `default_extract_dest` /
> `image_ext_from_bytes` / `rewrite_container_meta` helpers), and the read-only `DependencyGraph` +
> `asset_bytes` / `build_dependency_graph` plus `analyze_clean` (`CleanReportData` / `CleanCandidate` /
> `CleanCategory`) and `delete_unused` (`DeleteUnusedData`) — the substrate for `reimport-model`,
> `material-import`, `extract-subasset`/`clear-extraction`, `asset-references`, and
> `clean-assets`/`delete-unused`. All exported from `lib.rs`; covered by `manage::tests`.

## Goal

Port the disk-side import pipeline: `bake_model` (an `ImportedModel` → one self-contained `.smodel`
container with mesh/material/texture/clip chunks + a META chunk), `import_model` (the thin
translate-then-bake wrapper), `reimport_model` (replay the stored recipe with the same `model_id`),
`catalog_rows_for_model` (derive parent + sub-asset catalog rows from a META), `scan_assets` /
`load_catalog` (filesystem-as-source-of-truth reconciliation via the regenerable catalog cache),
`write_catalog_cache`, plus standalone texture register/import (`register_texture_bytes`,
`register_hdr_texture_bytes`, `import_texture`) and `import_material_folder` / `detect_material_role`.

## Why this shape (NO LEGACY)

Bake is pure disk + catalog (no GPU, no spawn). The reimport recipe stored in the META is the
deterministic replay contract: source path, **content hash (not mtime)**, `ImporterVersion`, and
`ImportOptions` (recorded intent). Sub-ids are stable via geometry's `sub_id_for` keyed by source name,
so a reimport reuses the same ids and the soft references in spawned entities stay valid.
`catalog_rows_for_model` is shared by bake and scan so a freshly-baked container and a rediscovered one
yield **identical** rows (the `rigged` flag from META skin presence; an extracted/remapped sub-asset
points its row at the external file with `container = 0`, `chunk = -1`). The filesystem is the source of
truth: a never-saved import (a `.smodel` on disk the `project.json` never recorded) is rediscovered on
the next scan, and a deleted file's row is dropped — so an unsaved import can never become a dead
orphan. Texture register writes the encoded bytes + a catalog row + seeds the GPU cache. `walkdir`
replaces the C++ directory iteration; the scan order must be deterministic (sorted) so the catalog cache
is reproducible.

## Grounding (real files/symbols)

- `engine-old/source/saffron/assets/assets.cppm`: `bakeModel` (the `Pending` chunk list with META at
  index 0, `subIdFor` mesh/texture/material/clip ids, `meta.import.sourceHash = hashFileFnv`,
  `ImporterVersion`, `ImportOptions::toJson`), `importModel`, `reimportModel`, `catalogRowsForModel`
  (the parent + sub-asset rows, `rigged`, the remap → external-file row), `scanAssets`, `loadCatalog`,
  `writeCatalogCache`, `ScanDelta` (`{added, removed}`), `BakeResult` (`{modelId, path, rows}`),
  `ImportOptions` (+ `Axis`, `colorspaceFor`), `registerTextureBytes`, `registerHdrTextureBytes`,
  `importTexture`, `importMaterialFolder`, `detectMaterialRole`, `ensureBuiltinModelAsset`.
- Upstream geometry: `translate_model` (glTF/OBJ → `ImportedModel`), `sub_id_for`, `save_mesh_to_buffer`/
  `save_mesh_skinned_to_buffer`, the `.smodel` `write_container`, `hash_file_fnv`.
- The AGENTS rule: "`project.json` is version-gated… the filesystem is the source of truth, so an
  unsaved import can never become a dead orphan."

## Acceptance gate

- `cargo build -p saffron-assets` + workspace green; clippy + fmt clean.
- A bake `#[test]` over a real fixture (`engine-old/assets/models/cube.gltf` copied to the test fixtures):
  `bake_model` writes a `.smodel`, returns a `BakeResult` with a Model parent row + one mesh + N material
  + N texture sub-asset rows; the sub-ids are stable across two bakes of the same source (re-import
  determinism).
- `catalog_rows_for_model` `#[test]`: the rows derived from a freshly-baked container's META equal the
  rows derived from re-reading that container's META off disk (bake/scan agreement); a remapped sub-asset
  row points at the external path with `container == 0`.
- `scan_assets`/`load_catalog` `#[test]`s: a fresh scan over a dir with one `.smodel` adds its rows; a
  scan after deleting the file removes the rows; the scan order is deterministic across runs (the
  walkdir results are sorted); `register_texture_bytes` writes the file, adds a Texture row, and seeds
  the texture cache.
