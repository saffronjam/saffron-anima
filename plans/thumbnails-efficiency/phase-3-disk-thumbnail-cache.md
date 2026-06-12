# Phase 3 — disk thumbnail cache

**Status:** COMPLETED

Nothing survives a restart today: the engine regenerates every thumbnail from scratch on
each `get-thumbnail`, and the editor's in-memory blob-URL cache
(`editor/src/state/store.ts:1472-1544`) is cleared on every project load
(`invalidateThumbnails`, `store.ts:1165`). The engine is the right owner for a persistent
cache — it knows the asset files and when they change; the editor keeps its session cache
as-is on top.

## The work

- Cache location: `<projectRoot>/cache/thumbnails/` next to the project's `assets/`
  (project roots live under `projectUserdataRoot()`, `assets.cppm:144`). One PNG per
  entry. Make sure project save/load and the catalog scan ignore the `cache/` dir.
- Key: asset uuid + requested size + a stamp of the source file
  (`last_write_time` + file size, the same stat fields `probe-asset` already reads).
  Encode size and stamp in the filename — e.g. `<uuid>-<size>-<stamphash>.png` — so a hit
  is a single `std::filesystem::exists` after a stat of the source, and a stale entry is
  simply never matched again. Include a cache format version in the stamp hash so a
  generation-affecting change (phase 1/2 behaviour, future format tweaks) invalidates
  wholesale.
- In `thumbnailResult` (`control_commands_asset.cpp:340`): on hit, read the PNG bytes and
  base64 them into the reply — no decode, no GPU work, no PNG encode. On miss, generate as
  today and write the PNG before replying (best-effort: a failed write logs and still
  replies).
- What each type stamps against:
  - Texture: the imported source file under `assets/textures/`.
  - Mesh: the `.smesh` file.
  - Material: the `.smat` file *for now* — correct for direct edits, incomplete for
    instances whose parent changed (parent edits reflow instances without touching the
    child file, `applyOverrides` semantics in `assets.cppm`). Phase 4 closes that hole;
    until then a material cache entry may be stale after a parent edit.
- The PNG width/height from a cache hit must still be reported truthfully (read them from
  the PNG header or store them in the filename stamp).

## Verification

- e2e: `get-thumbnail` twice for the same asset across an engine restart (the harness
  already boots/kills engines); assert the cache file exists after the first call and that
  the second engine serves an identical PNG. Touch the source file (bump mtime) and assert
  regeneration.
- Manual: second editor start of "My project" shows all tiles near-instantly; no
  multi-second HDR spike.
- Milestone gate: `make engine` + `make prepare-for-commit`.
- Docs: describe the cache (location, key, lifetime) in
  `docs/content/explanations/ui-and-editor/assets-panel-and-thumbnails.md`.
