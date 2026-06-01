+++
title = 'Selection'
weight = 9
+++

# Selection

There is one selected entity at a time, and it lives in two places: the engine (authoritative) and the React store (a fast local mirror). You select by clicking a row in the [hierarchy](../hierarchy-panel/), clicking a light/camera billboard in the viewport, or ray-picking a mesh; clicking empty space deselects. The two stay in sync through a version-stamped poll, with optimistic local writes so the UI never feels a round-trip.

## Optimistic select, then reconcile

A click sets `store.selectedId` immediately, then fires the engine command:

```ts
selectEntity(id);                              // local highlight, no wait
void client.selectEntity(id).catch(() => {});  // tell the engine
```

The engine bumps a `selectionVersion` on every selection change (from `select`, `deselect`, `pick`, or a destroy that clears it). The reconcile poll reads `get-selection` each tick — it returns `{entity, selectionVersion, sceneVersion}` — and only re-applies the selection when the version or the selected id actually changed. So the common case (nothing changed) costs one cheap call, and an *external* change (e.g. `se select`) still propagates back into the store within a tick.

```ts
const nextSelectedId = selection.entity ? selection.entity.id : null;
const selectionChanged =
  selection.selectionVersion !== knownSelectionVersion ||
  nextSelectedId !== knownSelectedId;
if (selectionChanged) live.setSelectedId(nextSelectedId);
```

When the selection (or `sceneVersion`) changes, the poll re-`inspect`s the new entity into `componentsBySelected`, which is what the [inspector](../inspector/) renders. Writes are gated off while `dragActive`, so a gizmo or scrub drag in progress is never clobbered by a poll.

## Picking in the viewport

A plain left-click in the viewport (a press that doesn't travel far enough to be a [gizmo](../gizmo/) drag) is a ray-pick. The [viewport panel](../viewport-panel/) maps the click to `{u,v}` in `[0,1]` and calls `pick`:

```ts
const result = await client.pick(u, v);
if (result.hit && result.id) setSelectedId(result.id);
else setSelectedId(null);   // empty space deselects
```

The engine builds a ray from the [editor camera](../editor-camera/) through that UV, tests billboards first then the nearest mesh AABB, selects the hit (bumping `selectionVersion`), and returns `{hit, id?, name?}`. A miss returns `{hit:false}` and deselects. The store update is optimistic; the reconcile poll confirms it via the version it just bumped.

The ray + AABB math and the Vulkan-clip caveat are covered in [Picking](../../scene-and-ecs/picking/). The click-vs-drag split (so a click on a gizmo handle drags rather than picks) lives in the viewport panel's pointer gesture.

## In the code

| What | File | Symbols |
|---|---|---|
| Selection slice + optimistic select | `editor/src/state/store.ts` | `selectedId`, `selectEntity`, `setSelectedId` |
| Version-stamped reconcile | `editor/src/state/store.ts` | `startReconcile`, `selectionVersion`, `sceneVersion`, `getSelection` |
| Hierarchy click | `editor/src/panels/HierarchyPanel.tsx` | `onSelect` |
| Viewport pick | `editor/src/panels/ViewportPanel.tsx` | `runPick`, `client.pick` |
| Selection + pick (engine) | `control_commands_scene.cpp` | `select`, `get-selection`, `deselect`, `pick`; `pickEntity` |
| Poll counters (engine) | `editor.cppm` | `selectionVersion`, `sceneVersion`, `setSelection` |

## Related

- [Picking](../../scene-and-ecs/picking/) — the ray + AABB math behind click-select
- [Hierarchy panel](../hierarchy-panel/) — selection by list click
- [Gizmo](../gizmo/) — why a press is split into click (pick) vs drag (manipulate)
- [Scene commands](../../tooling-and-control/scene-commands/) — `select`/`get-selection`/`deselect`/`pick` and the poll counters
