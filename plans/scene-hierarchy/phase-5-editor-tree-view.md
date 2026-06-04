# Phase 5: Editor: tree-view outliner with drag-reparent and the pinned Environment node

**Status:** NOT STARTED

<!-- Flip to COMPLETED (with a dated note) once every "Done when" box is checked and the run is validation-clean. Delete this file only after COMPLETED + merged. -->

## Goal

Replace the flat `HierarchyPanel` with a real outliner: build the tree client-side from the
flat `entities` array plus the per-entity `parentId` that phase 4 added to `list-entities`,
render twisties + depth indentation, drag a row onto another to reparent via the `set-parent`
command, pin a non-deletable virtual `Environment` sentinel node at the root, and keep
expand/collapse state OUT of the `sceneVersion`-gated reconcile poll so a scene mutation never
collapses the tree. This supersedes the editor's flat-list/no-parenting non-goal, documented at
`docs/content/explanations/ui-and-editor/hierarchy-panel.md` (a flat list is parity;
parenting/resolveRefs were an explicit non-goal).

This is the editor half. The engine `RelationshipComponent` + world-transform machinery
(phases 1-3), the `set-parent` command + `parentId` on the entity-list entry (phase 4), and the
architecture decisions (sentinel env node, flat-list+parentId wire shape, ids-as-strings) are
settled upstream — see the scene-hierarchy `README.md` / phase-0. Do not re-litigate them here.

## Current state

- `entities: EntityRef[]` is a flat array in the store (`store.ts:25`, init `:92`), replaced
  whole by `setEntities` (impl `store.ts:110`). It is filled only by the reconcile poll's
  version-gated heavy lane (`refreshHeavyState`, `store.ts:251-317`) on a `sceneVersion` diff,
  via `client.listEntities()` (`store.ts:284`) → `setEntities(list.entities)` (`store.ts:289`),
  and the write is skipped while `dragActive` (`store.ts:288`).
- `EntityRef` is `{ id, name }` (generated `editor/src/protocol/se-types.ts:107`, re-exported by
  the hand-curated `editor/src/protocol/index.ts`). Phase 4 adds an optional `parentId` to a
  dedicated list-entry DTO (`EntityListEntry`), NOT to the shared `EntityRef` returned by ~12
  commands — so the outliner reads `parentId` off the same `entities` slice it already polls.
- `EntityList` is `{ entities: EntityRef[] }` (generated `editor/src/protocol/se-types.ts:250-251`,
  re-exported by `index.ts`).
- `selectedId` (`store.ts:26`) + optimistic `selectEntity` (`store.ts:119`), confirmed by
  `selectionVersion` / `get-selection`. `dragActive` (`store.ts:43`, setter impl `:177`) already
  exists and is honored across the poll to protect optimistic writes mid-scrub. There is no
  expand-state and no tree shape today.
- `resetSceneState` (`store.ts:152`) clears `entities` + `selectedId` + `componentsBySelected`
  on a project/scene load and forces the version diff to re-fire.
- `HierarchyPanel.tsx` maps `entities` to flat `<button>` rows (`HierarchyPanel.tsx:95-134`),
  each wrapped in a Radix `ContextMenu` (Copy/Delete, `:122-132`); left-click → optimistic
  `selectEntity` + `client.selectEntity` (`:40-43`). The file-comment (`:8-11`) banks on rows
  living in the left column so Radix anchors the menu over the sidebar, never over the X11
  viewport rect. No indentation, twisties, or drag.
- `client.ts` scene wrappers sit near `selectEntity` (`client.ts:101`) / `destroyEntity`
  (`client.ts:104`); the typed `call` passthrough (`client.ts:83-88`) carries untyped mutations
  too (a not-yet-result-mapped command still goes through `call`, resolving as `unknown`).
- `EnvironmentPanel.tsx` is a tab sibling of Inspector/Stats in `Layout.tsx` (`LeftBottomTabs`),
  keyed off `get-environment` / `set-environment` and `sceneVersion`. The env is panel-resident
  today, not a hierarchy row. `Scene.environment` is global engine state, not an entity (the
  completed-and-removed skybox plan settled this, recorded in the scene.cppm:242-244 comment and
  the docs under `docs/content/explanations/image-based-lighting/`; the phase-0 decision affirms
  it) — the env node is a client-side sentinel only.
- `WireUuid` is `type: string` on the wire (generated `editor/src/protocol/se-types.ts:7`,
  from the `WireUuid` DTO in `control_dto.cppm:21-24`), so `parentId` is a string in JSON and in
  TS — never `Number()` it.

