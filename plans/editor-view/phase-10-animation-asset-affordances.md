# Phase 10 — animation-asset affordances

**Status:** COMPLETED (kept as a build record; superseded by the general Asset editor — see this dir's README)
**Depends on:** 5 (the workspace + open routing)

## Goal

Make rig/animation assets feel first-class everywhere they appear: real icons instead of the `File`
fallback, a duration badge on clip tiles, sensible double-click routing, and clean empty/error
states — the polish that turns the feature from "reachable" into "discoverable". (UE5's asset
browser color-codes and tooltips animation assets; we do the lightweight equivalent.)

## What exists to build on

- Both icon fallthroughs are explicit: the tile `TypeIcon` (`AssetTile.tsx:92-101` — mesh→Box,
  texture→ImageIcon, else File) and the tab `tabIcon` (`WindowTitlebar.tsx:425-444` — same shape);
  `"animation"` exists in the type union (`se-types.ts:978-984`) but no surface branches on it.
- Clip duration is already on the catalog row (`assets.cppm:298-303`) and the wire via `list-clips`
  (`AnimationClipDto`), and on `get-rig` clip entries — the tile needs it on `AssetEntryDto` or
  fetched lazily; the DTO addition is one optional field + regen (precedent says keep the DTO lean,
  but duration is display data the grid genuinely needs — add it as optional, omitted for
  non-animation rows).
- Thumbnails: `Animation` assets have none (`control_commands_asset.cpp:401-403`); the tile shows
  the icon. A posed-rig thumbnail (render the linked rig mid-clip) is possible after phase 2 but is
  one-shot PNG work with real cost — explicitly deferred; the icon + duration badge is v1.
- Routing today (phase 5): animation double-click → rig editor; mesh double-click → image viewer,
  context-menu "Open in Rig editor".
- Error states: `get-rig`'s stable not-a-rig error (phase 5) — the asset has no skin in its
  `.smodel` container.

## Work

### 1. Icons + badges

- `TypeIcon` + `tabIcon`: `animation` → a film/clapper lucide icon (match the Topbar timeline
  trigger's `Clapperboard` for vocabulary consistency); rigged mesh assets keep `Box` (no async
  rig probing in the icon path).
- Clip tiles: a bottom-right duration badge (`Badge` ui primitive, `formatTime` from the timeline)
  when duration is present.

### 2. Routing completeness

- Mesh assets that **have** a rig: make double-click open the rig editor instead of the image
  viewer. "Has a rig" must be synchronously known to the click handler — expose the
  `rigged` flag (derived from the container's MetadataChunk at scan — the `.smodel` prefix read knows
  whether the model has a skin — so it survives project reload) on `AssetEntryDto` as an optional
  field, read straight from the grid's `AssetEntry`. Unrigged meshes keep the image viewer. Context menus offer both
  ("View image" / "Open in Rig editor") for rigged meshes.
- A clip whose owning model has no rig (`get-rig { asset: clipId }` errors): double-click opens the
  rig editor's not-a-rig error state — not a dead toast. (Phase 5 already routes clip-open through
  `get-rig`; this phase just ensures the error surface is the workspace, not a swallowed rejection.)

### 3. State polish

- Workspace empty/error states reviewed as a set: not-a-rig (re-import hint), asset deleted while
  tab open (the `setAssetList` re-title path leaves a stale tab — close it like the delete flow
  does, `closeViewTab(\`asset:${asset.id}\`)` in `confirmDeleteAssets`, `AssetsPanel.tsx:614`),
  engine restart mid-preview (the watchdog flow lands back on the scene tab; re-entering re-spawns).
- Tooltips only where meaning is non-obvious (`editor/AGENTS.md`): the duration badge and type
  icons need none; the not-a-rig error state's hint explains the asset has no rig in its `.smodel`
  container.

## Validation (done criteria)

- `bun run check` + `bun run lint` clean; `make engine` + contract test clean (the DTO fields).
- Manual (`make run`): the Assets grid shows clip icons + durations; double-click matrix — rigged
  mesh → rig editor, unrigged mesh → image viewer, clip → rig editor with that clip active,
  no-rig clip → not-a-rig error state.
- `make e2e`: `list-assets` rows carry `rigged`/`duration` where expected.
- `docs/`: the rig-editor page's "opening" section updated with the final routing.

## Notes / gotchas

- `rigged` is **derived from the container** (the `.smodel` MetadataChunk has a `skin`), populated
  into the catalog by the scan (saffron-models phase 9), so `list-assets` reads it straight off the
  in-memory catalog — no per-list stat — and it survives project reload because the scan rebuilds it
  from the file. This phase only adds the wire DTO field + the routing that reads it.
- Surfacing `rigged`/`duration` on `AssetEntryDto` means the contract schemas regenerate — remember
  `additionalProperties: false` makes a forgotten regen a hard contract failure, which is the point.
- Posed-rig thumbnails for clips stay deferred; if/when added they reuse `enter-rig-preview` +
  the capture path offline, not a new render pipeline.
