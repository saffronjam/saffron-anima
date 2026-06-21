+++
title = 'Asset pickers'
weight = 7
+++

# Asset pickers

An asset picker is a form control that edits a reference to a catalog asset. A component field stores the asset's Uuid rather than its name, and the picker presents that Uuid as a thumbnail and a label the user can change.

The picker serves two channels into the same field: a dropdown combo that lists candidate assets, and a drop target that accepts a tile dragged from elsewhere in the editor. Both are type-gated, so a field that holds a mesh can only ever be assigned a mesh. `AssetPicker` is one reusable React widget; the [inspector](../inspector/) and the [environment panel](../theme-and-fonts/) mount it on any uuid field.

## The picker combo

`AssetPicker` is a shadcn `Popover` listing `(none)` plus every catalog asset of the field's `assetType`. It reads `store.assets`, filters by type, shows the current selection's name and swatch, and emits `onChange(id)` when the user picks — `(none)` emits `"0"`:

```ts
const options = assets.filter((a) => a.type === assetType);  // mesh fields list only meshes
const isNone = value === NONE_UUID || value === "";
const selected = isNone ? null : (options.find((a) => a.id === value) ?? null);
```

A Mesh field passes `assetType: "mesh"`, an albedo or sky field `"texture"`, so a field can only hold the right kind of asset. Each row draws a small swatch through `getThumbnailUrl` at 64px — the same [blob-URL cache](../assets-panel-and-thumbnails/) the tiles use, so the picker and the grid never double-fetch a thumbnail.

The picker is **field-agnostic**: it only emits the chosen id. The inspector owns the write. `Mesh.mesh` and `Material.albedoTexture` go through the dedicated `assign-asset`; every other uuid field goes through `set-component-field`. The id is a **string** end-to-end (engine Uuids are u64), never `Number()`d.

## Type-gated drag-drop

A tile is an HTML5 drag source carrying `application/x-se-asset` — a JSON `{id, type}` payload. The picker is the matching drop target and accepts only when the dragged asset's type matches its own field type:

```ts
const onDrop = (event) => {
  event.preventDefault();
  const payload = readAssetPayload(event.dataTransfer);
  if (payload && payload.type === assetType) onChange(payload.id);  // type guard
};
```

Dragging a texture onto a Mesh field does nothing: the same type comparison guards both the combo filter and the drop, so a mismatched drop cannot land. This is a distinct channel from the OS file drop the [Assets panel](../assets-panel-and-thumbnails/) listens for, which imports a new asset rather than assigning an existing one.

## In the code

| What | File | Symbols |
|---|---|---|
| The picker widget | `editor/src/components/AssetPicker.tsx` | `AssetPicker`, `AssetSwatch`, `PickerRow` |
| Drag payload + reader | `editor/src/components/AssetTile.tsx` | `ASSET_DND_MIME`, `AssetDragPayload`, `readAssetPayload` |
| Where it's mounted | `editor/src/components/fieldRenderer.tsx` | the `uuid` case in `renderField`, `FieldHint.asset` |
| The write (client) | `editor/src/panels/InspectorPanel.tsx` | `sendWrite` (`assignAsset` / `setComponentField`) |
| Assign (engine) | `engine/crates/control/src/commands_asset.rs` | `assign-asset` |

## Related

- [Assets panel & thumbnails](../assets-panel-and-thumbnails/) — the drag source + the shared thumbnail cache
- [Inspector](../inspector/) — where the picker fields are mounted
- [Asset catalog in the scene](../../scene-and-ecs/asset-catalog-in-scene/) — the Uuid → name/path mapping
- [Asset commands](../../tooling-and-control/asset-commands/) — `assign-asset` and `list-assets`
