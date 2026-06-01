+++
title = 'Asset pickers'
weight = 7
+++

# Asset pickers

A `MeshComponent` or `MaterialComponent` references an asset by Uuid, not by name. The [inspector](../inspector/) edits that Uuid with a picker combo showing each asset's thumbnail and name, and the same field doubles as a drop target so you can drag a tile from the [Assets panel](../assets-panel-and-thumbnails/) onto it. `AssetPicker` is one reusable React widget the inspector (and the [environment panel](../theme-and-fonts/)) mount on any uuid field.

## The picker combo

`AssetPicker` is a shadcn `Popover` listing `(none)` plus every catalog asset of the field's `assetType`. It reads `store.assets`, filters by type, shows the current selection's name and swatch, and emits `onChange(id)` when you pick — `(none)` emits `"0"`:

```ts
const options = assets.filter((a) => a.type === assetType);  // mesh fields list only meshes
const isNone = value === NONE_UUID || value === "";
const selected = isNone ? null : (options.find((a) => a.id === value) ?? null);
```

A Mesh field passes `assetType: "mesh"`, an albedo or sky field `"texture"`, so a field can only ever hold the right kind of asset. Each row draws a small swatch through `getThumbnailUrl` at 64px — the same [blob-URL cache](../assets-panel-and-thumbnails/) the tiles use, so the picker and the grid never double-fetch a thumbnail.

The picker is **field-agnostic**: it only emits the chosen id. The inspector owns the write — `Mesh.mesh` and `Material.albedoTexture` go through the dedicated `assign-asset`, every other uuid field through `set-component-field`. The id is a **string** end-to-end (engine Uuids are u64), never `Number()`d.

## Drag-drop, type-gated

A tile is an HTML5 drag source carrying `application/x-se-asset` — a JSON `{id, type}`, the React analog of the old `AssetDragPayload`. The picker is the matching drop target and accepts only when the dragged asset's type matches its own field type:

```ts
const onDrop = (event) => {
  event.preventDefault();
  const payload = readAssetPayload(event.dataTransfer);
  if (payload && payload.type === assetType) onChange(payload.id);  // type guard
};
```

Dragging a texture onto a Mesh field does nothing — the same type comparison guards both the combo filter and the drop, so a mismatched drop can't land. This is a distinct channel from the OS file drop the [Assets panel](../assets-panel-and-thumbnails/) listens for (which imports a new asset rather than assigning an existing one).

## Kept out of the viewport

Like every Radix popover in the editor, the picker's dropdown is portalled to the document root. It must open over a non-viewport region or the reparented native window would occlude it, so the pickers live in the side docks (inspector / environment) where their popovers open over the sidebar. Same rule as the menus and the loading overlay — the [native bridge](../tauri-editor-and-x11-bridge/) page covers why.

## In the code

| What | File | Symbols |
|---|---|---|
| The picker widget | `editor/src/components/AssetPicker.tsx` | `AssetPicker`, `AssetSwatch`, `PickerRow` |
| Drag payload + reader | `editor/src/components/AssetTile.tsx` | `ASSET_DND_MIME`, `AssetDragPayload`, `readAssetPayload` |
| Where it's mounted | `editor/src/components/fieldRenderer.tsx` | the `uuid` case in `renderField`, `FieldHint.asset` |
| The write (client) | `editor/src/panels/InspectorPanel.tsx` | `sendWrite` (`assignAsset` / `setComponentField`) |
| Assign (engine) | `control_commands_asset.cpp` | `assign-asset` |

## Related

- [Assets panel & thumbnails](../assets-panel-and-thumbnails/) — the drag source + the shared thumbnail cache
- [Inspector](../inspector/) — where the picker fields are mounted
- [Asset catalog in the scene](../../scene-and-ecs/asset-catalog-in-scene/) — the Uuid → name/path mapping
- [Asset commands](../../tooling-and-control/asset-commands/) — `assign-asset` and `list-assets`
