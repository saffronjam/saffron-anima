# Phase 17 — Thumbnails baked at import + async per-chunk

**Status:** NOT STARTED
**Depends on:** 16

## Goal

Bake a container thumbnail (and per-sub-asset previews) at **import time** into the existing thumbnail cache
rather than on first view, and render per-chunk previews **async, off the frame loop**. Thumbnail keying
shifts to the embedded **sub-id** (`(modelId, subId)`). e2e/contract for the thumbnail commands. This
composes with `plans/thumbnails-efficiency` phases 5–7 (the async path). Defers: nothing downstream.

## Why

Once the browser shows one expandable model tile (phase 16), the model and its sub-assets need previews so
the single item is recognizable and its contents browsable. Generating them lazily on first view, with the
current synchronous `device.waitIdle()` in `renderer_thumbnail.cpp`, would hitch badly for a multi-material
model (one stall per preview). Baking at import + async generation removes the cold-browse stall.

## What changes

- **Optionally embed a `THMB` chunk** in the `.smodel` at bake (phase 05) — a small model preview — so the
  scan can show a thumbnail from the prefix read without rendering. (Decision: embed the model-level
  thumbnail; render per-sub-asset previews into the cache lazily/async.)
- **Bake at import:** after `bakeModel`, enqueue thumbnail renders for the model + each sub-asset into the
  existing cache (`cache/thumbnails/`), keyed by `(modelId, subId)`.
- **Async rendering:** move thumbnail generation off the synchronous `device.waitIdle()` path onto the
  async mechanism from `thumbnails-efficiency` 5–7 (a worker + a frames-in-flight fence), so a cold import
  of a large `.glb` doesn't stall the frame loop. If 5–7 hasn't landed, gate the per-chunk baking behind a
  budget (N per frame) to bound the stall, and note the dependency.
- **Keying:** the thumbnail command + cache key move from a flat asset uuid to `(modelId, subId)` (a
  standalone asset is `(0, id)`), so an embedded material's preview is addressable.

## Files to touch

- `engine/source/saffron/rendering/renderer_thumbnail.cpp` — async render path (or budgeted), key by
  `(modelId, subId)`; reuse `renderMeshThumbnail` / the material preview pass from material-uplift.
- `engine/source/saffron/control/control_commands_asset.cpp` — the thumbnail request command accepts a
  `(modelId, subId)` key; bake-at-import enqueue.
- `engine/source/saffron/assets/assets.cppm` — optional `THMB` chunk at bake; enqueue thumbnails post-bake.
- `editor/src/state/store.ts` — thumbnail fetch keyed by `(modelId, subId)`.

## Steps

1. Add the `(modelId, subId)` key to the thumbnail command + cache; keep a `(0, id)` form for standalone.
2. Optionally write a `THMB` chunk at bake (phase 05 hook) for the model-level preview.
3. Enqueue model + sub-asset thumbnail renders after import; render async (or budgeted if 5–7 isn't in).
4. Editor: fetch sub-asset thumbnails by the new key (phase 16 tiles).
5. e2e/contract: import a model → thumbnails for the model + each material exist in the cache without a
   frame stall (assert via the async path or the budget); the thumbnail command round-trips a `(modelId,
   subId)` key.

## Gate / done

- `make engine` clean; the thumbnail e2e/contract passes (keyed previews, baked at import, no synchronous
  stall on the async path); `make e2e` + contract test pass; `bun run check`/`lint` if the editor key
  changed; `make prepare-for-commit` clean.

## Risks

- **Synchronous stalls if the async path isn't ready:** `renderer_thumbnail.cpp` does `device.waitIdle()`
  today; baking many previews at import multiplies stalls. Either land on `thumbnails-efficiency` 5–7 first
  or budget the per-frame work; do not ship a cold import that hitches for seconds.
- **Cache key migration:** existing thumbnails keyed by a flat uuid must coexist or be invalidated; define
  the `(0, id)` standalone form so nothing double-renders.
- **`THMB` chunk size:** keep the embedded model preview small (the prefix read pays for it); per-sub-asset
  previews stay in the external cache, not the container.
- **Coordination with thumbnails-efficiency:** that plan owns the async/invalidation machinery; this phase
  consumes it. Keep the key change compatible with its cache layout.
