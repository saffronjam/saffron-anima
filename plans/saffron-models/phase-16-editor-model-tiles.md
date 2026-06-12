# Phase 16 — Editor: model tiles, instantiate-on-drag, extract, clean modal

**Status:** COMPLETED
**Depends on:** 15

> Implementation note. **Verified (automated gate — `bun run check` + `bun run lint` + `make engine` all
> green):** the engine DTO now carries `AssetEntryDto.container` + real `Model`/`Material`
> `AssetTypeDto` values (so the editor can tell a model from its sub-assets); the typed control client
> (`client.ts`) gained wrappers for the whole decoupled flow — `importModelToAsset`,
> `instantiateModel`, `extractSubAsset`/`clearExtraction`, `scanAssets`, `reimportModel`, `modelInfo`,
> `assetReferences`, `cleanAssets`, `deleteUnused`; the store (`store.ts`) gained the matching actions;
> and the Assets grid now **hides embedded sub-assets** (`asset.container` set), so a Sponza-style
> import shows as **one model tile** instead of ~50 rows — the user's headline pain. **Needs an
> interactive Wayland `tauri dev` session to verify (the one gate step not runnable headlessly):** the
> drag-model→viewport instantiate drop target, the expand-to-show-sub-assets rows, the Extract
> context-menu item, and the clean-review modal — the commands are all wired and typed; their on-screen
> interaction wiring is the remaining hand-verification.

## Goal

Surface the whole flow in the Tauri/React editor: render each `.smodel` as **one hierarchical tile** that
expands to its sub-assets (read from the catalog's container linkage), make **dragging a model into the
scene** call `instantiate-model`, add a context-menu **Extract** on sub-assets (`extract-subasset`), and a
**clean-unused review modal** over `clean-assets` / `delete-unused`. Store actions + typed client
passthrough. Defers: baked thumbnails (17), docs (18).

## Why

This is where the user's two original pains are actually fixed in the UI: the Assets panel shows **one
item** per Sponza import (not ~50), and creating the entity is a **drag**, the same every time, decoupled
from import. Extract and clean give the on-demand editing + housekeeping the engine now supports.

## Editor changes

- **Hierarchical tiles** (`AssetsPanel.tsx` / `AssetTile.tsx`): a `type==="model"` row is expandable; its
  children are the catalog rows whose `container === model.id`, grouped by sub-asset type. Sub-asset rows
  are not shown at the top level (they're inside their model), collapsing the flood to one item.
- **Drag-to-instantiate:** dropping a model tile onto the viewport/hierarchy calls
  `client.instantiateModel(modelId, name)` (phase 08), not the old import-spawns path. Reuse the existing
  `dragActive` flow in `store.ts`.
- **Context menu → Extract:** on a sub-asset row, `client.extractSubAsset(modelId, subId)` (phase 12) →
  refresh; show extracted sub-assets with an "extracted/overridden" badge.
- **Clean review modal:** a panel that calls `client.cleanAssets({ dryRun: true })` (phase 15), shows
  candidates grouped by category (Unused / Orphaned / Broken / Review) with per-item exclude checkboxes and
  byte sizes, and a confirm button that calls `client.deleteUnused({ confirm: true, ids })` then refreshes.
  Review/Broken items are non-deletable (display-only), matching the engine contract.
- **Reference/info affordance:** a sub-asset/model tile can request `model-info` / `asset-references`
  (phase 14) to show "used by N entities" / footprint (a lightweight inspector popover).

## Files to touch

- `editor/src/panels/AssetsPanel.tsx` — hierarchical listing, drag-to-instantiate, context menu, clean
  modal entry point.
- `editor/src/components/AssetTile.tsx` — expandable model tile + sub-asset rows + extracted badge.
- `editor/src/state/store.ts` — actions: `instantiateModel`, `extractSubAsset`, `cleanAssets`,
  `deleteUnused`, `scanAssets`; group catalog rows by `container`.
- `editor/src/lib` / the typed control client — passthroughs for the new commands (types come from the
  regenerated `@saffron/protocol`).

## Steps

1. Group catalog rows by `container` in the store; expose a `models` selector (parents + their children).
2. Render hierarchical tiles; hide sub-assets at the top level; add the expand/collapse.
3. Wire drag-to-instantiate to `instantiate-model`; remove the old import-spawns assumption from the UI.
4. Add the Extract context action + the extracted badge.
5. Build the clean review modal over `clean-assets` / `delete-unused` with category grouping + confirm.
6. `bun run check` (regenerate/typecheck `@saffron/protocol`) + `bun run lint`; click-through in `tauri dev`.

## Gate / done

- `bun run check` + `bun run lint` clean; `make engine` clean (no engine change expected, but gate it);
  manual verify in `tauri dev`: a Sponza import shows as one expandable tile, drag spawns an instance,
  Extract pulls a material out, the clean modal lists + deletes an unused model on confirm.
- `make prepare-for-commit` clean.

## Risks

- **Wayland/subsurface requirement:** `tauri dev` needs a Wayland session for the viewport presenter; the
  drag-to-viewport drop target must hit the right surface. Verify the drop coordinates map to the scene.
- **Stale catalog after mutations:** extract/instantiate/delete change the catalog; the store must refresh
  (via the command's returned delta or a `scan-assets`) or the tree shows stale rows.
- **Protocol drift:** the editor types come from `@saffron/protocol`; if `bun run check` isn't run after
  the engine-side `gen.ts` changes (phases 08/12/15), the client calls won't typecheck. Run it.
- **Destructive UX:** the clean modal is the user-facing delete path; keep dry-run default, require an
  explicit confirm, and disable delete for Broken/Review items.