## Implementation

### 1. Carry `parentId` and add expand-state to the store (`editor/src/state/store.ts`)

- `entities` stays a flat array (`store.ts:25`); its element type switches from `EntityRef` to the
  regenerated `EntityListEntry` (carrying the optional `parentId`), so the store edit is the type
  annotation + import swap — `EntityRef` → `EntityListEntry` — plus the `setEntities` signature.
- Add expand-state to `EditorState` (interface near `store.ts:43-49`, impl near `store.ts:105-108`):
  - `expandedIds: Set<string>` (init empty set).
  - `toggleExpanded(id: string): void` — flip membership.
  - `setExpanded(id: string, expanded: boolean): void` — explicit set/clear (used by the
    chevron and by "expand all ancestors on select").
  These are PLAIN UI state. Keep them OUT of the `sceneVersion`/`selectionVersion` keying — they
  must never be reset by the reconcile poll, or every scene mutation collapses the tree.
- In `setEntities` (impl `store.ts:110`) prune `expandedIds`: drop any id no longer present in the
  new `entities` (a vanished id can never have visible children). Keep all surviving ids. New
  parents default collapsed (do not auto-expand on appearance). Do this as a small merge inside
  `setEntities` so the reconcile path at `store.ts:289` reconciles expand-state in lockstep with
  the entity list.
- `resetSceneState` (`store.ts:152`) clears `expandedIds` (new scene, no stale expansion) alongside
  the `entities`/`selectedId` reset.
- Persist `expandedIds` across reloads via `localStorage` keyed by project path (mirror how
  `viewportHidden` and friends are plain UI state). Serialize the set as a string array; rehydrate
  on project open in the same place `project` is set. Per-session-only is an acceptable fallback if
  the project-path key is awkward — persistence is polish, not load-bearing.
- Add a module-level (NOT Zustand) tree builder near the other module helpers (e.g. beside
  `getThumbnailUrl`):
  ```ts
  export interface TreeNode { entity: EntityListEntry; children: TreeNode[]; }
  export function buildTree(entities: EntityListEntry[]): TreeNode[];
  ```
  Group entities by `parentId` under a synthetic root (`parentId` null/absent/`"0"` → top level).
  An entry whose `parentId` references a missing or self id is treated as a root (defensive: the
  engine already roots dangling parents on load, but the client must not loop). Preserve the
  engine's array order within each sibling group (siblings are unordered in v1).
- Add the optimistic reparent action `setParent(id: string, parentId: string | null): void`:
  - Optimistically relink in-place: set the moved entity's `parentId` field in `entities` and
    leave every other field (and `selectedId`) untouched. The relink MUST NOT clear `selectedId`
    or churn selection.
  - Call `client.setParent(id, parentId)`; set `dragActive` true for the duration (mirroring the
    scrub/gizmo pattern) so the poll's `setEntities` at `store.ts:288-289` cannot clobber the
    optimistic relink mid-round-trip; clear `dragActive` in `finally`.
  - On reject, the next poll's authoritative `setEntities` restores the true tree — no manual
    rollback needed beyond clearing `dragActive`.

### 2. Typed `setParent` wrapper (`editor/src/control/client.ts`)

- Add to the scene section near `selectEntity` (`client.ts:101`) / `destroyEntity` (`client.ts:104`):
  ```ts
  setParent(id: string, parentId: string | null): Promise<unknown> {
    return call("set-parent", { entity: id, parent: parentId ?? null });
  },
  ```
  The typed `call` passthrough (`client.ts:83-88`) carries an as-yet-unmapped command too,
  resolving as `unknown`; if phase 4 returns an `EntityRef` from `set-parent` and registers it in
  `CommandResultMap`, the wrapper's return type tightens to `Promise<EntityRef>` with no call-site
  change. `parentId` stays a `string | null` end-to-end — never `Number()` it. `null` (or `"0"`,
  matching the engine root sentinel) detaches to root.

### 3. Split out `HierarchyTree` + `TreeRow` (`editor/src/panels/HierarchyTree.tsx`, new)

- Create `editor/src/panels/HierarchyTree.tsx` exporting `HierarchyTree` (renders `buildTree`
  output) and an internal recursive `TreeRow`. `HierarchyPanel.tsx` keeps the header + `CreateMenu`
  (`HierarchyPanel.tsx:84-89`) and `ScrollArea` (`:90`), and replaces the flat `entities.map`
  (`:95-134`) with `<HierarchyTree nodes={buildTree(entities)} />`.
