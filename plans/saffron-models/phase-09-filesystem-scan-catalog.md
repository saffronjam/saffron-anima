# Phase 09 — Filesystem scan + catalog rebuild

**Status:** NOT STARTED
**Depends on:** 08

## Goal

Implement `scanAssets(root)`: walk `assets/`, prefix-read every `.smodel` into a parent + sub-asset catalog
rows, index standalone files (identity from their header or a `.smeta` sidecar), diff against the in-memory
catalog, and patch GPU caches after `waitGpuIdle`. Wire `loadProject` (`assets.cppm:933`) to build the
catalog **from a scan** instead of `project.json`'s `assets` array; `project.json` keeps only `assetFolders`
+ UI state. Add a `scan-assets` control command + e2e. Defers: the `.smeta` schema details (10), the
regenerable cache (11).

## Why

This makes the **filesystem the source of truth** and **eliminates the orphan-on-restart class outright**:
a freshly imported `.smodel` is rediscovered on next load whether or not the project was saved, because the
file itself is the record (the UE Asset Registry / Unity AssetDatabase model). It is the structural fix for
the user's "all my Sponza textures are dead orphans because I didn't save `project.json`."

## `scanAssets`

```cpp
struct ScanDelta { std::vector<AssetEntry> added, changed; std::vector<Uuid> removed; };

// Walk root/assets, rebuild catalog rows from disk, diff vs the live catalog, return the delta.
// .smodel → readContainerMetadata (prefix read) → 1 Model row + N sub-asset rows (container/chunk set).
// standalone .smesh/.smat/<image>/.sanim → 1 row (identity from header, else .smeta sidecar — phase 10).
std::expected<ScanDelta, std::string> scanAssets(AssetServer& assets);
```

Algorithm:
1. Recursively enumerate `assets/` (skip `assets/.cache/`).
2. For each `.smodel`: `readContainerMetadata` (phase 02) → emit the parent `Model` row + one row per
   `subAssets[i]` (`container=modelId`, `chunk=i`, `colorspace` from the sub-asset / chunk flags).
3. For each standalone recognized file: emit a row; identity = the file's embedded id (`.smesh` has no id
   header today, so standalone meshes need a `.smeta` — phase 10) else mint+write a `.smeta`.
4. Derive `folder` from the on-disk directory path under `assets/` (the directory layout *is* the browser
   folder); fold in any UI-only custom names from `project.json`.
5. Diff against the live `catalog.entries` by id → `ScanDelta`. Rebuild `byId`.
6. For `removed`/`changed` ids that have live GPU resources, `waitGpuIdle` then drop/refresh the
   `meshRefByUuid`/`textureRefByUuid`/`materialRefByUuid`/`modelRefByUuid` entries (phase 06).

## `loadProject` change

`loadProject` (`assets.cppm:933`) today does `catalogFromJson(doc["assets"])` before loading the scene.
New order: **`scanAssets` → build catalog + `byId` → then load the scene** (so entity `(modelId, subId)`
refs resolve against the freshly scanned catalog). `saveProject` (`assets.cppm:765`) stops writing the
`assets` array (keeps `assetFolders` + UI state). No `ProjectVersion` bump.

## `scan-assets` command

`scan-assets` → `scanAssets` → returns the `ScanDelta` (counts + the affected rows) so the editor can
refresh without a restart (Unity `AssetDatabase.Refresh`). DTO + `gen.ts` + fixtures + `se` verb.

## Files to touch

- `engine/source/saffron/assets/assets.cppm` — `scanAssets`, `ScanDelta`; change `loadProject`/`saveProject`
  catalog source; GPU-cache patching after `waitGpuIdle`.
- `engine/source/saffron/control/control_commands_asset.cpp` + `control_dto.cppm` + `gen.ts` — the
  `scan-assets` command + DTO + regen.
- `tests/e2e` — a scan/orphan round-trip.

## Steps

1. Implement the recursive walk + per-extension row builder (containers via prefix read; standalone via
   header/`.smeta` stub — full `.smeta` is phase 10).
2. Derive `folder` from directory path; reconcile with `project.json` UI state.
3. Implement the diff + `byId` rebuild + GPU-cache patch (guard with `waitGpuIdle`).
4. Repoint `loadProject` to scan-first; stop `saveProject` writing `assets`.
5. Add the `scan-assets` command (+ DTO + `gen.ts` + fixtures).
6. e2e: import a model (writes a `.smodel`), **delete `project.json`** (simulate "never saved"), reload →
   `scanAssets` reconstructs the identical catalog (the orphan test); then add a file on disk and
   `scan-assets` → it appears; remove it → it's dropped. Validation-clean log.

## Gate / done

- `make engine` clean; the orphan e2e proves a never-saved import survives a reload via scan; `scan-assets`
  reachable; `make e2e` + contract test pass; `make prepare-for-commit` clean.
- No `ProjectVersion` bump.

## Risks

- **GPU-cache invalidation on live resources:** dropping a `meshRefByUuid` entry while a frame is in flight
  is a use-after-free. Gate every cache mutation behind `waitGpuIdle` (the teardown discipline) or a
  frames-in-flight deferred free.
- **Scan cost / blocking:** a synchronous full scan on every load is fine at fresh-project scale (prefix
  reads are cheap), but log the count; phase 11's cache and a future async scan address larger trees.
- **Standalone `.smesh` has no id header:** until phase 10's `.smeta`, a loose `.smesh` can't self-identify;
  scope this phase to containers + already-`.smeta`'d files, and let 10 complete foreign-file identity.
- **Folder derivation vs existing logical folders:** today `folder` is logical and disk is flat. Moving to
  directory-derived folders changes where imports land; ensure the importer writes into the intended
  subdirectory so the scan derives the right folder.
