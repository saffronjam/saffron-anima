+++
title = 'Assets panel & thumbnails'
weight = 8
+++

# Assets panel & thumbnails

The Assets panel is a responsive tile grid over the project's [asset catalog](../../scene-and-ecs/asset-catalog-in-scene/). Each tile shows a thumbnail and an editable name, and is a drag source for the inspector's [pickers](../asset-pickers-and-drag-drop/). It is a React panel reading `store.assets`; the thumbnails are PNGs fetched over the control socket and cached as blob URLs.

## The tile grid

`AssetsPanel` lays the catalog out with a CSS grid (`repeat(auto-fill, minmax(72px, 1fr))`) so tiles reflow to the panel width. Each `AssetTile` is a 72px tile: a square thumbnail on top, an in-place name button beneath. When the catalog is empty the panel shows an import/drag-and-drop prompt instead. The list comes from the reconcile poll's `list-assets` refresh (eagerly re-fetched after an import or rename).

## Thumbnails over the socket

There is no shared GPU context between the engine and the webview, so a thumbnail can't be a registered descriptor — it travels as data. `get-thumbnail` renders the asset to a small offscreen and reads it back as a **base64 PNG** in the JSON result; the client decodes it to a `Blob` and an object URL:

- a **texture** asset renders its own decoded image;
- a **mesh** asset renders a [3D preview](../mesh-thumbnails/);
- anything else (or a failed render) falls back to a Lucide type icon in the webview.

A `get-thumbnail` call is a GPU→CPU readback plus a PNG encode, so it must not run per frame or per tile. A module-scope cache keyed by asset id holds `{ blob URL, the px size it was fetched at }`, and concurrent requests for the same asset share one in-flight promise. A cached URL is reused whenever it is at least as large as the requested size, so a 128px grid tile and a 16px combo swatch both ride the same fetch:

```ts
export async function getThumbnailUrl(assetId: string, size: number): Promise<string> {
  const cached = thumbnailCache.get(assetId);
  if (cached && cached.size >= size) return cached.url;     // hit
  const inflight = thumbnailInflight.get(assetId);
  if (inflight) return inflight;                            // dedupe
  // miss → get-thumbnail, decode base64 PNG, store the object URL
}
```

When the catalog changes under us — a project or scene load — every cached blob URL is stale, so `invalidateThumbnails` revokes them all and the lazy cache re-fetches on demand.

## Rename in place

Double-clicking a tile's name turns it into an input bound to a draft string; Enter (or blur) commits with `rename-asset`, Escape cancels. The engine returns the new `{id, name}` and the catalog refresh reflects it. Names are UTF-8, so non-Latin names round-trip through the project file (rendering them needs a broader font — a known follow-up).

```ts
void client.renameAsset(entry.id, next).then(() => refreshAssets());
```

## Import and drag-drop

The **Import** button opens the Tauri file dialog (`tauri-plugin-dialog`); the panel is also an **OS file-drop** target via the webview drag-drop event. Both route by extension — images go to `import-texture` (catalog-only, no spawn), everything else to `import-model` — matching the engine's own `importToCatalog`. The OS file-drop is hit-tested against the panel's rect (scaled by `devicePixelRatio`, since the drop position is in physical pixels) so dropping a model on the *viewport* doesn't trigger a catalog import here.

Each tile is also an HTML5 drag *source* (a distinct channel — `application/x-se-asset` — from the OS file drop), carrying `{id, type}` so an inspector [picker](../asset-pickers-and-drag-drop/) can type-gate the drop.

## The View modal

Double-clicking a tile opens a 512px preview in a shadcn `Dialog` (`view-asset`, the same readback path as `get-thumbnail`). The modal hits the core viewport-bridge constraint: the reparented native window always paints on top of its rect, so a dialog centered over the viewport would be hidden behind it. While the viewer is open the panel sets `store.viewportHidden`, which the [viewport panel](../viewport-panel/) reads to park the native window off-screen; closing the modal clears the flag and the native window re-glues to its div. The preview PNG renders entirely in the webview, so it is never occluded.

## In the code

| What | File | Symbols |
|---|---|---|
| Tile grid + import + drop | `editor/src/panels/AssetsPanel.tsx` | `AssetsPanel`, `importPath`, `isInsidePanel` |
| Tile + rename + drag source | `editor/src/components/AssetTile.tsx` | `AssetTile`, `RenameInput`, `ASSET_DND_MIME` |
| Thumbnail blob-URL cache | `editor/src/state/store.ts` | `getThumbnailUrl`, `invalidateThumbnails`, `thumbnailCache` |
| The View modal + park | `editor/src/components/AssetViewer.tsx` | `AssetViewer`, `viewportHidden` |
| Readback (engine) | `control_commands_asset.cpp` | `get-thumbnail`, `view-asset`, `list-assets`, `rename-asset` |

## Related

- [Mesh thumbnails](../mesh-thumbnails/) — the 3D preview render behind a mesh tile
- [Asset pickers](../asset-pickers-and-drag-drop/) — the drop targets these tiles feed
- [Asset catalog in the scene](../../scene-and-ecs/asset-catalog-in-scene/) — the catalog this grid views
- [Asset commands](../../tooling-and-control/asset-commands/) — import/list/rename + the thumbnail readback
