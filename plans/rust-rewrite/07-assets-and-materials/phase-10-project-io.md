# Phase 10 — project I/O

**Status:** COMPLETED

**Depends on:** 07-assets-and-materials:phase-8-import-bake-and-scan, 03-ecs-and-scene:phase-7-scene-document-and-migrations (scene_to_json/from_json), 06-rendering (render settings serde), 11-thumbnail-worker (clear on switch)

## Goal

Port the `project.json` lifecycle: `save_project` (bundle version + name + displayName + catalog +
folders + scene + render settings + optional editorCamera/debugOverlays), `load_project` (version-gate,
idle-before-clear, catalog-from-doc-then-reconcile-against-disk, scene load), `create_project` /
`create_auto_empty_project`, `create_project_script`, and the project path/name helpers (`appDataRoot`,
`projectUserdataRoot`, `validProjectName`, `defaultDisplayName`, `ProjectInfo`, `ProjectVersion`).

## Why this shape (NO LEGACY)

`ProjectVersion = 1`; a version mismatch is a typed `Error::BadProjectVersion`, not a silent best-effort
load. The `editorCamera` and `debugOverlays` blocks belong to `saffron-sceneedit`, not here, so they
ride through `save_project`/`load_project` as opaque `serde_json::Value` round-tripped to the caller (the
host) — this crate never owns or interprets them. The load order is load-bearing and survives exactly:
parse → version-gate → `wait_gpu_idle(renderer)` → stop/clear the thumbnail worker → `clear_asset_caches`
→ set the asset root → ensure the script `src/` + library → load the catalog from the doc → reconcile
against disk (the filesystem is the source of truth; a cold scan when the cache misses) → sweep orphan
thumbnail cache files → apply render settings → pull camera/overlays → `scene_from_json`. The
idle-before-clear is the UAF guard (§3 of the README): the GPU must be idle before the caches' `Arc`s
drop. Save writes via the json crate's pretty dump; the doc's key/object order matches the C++
insertion order for byte-compat.

## Grounding (real files/symbols)

- `engine-old/source/saffron/assets/assets.cppm`: `saveProject` (the doc bundle, the optional
  `editorCamera`/`debugOverlays`), `loadProject` (the exact ordered sequence incl. `waitGpuIdle` →
  `clearAssetCaches` → `setAssetRoot` → `ensureScriptSrc`/`ensureScriptLibrary` → `catalogFromJson` →
  `loadCatalog` reconcile → `sweepThumbnailCacheOrphans` → `applyRenderSettings` → `sceneFromJson`),
  `createProject`, `createAutoEmptyProject`, `createProjectScript`, `ProjectVersion`, `ProjectInfo`,
  `appDataRoot`, `projectUserdataRoot`, `validProjectName`, `defaultDisplayName`, `StarterScript`,
  `projectJsonPath`, `projectInfoFromPath`.
- Upstream: scene `scene_to_json`/`scene_from_json` + `catalog_to_json`/`catalog_from_json`; rendering
  `render_settings_to_json`/`apply_render_settings`; the `wait_gpu_idle` seam (host/rendering).
- The AGENTS rule: "Clear caches only after `waitGpuIdle`… `project.json` is version-gated… bundles
  assets + folders + scene + render settings + optional `editorCamera` and `debugOverlays`, which are
  round-tripped back to the caller."

## Acceptance gate

- `cargo build -p saffron-assets` + workspace green; clippy + fmt clean.
- A round-trip `#[test]`: `save_project` then `load_project` reproduces the catalog, folders, scene, and
  render settings; a byte-equality test of the saved doc against a captured C++ `project.json` (key order
  + decimal-string ids); a `version != 1` doc returns `Error::BadProjectVersion`.
- An order `#[test]` (with a recording stub renderer/worker): `load_project` calls `wait_gpu_idle` →
  `clear_thumbnail_queue`/`stop` → `clear_asset_caches` **before** swapping the catalog (assert the
  recorded call sequence); the opaque `editorCamera`/`debugOverlays` blocks round-trip unchanged to the
  caller.
- `valid_project_name` / `default_display_name` `#[test]`s reproduce the C++ rules (lowercase/digit/`-`,
  length cap, the capitalize-on-`-` display name); `create_auto_empty_project` produces a loadable
  minimal project.
