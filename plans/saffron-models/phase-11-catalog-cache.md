# Phase 11 — Catalog cache (regenerable)

**Status:** COMPLETED
**Depends on:** 10

> Implementation note: the cache is keyed by a single `assetSignature` — an FNV fold over sorted
> (relpath, mtime, size) for every file under assets/ (sidecars included, `.cache/` excluded, stat-only).
> `loadCatalog` reuses the cached rows only when the recomputed signature matches; any add/remove/touch/
> sidecar-edit (or a missing/corrupt cache) falls through to a cold `scanAssets` + rewrite. Coarser than
> the plan's per-row mtime check (one change re-scans all), but dead simple and provably non-load-bearing.
> `loadProject` calls `loadCatalog`; `saveProject` still writes the assets array; `.cache/` is already
> gitignored. Verified by `tests/e2e/catalog_cache.test.ts` (delete → identical cold-scan catalog;
> corrupt → clean fallback).

## Goal

Persist a regenerable `assets/.cache/catalog.json` keyed by **project-relative path → (mtime, sourceHash,
subIds)** — the UE `AssetRegistry.bin` / Unity `Library` / Godot `.godot/imported` analog. On load, trust a
cache row when the file's mtime is unchanged (and confirm with the content hash on suspicion); re-prefix-read
only drifted files; rebuild from a cold scan when the cache is missing or stale. **Deleting the cache is
always safe.** Per the README decision this ships minimal but is **never load-bearing**: the catalog is
correct from a cold scan regardless. Defers: nothing downstream depends on the cache.

## Why

The scan (phase 09) is cheap at fresh-project scale, but opening every `.smodel` to prefix-read it on each
load is O(files) I/O. A cache makes cold-start O(read one file) in the common case, matching how every
engine keeps a derived index — while preserving the "filesystem is truth" invariant, because the cache is
strictly a memoization that is discarded on any doubt.

## The cache format

```jsonc
// assets/.cache/catalog.json  (gitignored; safe to delete)
{
  "version": 1,
  "entries": [
    { "path": "models/<uuid>.smodel", "mtime": 1734000000, "size": 1048576,
      "sourceHash": "<xxh3>", "rows": [ /* the AssetEntry rows this file contributed, incl. sub-assets */ ] },
    { "path": "textures/<uuid>.png", "mtime": 1733999000, "size": 524288, "rows": [ /* 1 row */ ] }
  ]
}
```

Change detection (Unity/Godot/UE consensus): **mtime + size as a cheap pre-filter, content hash to
confirm**. Never timestamp alone.

## Load path

```cpp
std::expected<void, std::string> loadCatalog(AssetServer& assets) {
  // 1. read assets/.cache/catalog.json if present
  // 2. for each on-disk file: if a cache row matches (path + mtime + size) → reuse its rows;
  //    else prefix-read/scan the file (phase 09) and replace the row
  // 3. drop cache rows whose file is gone; add rows for new files
  // 4. rebuild byId; write the refreshed cache back
  // Missing/corrupt cache ⇒ fall through to a full scanAssets() and write a fresh cache.
}
```

`loadProject` (phase 09) calls `loadCatalog` instead of a bare `scanAssets` — but `loadCatalog` is a thin
wrapper that degrades to `scanAssets` on any mismatch, so correctness never depends on the cache being
present or valid.

## Files to touch

- `engine/source/saffron/assets/assets.cppm` — `loadCatalog`/`writeCatalogCache`; `ensureAssetDirectories`
  creates `assets/.cache/`; `loadProject` calls `loadCatalog`.
- `.gitignore` — ignore `assets/.cache/` (or the project template's ignore).

## Steps

1. Define the cache JSON + read/write.
2. Implement `loadCatalog`: cache-hit reuse by (path, mtime, size), hash-confirm on suspicion, scan-miss
   fallback, drop-missing, add-new, rebuild `byId`, rewrite cache.
3. Repoint `loadProject` to `loadCatalog`.
4. e2e: import a model → load (builds cache) → **delete the cache** → load again → assert an **identical**
   catalog (parity with deleting `AssetRegistry.bin`); touch a file's mtime → assert only that file is
   re-read; corrupt the cache JSON → assert a clean full-scan fallback.

## Gate / done

- `make engine` clean; the delete-cache e2e proves identical catalog from a cold scan; `make e2e` + contract
  test pass; `make prepare-for-commit` clean.
- The cache is never required for correctness (prove by the delete-cache test).

## Risks

- **Cache becoming load-bearing:** the failure mode is a subtle dependency where something only works with a
  warm cache. The delete-cache e2e is the guard — keep it.
- **Stale-cache correctness:** mtime can lie (copies, restores). The hash-confirm step must trigger on any
  size/mtime oddity, or a stale row serves wrong data. When in doubt, re-scan the file.
- **Cache write churn / determinism:** writing the cache on every load is fine, but the serialized rows must
  be stable (sorted) so VCS/diffs and the contract surface don't churn — and the cache must be gitignored.
- **Concurrency:** two engine instances sharing a project could race on the cache file; treat a write
  conflict as "discard and rescan," never as corruption.
