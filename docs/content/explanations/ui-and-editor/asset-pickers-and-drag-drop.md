+++
title = 'Asset pickers'
weight = 7
+++

# Asset pickers

A `MeshComponent` or `MaterialComponent` references an asset by Uuid, not by name. The inspector edits that Uuid with a picker combo showing each asset's thumbnail and name, and the same field doubles as a drag-drop target so you can drop a tile from the [Assets panel](../assets-panel-and-thumbnails/) onto it.

## The picker combo

`drawAssetPicker` is one reusable widget used by both the Mesh field and the Material albedo field. It reads the [asset catalog](../../scene-and-ecs/asset-catalog-in-scene/) through the scene (a borrowed pointer, tolerant of null), shows the current selection's name, and lists matching assets when opened:

```cpp
void drawAssetPicker(Scene& scene, AssetType type, const char* label, Uuid& target,
                     const std::function<ImTextureID(const AssetEntry&)>& thumbnailFor)
{
    const AssetCatalog* catalog = scene.catalog;
    ...
    if (ImGui::BeginCombo(comboId.c_str(), current.c_str()))
    {
        if (ImGui::Selectable("(none)", target.value == 0)) { target = Uuid{ 0 }; }
        for (const AssetEntry& entry : catalog->entries)
        {
            if (entry.type != type) { continue; }   // mesh fields only list meshes
            ...
            if (ImGui::Selectable(entry.name.c_str(), entry.id.value == target.value))
                target = entry.id;
        }
    }
}
```

The combo filters by `AssetType`, so a Mesh picker only offers meshes and an Albedo picker only offers textures. Selecting writes the asset's Uuid into the component field, and `(none)` clears it to `Uuid{ 0 }`. Each row draws the asset's thumbnail through the same `thumbnailFor` callback the Assets panel uses, so the picker and the tile grid show the same preview.

## The drag-drop payload

The picker field is also a drop target. The payload is a small POD carrying the asset id and its type, sent under a named channel:

```cpp
struct AssetDragPayload
{
    u64 id = 0;
    AssetType type = AssetType::Mesh;
};
```

The Assets panel sets this payload on each tile as a drag source. The picker accepts it under the same `"SE_ASSET"` channel and rejects a type mismatch before assigning:

```cpp
if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SE_ASSET"))
{
    const AssetDragPayload* drag = static_cast<const AssetDragPayload*>(payload->Data);
    if (drag != nullptr && drag->type == type)   // a texture won't drop onto a mesh field
        target = Uuid{ drag->id };
}
```

The type check makes the drop safe: dragging a texture tile onto a Mesh field does nothing, because the field declared `AssetType::Mesh`. The same comparison guards both the combo filter and the drop, so a field can't end up holding the wrong kind of asset.

## Why a callback for thumbnails

`drawAssetPicker` doesn't render thumbnails itself; it's handed a `thumbnailFor` closure. The editor module has no renderer or asset server — those live in the client — so turning an asset into a GPU texture is delegated. The client's closure caches the registered `ImTextureID` per asset so `AddTexture` runs once, and falls back to a type icon when a real preview isn't available. See [Assets panel & thumbnails](../assets-panel-and-thumbnails/) for that side.

## In the code

| What | File | Symbols |
|---|---|---|
| The picker widget | `editor_components.cpp` | `drawAssetPicker` |
| Where it's used | `editor_components.cpp` | Mesh + Material albedo draws in `registerBuiltinComponents` |
| The payload | `editor_context.cppm` | `AssetDragPayload` |
| Drop accept + type guard | `editor_components.cpp` | `AcceptDragDropPayload("SE_ASSET")` |

## Related

- [Assets panel & thumbnails](../assets-panel-and-thumbnails/) — the drag source + thumbnail callback
- [Inspector](../inspector/) — where the picker fields are drawn
- [Asset catalog in the scene](../../scene-and-ecs/asset-catalog-in-scene/) — the Uuid → name/path mapping
