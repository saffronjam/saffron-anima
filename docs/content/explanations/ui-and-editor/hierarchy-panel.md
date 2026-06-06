+++
title = 'Hierarchy panel'
weight = 5
+++

# Hierarchy panel

The Hierarchy panel is the editor's outliner: a tree of every entity in the scene with create,
copy, delete, rename, select, and reparent operations. The engine ships a flat list — each
`list-entities` entry carries an optional `parentId` — and the client groups it into a forest
(`buildTree`), so the wire stays cheap to diff and the server never serializes a nested graph.
A pinned `Environment` row sits above the entities; it is a client-side sentinel for the global
[scene environment](../../image-based-lighting/), not an entity.

The panel renders directly from the store's `entities` slice and never fetches on its own. The
[reconcile poll](../selection/) keeps that slice current, and the panel re-renders whenever it
changes.

## A render of the store

The rows come from `store.entities`, which the reconcile poll refreshes only when `sceneVersion`
changes. `buildTree` groups the flat array by `parentId` (absent or `"0"` means root; an unknown
or self-referencing parent lands the row at the root, so corrupt data cannot loop the client).
Rows indent by depth and show a twisty only when they have children.

Expand/collapse state (`expandedIds`) is plain UI state, deliberately outside the
`sceneVersion` keying — a scene mutation never collapses the tree. `setEntities` prunes ids that
left the scene, selection reveals collapsed ancestors, and the set persists per project path in
`localStorage`. The context menu and the inline rename input anchor to the sidebar rows: the
viewport is a reparented native X11 child that paints over the webview, so every popover and drag
affordance must stay in non-viewport DOM.

A header toggle (default off) additionally shows the **selected** entity's components as
read-only leaf subrows under its row, sourced from the `inspect` result the poll already keeps
for the Inspector — never an extra control call, and never for non-selected rows. Clicking a
subrow keeps the entity selected and scrolls the matching Inspector section into view (a one-shot
`focusComponent` signal); editing always stays in the Inspector. The subrow list reuses the
Inspector's `orderedComponentNames`, so the tree leaves and the panel sections share one order
and one hidden set (`Relationship` never shows in either).

A second header toggle hides skeleton joints: rows flagged `bone` by `list-entities` drop out of
the rendered tree and their non-bone descendants re-anchor to the nearest visible ancestor
(`reanchorPastBones`) — a rig's dozens of joint rows collapse away without orphaning the skinned
mesh. The filter shapes only what renders; drag validity and reparenting still run against the
real ancestry.

## Selection is optimistic

Clicking a row sets the selection locally and tells the engine in the same step, so the row
highlights without waiting a poll interval:

```ts
const onSelect = (entity: EntityListEntry): void => {
  selectEntity(entity.id);            // optimistic local highlight
  void client.selectEntity(entity.id).catch(() => {});
};
```

The poll confirms via `selectionVersion`; the engine is authoritative if a newer version arrives.
See [Selection](../selection/) for the round-trip.

## Reparenting

Dragging a row onto another (or the context menu's `Parent to…` / `Unparent`) calls the store's
`setParent`, which relinks the moved entity's `parentId` optimistically — selection untouched —
and sends `set-parent` to the engine, holding `dragActive` over the round trip so the poll cannot
clobber the relink. The engine is authoritative: it refuses self-parents and cycles and rebases
the child's local transform so its world placement does not move (see the
[scene hierarchy](../../scene-and-ecs/scene-hierarchy/)). The client pre-filters anyway —
dropping a row onto itself or its own descendant never fires the command — and a rejected
reparent rolls the optimistic relink back, since a failed `set-parent` bumps no `sceneVersion`
for the poll to recover from.

Drag visuals are in-flow sidebar DOM only (a row ring and a root drop strip); a floating drag
image or a portal'd indicator over the viewport rect would be painted over by the native child.

The tree holds entities only. The environment is global `Scene` state, not an entity, and is
edited solely through the Environment tab in the sidebar — it never appears as a tree row, so
`get-selection` / `inspect` are never handed a non-entity id.

## Creating entities

The Create dropdown maps menu labels to `add-entity` presets — Empty, Cube, Point/Spot/Directional
Light, Camera. The engine spawns the entity, adds the right component, and auto-selects it. On
success the panel mirrors that selection locally, and the `sceneVersion` bump refreshes the list.
The engine resolves and uploads the cube mesh itself behind `add-entity cube`.

## Copy, delete, rename

Copy and delete go through the engine, so the commands are safe to call directly from a menu
handler. `copy-entity` is a deep duplicate — every component, a fresh UUID — that joins the
source's parent as a sibling and selects the copy. `destroy-entity` takes the whole subtree with
it and clears the engine selection when it sat anywhere inside; the panel clears its local
selection for the root case in the same step.

Rename is inline: double-click a row (or use the context menu) to edit in place. Enter or blur
commits through `rename-entity` with an optimistic row update; Escape cancels.

## In the code

| What | File | Symbols |
|---|---|---|
| The panel shell | `editor/src/panels/HierarchyPanel.tsx` | `HierarchyPanel`, `TreeActions` |
| Tree, rows, drag, sentinel | `editor/src/panels/HierarchyTree.tsx` | `HierarchyTree`, `TreeRow`, `EnvironmentRow`, `RenameRow` |
| Tree building + expand-state | `editor/src/state/store.ts` | `buildTree`, `expandedIds`, `setParent`, `selectedSentinel` |
| Create presets | `editor/src/app/CreateMenu.tsx` | `CREATE_PRESETS`, `CreateMenu` |
| Commands (engine) | `control_commands_scene.cpp` | `list-entities`, `set-parent`, `add-entity`, `copy-entity`, `destroy-entity`, `select` |

## Related

- [Scene hierarchy](../../scene-and-ecs/scene-hierarchy/) — the engine-side relationship + world transforms
- [Inspector](../inspector/) — what shows for the selected entity
- [Selection](../selection/) — the optimistic select + version reconcile round-trip
- [Scene commands](../../tooling-and-control/scene-commands/) — the list/create/copy/destroy commands
