+++
title = 'Assets panel & thumbnails'
weight = 8
+++

# Assets panel & thumbnails

The Assets panel is a tile grid of everything in the project's [asset catalog](../../scene-and-ecs/asset-catalog-in-scene/). Each tile shows a thumbnail and an editable name, and is a drag source for the inspector's [pickers](../asset-pickers-and-drag-drop/). The grid is generic over asset type; what a tile *shows* depends on a thumbnail callback the client supplies.

## The tile grid

`assetCatalogPanel` lays the catalog out in columns sized to the panel width. Each entry is a group: a 72px thumbnail image (or a `MESH`/`TEX` placeholder button when no thumbnail is available yet), then an in-place name field beneath it. A column counter wraps the grid with `ImGui::SameLine`. When the catalog is empty the panel shows a prompt to import or drag-and-drop instead.

## Rename in place

The name under each tile is a live edit field bound straight to the catalog entry:

```cpp
ImGui::SetNextItemWidth(tileSize);
ImGui::InputText("##name", &entry.name);  // rename in place (UTF-8)
```

`entry.name` is the catalog's own string, so typing renames the asset immediately. The catalog is passed in non-const for exactly this. Names are UTF-8, so non-Latin names round-trip through the project file (rendering them needs a broader font, a known follow-up).

## Each tile is a drag source

A tile sets the `AssetDragPayload` the inspector pickers accept:

```cpp
if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
{
    AssetDragPayload payload{ entry.id.value, entry.type };
    ImGui::SetDragDropPayload("SE_ASSET", &payload, sizeof(payload));
    ImGui::TextUnformatted(entry.name.c_str());
    ImGui::EndDragDropSource();
}
```

`SourceAllowNullID` is needed because, when a tile has a real thumbnail, its last drawn item is a plain `ImGui::Image` with no ID; the flag lets ImGui derive a drag-source id from the item's position. See [the picker side](../asset-pickers-and-drag-drop/) for the matching drop.

## Thumbnails come from a callback

The panel renders a thumbnail through `thumbnailFor`, a closure passed in by the client. The panel never touches the renderer — it calls back and draws whatever `ImTextureID` it gets. The client's closure decides what each asset shows and caches the result so it's registered once:

- a **texture** asset shows its own decoded image, registered with [the ImGui Vulkan backend](../imgui-integration/);
- a **mesh** asset shows a [3D preview](../mesh-thumbnails/) rendered by `renderMeshThumbnail`;
- anything else (or a load failure) falls back to a vendored Lucide **SVG** icon, rasterized via nanosvg in `uploadSvgIcon`.

The cache (`state->thumbnails`) keyed by asset id keeps `AddTexture` from running every frame: the first frame that asks for a thumbnail builds it, the rest hit the cache.

## Double-click to view

Double-clicking a tile (or "View" from its context menu) opens a floating preview window through the `onView` closure. A mesh renders a larger 512px preview; a texture shows the decoded image. `viewerPanel` draws it square in a resizable window, and the client unregisters the preview texture when the window closes.

## In the code

| What | File | Symbols |
|---|---|---|
| Tile grid + columns | `editor_panels.cpp` | `assetCatalogPanel` |
| In-place rename | `editor_panels.cpp` | `ImGui::InputText("##name", &entry.name)` |
| Drag source | `editor_panels.cpp` | `BeginDragDropSource`, `SourceAllowNullID` |
| Thumbnail callback + cache | `editor_app.cppm` | `thumbnailFor`, `state->thumbnails` |
| Mesh preview render | `renderer_thumbnail.cpp` | `renderMeshThumbnail` |
| Preview window | `editor_panels.cpp` | `viewerPanel`, `onView` |

## Related

- [Mesh thumbnails](../mesh-thumbnails/) — the orthographic 3D preview render
- [Asset pickers](../asset-pickers-and-drag-drop/) — the drop targets these tiles feed
- [Asset catalog in the scene](../../scene-and-ecs/asset-catalog-in-scene/) — the catalog this grid views
