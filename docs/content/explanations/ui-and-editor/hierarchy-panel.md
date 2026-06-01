+++
title = 'Hierarchy panel'
weight = 5
+++

# Hierarchy panel

The Hierarchy panel lists every entity in the scene and lets you create, copy, delete, and select them. It is a flat list (the scene has no parenting), and a pure render of the store's `entities` slice — the panel never fetches; the [reconcile poll](../selection/) keeps the list current, and the panel re-renders when it changes.

## A render of the store

The list comes from `store.entities`, which the reconcile poll refreshes only when `sceneVersion` changes. Each entity is a row; a left-click selects it, a right-click opens a Radix context menu with Copy and Delete. The header carries a Create dropdown ([the same preset list](#creating-entities) the menu bar uses).

Because the context menu and Create dropdown are Radix portals, they must render in a non-viewport region or they would be hidden behind the reparented native viewport window. The Hierarchy lives in the left dock, so its menus open over the sidebar and are never occluded.

## Selection is optimistic

Clicking a row sets the selection locally *and* tells the engine, so the row highlights without waiting a poll interval:

```ts
const onSelect = (entity: EntityRef): void => {
  selectEntity(entity.id);            // optimistic local highlight
  void client.selectEntity(entity.id).catch(() => {});
};
```

The poll confirms via `selectionVersion`; the engine is authoritative if a newer version arrives. See [Selection](../selection/) for the round-trip.

## Creating entities

The Create dropdown maps menu labels to `add-entity` presets — Empty, Cube, Point/Spot/Directional Light, Camera. The engine spawns the entity, adds the right component, and auto-selects it, so on success the panel mirrors that selection locally and the `sceneVersion` bump refreshes the list. The C++ editor's `onCreateCube` indirection is gone: the engine resolves and uploads the cube mesh itself behind `add-entity cube`, because the editor and engine are the same process now.

## Copy and delete

Copy and delete go through the engine, so there is no in-process deferred-mutation dance — the commands are safe to call from a menu handler:

```ts
const onCopy = (id) =>
  void client.copyEntity(id).then((ref) => selectEntity(ref.id)).catch(() => {});

const onDelete = (id) => {
  if (store.selectedId === id) setSelectedId(null);  // clear if it was selected
  void client.destroyEntity(id).catch(() => {});
};
```

`copy-entity` is a deep duplicate engine-side (every component, a fresh UUID) and selects the copy; the panel mirrors that selection and lets the `sceneVersion` bump refresh the list. `destroy-entity` clears the selection locally first if the deleted entity was selected, matching the engine's own clear-on-destroy.

Rename is not inline here — it is the Inspector's `Name` field, the same as the old editor.

## In the code

| What | File | Symbols |
|---|---|---|
| The panel | `editor/src/panels/HierarchyPanel.tsx` | `HierarchyPanel`, `onSelect`, `onCopy`, `onDelete` |
| Create presets | `editor/src/app/CreateMenu.tsx` | `CREATE_PRESETS`, `CreateMenu` |
| The entity list slice | `editor/src/state/store.ts` | `entities`, `sceneVersion`, `setEntities` |
| Commands (engine) | `control_commands_scene.cpp` | `list-entities`, `add-entity`, `copy-entity`, `destroy-entity`, `select` |

## Related

- [Inspector](../inspector/) — what shows for the selected entity (and where rename lives)
- [Selection](../selection/) — the optimistic select + version reconcile round-trip
- [Scene commands](../../tooling-and-control/scene-commands/) — the list/create/copy/destroy commands
