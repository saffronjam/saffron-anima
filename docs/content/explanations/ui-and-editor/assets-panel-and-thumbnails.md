+++
title = 'Assets panel & thumbnails'
weight = 8
+++

# Assets panel & thumbnails

The Assets panel is a tile grid over the project's [asset catalog](../../scene-and-ecs/asset-catalog-in-scene/). Each tile shows a thumbnail and an editable name, and acts as a drag source for the inspector's [pickers](../asset-pickers-and-drag-drop/). Virtual folders group catalog entries without changing the imported files on disk.

Thumbnails are PNGs fetched over the control socket and cached as blob URLs, because the engine and the webview share no GPU context. The panel itself is a React component reading `store.assets`.

## The tile grid

`AssetsPanel` lays the current folder out with a CSS grid (`repeat(auto-fill, minmax(72px, 1fr))`), so tiles reflow to the panel width. Each `AssetTile` is a 72px tile: a square thumbnail on top, an in-place name button beneath. The root view also shows folder tiles. An empty catalog shows an import and drag-and-drop prompt instead. The list comes from the reconcile poll's `list-assets` refresh, re-fetched eagerly after imports, moves, renames, or deletes.

Right-clicking the asset background opens commands for **New Folder** and **Import**. Right-clicking a folder opens **Rename** and **Delete**; Delete removes the virtual folder and moves its assets back to Root. Right-clicking an asset opens **View** and **Delete**; Delete asks the engine for `asset-usages` first, shows an in-place confirmation with affected slots, then calls `delete-asset`.

## Thumbnails over the socket

A thumbnail travels as data rather than a registered descriptor, since no GPU context is shared. `get-thumbnail` renders the asset to a small offscreen and reads it back as a **base64 PNG** in the JSON result; the client decodes it to a `Blob` and an object URL:

- a **texture** asset renders its own decoded image;
- a **mesh** asset renders a [3D preview](../mesh-thumbnails/);
- anything else, or a failed render, falls back to a Lucide type icon in the webview.

Each `get-thumbnail` call is a GPU→CPU readback plus a PNG encode, so it must not run per frame or per tile. A module-scope frontend cache keyed by asset id holds `{ blob URL, the px size it was fetched at }`, and concurrent requests for the same asset share one in-flight promise. A cached URL is reused synchronously whenever it is at least as large as the requested size, so folder navigation does not ask the backend for thumbnails it already decoded in the current webview session:

```ts
export async function getThumbnailUrl(assetId: string, size: number): Promise<string> {
  const cached = thumbnailCache.get(assetId);
  if (cached && cached.size >= size) return cached.url;     // hit
  const inflight = thumbnailInflight.get(assetId);
  if (inflight) return inflight;                            // dedupe
  // miss → get-thumbnail, decode base64 PNG, store the object URL
}
```

When the catalog changes, on a project or scene load, every cached blob URL is stale. `invalidateThumbnails` revokes them all, and the lazy cache re-fetches on demand.

## Rename in place

Double-clicking a tile's name turns it into an input bound to a draft string. Enter or blur commits with `rename-asset`; Escape cancels. The engine returns the new `{id, name}`, and the catalog refresh reflects it. Names are UTF-8, so non-Latin names round-trip through the project file; rendering them needs a broader font, a known follow-up.

```ts
void client.renameAsset(entry.id, next).then(() => refreshAssets());
```

## Import and drag-drop

The **Import** button opens the Tauri file dialog (`tauri-plugin-dialog`); the panel is also an **OS file-drop** target via the webview drag-drop event. Both route by extension: images go to `import-texture` (catalog-only, no spawn), everything else to `import-model`, matching the engine's `importToCatalog`. The OS file-drop is hit-tested against the panel's rect, scaled by `devicePixelRatio` because the drop position is in physical pixels. Dropping a model on the *viewport* therefore does not trigger a catalog import here.

Each tile is also an HTML5 drag *source* on a distinct channel, `application/x-se-asset`, separate from the OS file drop. It carries `{id, type}` so an inspector [picker](../asset-pickers-and-drag-drop/) can type-gate the drop. Folder tiles and the current folder background accept the same payload and call `move-asset`.

## Asset tabs

Double-clicking a tile opens a closeable titlebar tab for that asset (`view-asset`, the same readback path as `get-thumbnail`). The pinned `Scene` tab owns the native viewport and cannot be closed. Asset tabs show a type icon and can be reordered by drag-drop.

## In the code

| What | File | Symbols |
|---|---|---|
| Tile grid + import + drop | `editor/src/panels/AssetsPanel.tsx` | `AssetsPanel`, `AssetPanelBody`, `FolderTile`, `FolderNameInput`, `importPath`, `isInsidePanel` |
| Tile + rename + drag source | `editor/src/components/AssetTile.tsx` | `AssetTile`, `RenameInput`, `ASSET_DND_MIME` |
| Thumbnail blob-URL cache | `editor/src/state/store.ts` | `getCachedThumbnailUrl`, `getThumbnailUrl`, `invalidateThumbnails`, `thumbnailCache` |
| Asset tabs | `editor/src/app/WindowTitlebar.tsx` | `TitlebarTab`, `ViewTab`, `openAssetTab` |
| Preview content | `editor/src/components/AssetViewer.tsx` | `AssetPreview`, `viewAsset` |
| Readback (engine) | `control_commands_asset.cpp` | `get-thumbnail`, `view-asset`, `list-assets`, `rename-asset`, `move-asset`, `delete-asset`, `delete-asset-folder` |

## Related

- [Mesh thumbnails](../mesh-thumbnails/) — the 3D preview render behind a mesh tile
- [Asset pickers](../asset-pickers-and-drag-drop/) — the drop targets these tiles feed
- [Asset catalog in the scene](../../scene-and-ecs/asset-catalog-in-scene/) — the catalog this grid views
- [Asset commands](../../tooling-and-control/asset-commands/) — import/list/rename + the thumbnail readback