- `TreeRow` props: `node: TreeNode`, `depth: number`. Per row:
  - Indent by `depth` (left padding `depth * step`); render a twisty/chevron button when
    `node.children.length > 0`, calling `toggleExpanded(node.entity.id)`; render an inert spacer
    when it has no children so labels align.
  - Reuse the existing selected-row styling: the `cn(...)` classes and `aria-selected` from
    `HierarchyPanel.tsx:108-114` (`bg-primary text-primary-foreground` when selected,
    `hover:bg-accent` otherwise).
  - Keep the select handler (`HierarchyPanel.tsx:40-43`): optimistic `selectEntity` +
    `client.selectEntity`.
  - Keep the Radix `ContextMenu` (`HierarchyPanel.tsx:96-133`) sidebar-anchored. Keep Copy/Delete
    (`onCopy`/`onDelete`, `HierarchyPanel.tsx:52-67`) and ADD:
    - `Unparent` → `setParent(id, null)`.
    - `Parent to…` → a submenu listing valid targets (every entity that is not the row itself and
      not a descendant of it), each → `setParent(id, targetId)`.
  - Only render children (recurse `TreeRow` at `depth + 1`) when `expandedIds.has(node.entity.id)`.
- Drag-to-reparent with HTML5 DnD, all affordances INSIDE the sidebar DOM:
  - `draggable` on the row; `onDragStart` stashes the dragged id (`dataTransfer.setData`); set
    `store.setDragActive(true)` for the gesture and clear it on `onDragEnd`.
  - `onDragOver` on a candidate row: pre-filter — reject if the target is the dragged row itself or
    any descendant of it (walk the dragged node's subtree client-side from `buildTree`, or walk the
    target's ancestor chain up via `parentId` and reject if it reaches the dragged id). On a valid
    target call `preventDefault()` and show an in-DOM highlight (a row background ring or an
    insertion line drawn with sidebar DOM only).
  - `onDrop` on a valid target: `store.setParent(draggedId, targetId)`. Dropping into the empty root
    area (or onto a dedicated root drop strip) → `setParent(draggedId, null)`.
  - Self/descendant drops are rejected BEFORE the round-trip (the engine cycle guard from phase 1/4
    is the backstop, but the gesture must not even fire `set-parent`).
- X11 overlay constraint: do NOT call `dataTransfer.setDragImage` to a floating element, and do NOT
  portal a drop indicator that can overlap the viewport rect — the reparented X11 child paints on
  top and any such layer is painted over. Use only in-flow sidebar DOM (row highlight + insertion
  line). Keep the `ContextMenu` Radix-anchored to the sidebar exactly as today
  (`HierarchyPanel.tsx:8-11`).

### 4. Pinned `Environment` sentinel node (`editor/src/panels/HierarchyTree.tsx` + bottom-tab wiring)

- Render a fixed `Environment` pseudo-row at the very top of the tree root, above the entity rows.
  It is NOT a `TreeRow` over a real `EntityRef` and NOT in `buildTree` output — model it as a
  sentinel.
- The sentinel is non-deletable (no Delete in its context menu, no `destroy-entity`), non-draggable
  (`draggable={false}`, never a drop target), and has no entity id.
- Selecting it switches the bottom tab to `EnvironmentPanel`. Keep ONE env editor: the node
  selects-and-switches-tab; do not build a second inline env editor. Drive the active bottom tab
  from store state — add a small `bottomTab` slice (e.g. `'inspector' | 'environment' | 'stats'`,
  default `'inspector'`) read by `Layout.tsx`'s `LeftBottomTabs`, and have the env-row click set it
  to `'environment'`. Model the env selection as a sentinel selection (e.g. a `selectedSentinel:
  'environment' | null` flag), NEVER a fake `EntityRef` and never written to `selectedId` — so
  `get-selection` / `inspect` and the reconcile poll are never asked to resolve a non-entity id.
- Style the env row with the same selected-row `cn(...)` classes so it reads as part of the tree,
  but gate its highlight on the sentinel flag, not `selectedId`.

### 5. Regenerated protocol (`editor/src/protocol/se-types.ts`)

- The wire contract is DTO-first: `parentId` lands on phase 4's dedicated `EntityListEntry` DTO
  (`{ id, name, parentId? }`) in `control_dto.cppm`, which switches `EntityList.entities` off the
  shared `EntityRef`. `editor/src/protocol/se-types.ts` is generated by `tools/gen-control-dto/gen.ts`
  (run via `bun run gen:protocol`, a shim spawning that generator; `bun run check`); the
  hand-curated `editor/src/protocol/index.ts` re-exports from it. After regen, `EntityList.entities`
  is `EntityListEntry[]` carrying the optional `parentId: WireUuid` (still a `string`). Point
  `buildTree`/`setEntities` at the generated `EntityListEntry` item type, and the store/client/tree
  consume it.

## Done when

- [ ] `bun run check` (gen:protocol + `tsc --noEmit`) and `bun run lint` (oxlint) pass clean;
      `bun run build` succeeds.
- [ ] The outliner shows a nested tree built from `parentId`: children indent under their parent,
      twisties appear only on rows with children, and expand/collapse works.
- [ ] An e2e/headless round-trip parents a scene: `se add-entity` ×2, `se set-parent <child>
      --parent <parent>`, then `se list-entities` reports the child's `parentId`; `writeScene` then
      `readScene` of that scene preserves the parent link (the engine round-trip from phase 1-4 is
      the substrate — assert it from `tests/e2e`).
- [ ] Dragging a row onto another reparents via `set-parent` and the native viewport updates;
      dropping onto self or a descendant is rejected client-side BEFORE any `set-parent` round-trip.
- [ ] A poll/scene refresh never collapses the tree: trigger a `sceneVersion` bump (e.g. `se
      add-entity`) with rows expanded and confirm expand-state survives; expand-state survives a
      reload via the persisted store; only vanished ids are pruned from `expandedIds`.
- [ ] Reparent does not churn selection: with an entity selected, `se reparent`/drag a different row
      and confirm `selectedId` is unchanged and `get-selection` still reports the same entity.
- [ ] The `Environment` node is pinned, non-deletable, and non-draggable; selecting it opens the
      `EnvironmentPanel` tab and is modeled as a sentinel — `get-selection` / `inspect` are never
      called with a non-entity id, and `selectedId` stays null/unchanged when the env node is active.
- [ ] All drag visuals stay inside the sidebar DOM: no `setDragImage` floating layer, no portal'd
      indicator over the viewport rect, and the `ContextMenu` stays Radix-anchored to the sidebar.
- [ ] A headless run driving these operations over the control plane is validation-clean (no Vulkan
      validation errors, no engine `ok:false` surfaced as an unhandled reject).

## Risks / seams

- X11 overlay z-order: native drag images and portal'd drop indicators that overlap the viewport
  rect get painted over by the reparented X11 child and look broken. Constrain every drag visual to
  in-flow sidebar DOM (row highlight + insertion line); never `setDragImage` to a floating element.
  This is the same constraint that keeps the `ContextMenu` sidebar-anchored (`HierarchyPanel.tsx:8-11`).
- Expand-state vs the version-gated poll: if `expandedIds` lived under `sceneVersion` keying, every
  scene mutation would collapse the tree. Keep it as plain UI state outside the poll and prune only
  vanished ids in `setEntities` (impl `store.ts:110`, reconcile call `:289`).
- Selection churn on reparent: the optimistic relink in `setParent` must not clear `selectedId`, and
  `set-parent` bumping `sceneVersion` (engine-side) must not silently change selection. Verify the
  React relink touches only the moved entity's `parentId`.
- Cycle / self-parent: dropping a node onto its own descendant must be rejected by the client
  pre-filter so the gesture never fires `set-parent`; the engine cycle guard (phase 1/4) is the
  backstop, not the first line of defense.
- One env editor, two surfaces: `EnvironmentPanel` is a bottom tab AND a tree node. Keep a single
  editor — the node selects-and-switches-tab — to avoid two divergent env UIs. The env node has no
  entity id; model it as a sentinel selection, never a fake `EntityRef`, so `get-selection` /
  `inspect` / the reconcile poll are never handed a non-entity id.
- `dragActive` reuse: the reparent gesture borrows the existing `dragActive` gate (`store.ts:43`).
  Ensure it is cleared in every exit path (drop success, drop reject, `onDragEnd`, `setParent`
  catch/finally) or the poll stays frozen and the tree goes stale.
- Hard dependency on phases 1-4: the tree is only as real as the `parentId` the engine emits and the
  `set-parent` command it serves. Until those land, `buildTree` produces an all-top-level list and
  drag-reparent has nothing to call. Land the engine + wire half first.
