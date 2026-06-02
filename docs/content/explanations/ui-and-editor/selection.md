+++
title = 'Selection'
weight = 9
+++

# Selection

Selection is the editor's notion of the one entity currently being edited. It exists in two places at once: the engine holds the authoritative value, and the React store keeps a fast local mirror.

A user selects in three ways:

- clicking a row in the [hierarchy](../hierarchy-panel/);
- clicking a light or camera billboard in the viewport;
- ray-picking a mesh.

Clicking empty space deselects. The two copies stay in agreement through a version-stamped poll, and local writes apply optimistically, so the interface never waits on a round-trip to the engine.

## Optimistic select, then reconcile

A click sets `store.selectedId` immediately, then fires the engine command:

```ts
selectEntity(id);                              // local highlight, no wait
void client.selectEntity(id).catch(() => {});  // tell the engine
```

The engine bumps a `selectionVersion` on every selection change, whether from `select`, `deselect`, `pick`, or a destroy that clears it. The reconcile poll reads `get-selection` each tick; it returns `{entity, selectionVersion, sceneVersion}`, and re-applies the selection only when the version or the selected id has changed. The common case (nothing changed) costs one cheap call, while an external change such as `se select` still propagates back into the store within a tick.

```ts
const nextSelectedId = selection.entity ? selection.entity.id : null;
const selectionChanged =
  selection.selectionVersion !== knownSelectionVersion ||
  nextSelectedId !== knownSelectedId;
if (selectionChanged) live.setSelectedId(nextSelectedId);
```

When the selection or `sceneVersion` changes, the poll re-`inspect`s the new entity into `componentsBySelected`, which the [inspector](../inspector/) renders. Writes are gated off while `dragActive`, so a poll never clobbers a gizmo or scrub drag in progress.

## Picking in the viewport

A plain left-click in the viewport is a ray-pick: a press that does not travel far enough to count as a [gizmo](../gizmo/) drag. The [viewport panel](../viewport-panel/) maps the click to `{u,v}` in `[0,1]` and calls `pick`:

```ts
const result = await client.pick(u, v);
if (result.hit && result.id) setSelectedId(result.id);
else setSelectedId(null);   // empty space deselects
```

The engine builds a ray from the [editor camera](../editor-camera/) through that UV, tests billboards first then the nearest mesh AABB, selects the hit while bumping `selectionVersion`, and returns `{hit, id?, name?}`. A miss returns `{hit:false}` and deselects. The store update is optimistic, and the reconcile poll confirms it through the version it just bumped.

The ray and AABB math, along with the Vulkan-clip caveat, are covered in [Picking](../../scene-and-ecs/picking/). The click-versus-drag split, which makes a click on a gizmo handle drag rather than pick, lives in the viewport panel's pointer gesture.

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
