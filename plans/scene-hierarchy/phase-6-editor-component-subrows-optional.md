# Phase 6: Optional: selected-entity component subrows in the tree

**Status:** NOT STARTED

<!-- Flip to COMPLETED when the Done-when checklist passes validation-clean (engine build, e2e/contract, `bun run check`). Delete this file only after COMPLETED + merged. -->

## Goal

Behind a UI toggle (default off), render the **selected** entity's components as
read-only leaf subrows under its tree row, sourced entirely from the already-fetched
`store.componentsBySelected` — **zero new control calls**. Clicking a subrow keeps the
entity selected and focuses the matching section in the Inspector. This is a pure
navigation affordance: editing always happens in the Inspector, never in the tree.
Full per-entity component subrows (Unreal SCS style) stay **out of scope** — they would
require N `inspect` calls and an `(id, sceneVersion)` cache that fights the version-gated
poll (see phase-0 decision "Components stay in the Inspector (Unity model), not the tree").

Depends on phase 5 (the tree view, `HierarchyTree.tsx`/`TreeRow`, expand-state, and the
flat-list+`parentId` wire shape must already exist). This phase adds no engine, control,
or schema changes — it is editor-only and reuses data the Inspector poll already keeps
fresh.

## Current state

- `componentsBySelected: InspectResult | null` (`editor/src/state/store.ts:29`) holds the
  selected entity's `inspect` result; it is refreshed only for the selected entity by the
  reconcile poll (`store.ts:305`, `setComponentsBySelected(inspected)`) and cleared when
  nothing is selected (`store.ts:295`). `applyOptimisticComponent` (`store.ts:123`)
  overlays edits between polls. This is the zero-cost source for subrows.
- `COMPONENT_ORDER` (`editor/src/panels/InspectorPanel.tsx:36`) is the canonical component
  order (`Name`, `Transform`, `Mesh`, `Camera`, `Material`, `DirectionalLight`,
  `PointLight`, `SpotLight`); `orderedComponentNames` (`InspectorPanel.tsx:56`) applies it
  to a present-component map, appending unknown components in insertion order. The
  Inspector is switch-free — every present component renders via `renderField`, so the
  engine-side `RelationshipComponent`/`WorldTransformComponent` from phases 1-4 surface
  here automatically unless filtered (`NON_REMOVABLE`, `InspectorPanel.tsx:50`, is the
  existing filter-set precedent).
- The tree row component is `TreeRow` in `editor/src/panels/HierarchyTree.tsx` (split out
  from the flat `HierarchyPanel.tsx` in phase 5). `HierarchyPanel.tsx:95` is the flat
  `entities.map` this plan's tree replaces; the selected-row `cn()` classes
  (`HierarchyPanel.tsx:109-114`) and the Radix `ContextMenu` anchored to the sidebar
  (`HierarchyPanel.tsx:20-26`, `:95-133`) are the patterns `TreeRow` reuses.
- Selection is `selectedId` (`store.ts:26`) + optimistic `selectEntity` (`store.ts:119`),
  confirmed by `selectionVersion`/`get-selection`. Expand-state (`expandedIds` /
  `toggleExpanded`, added in phase 5) is plain UI state outside the version-gated poll.
- The Inspector has no programmatic "focus a component section" path today; sections
  render in `orderedComponentNames` order with no per-section anchor or scroll target.

## Implementation

### 1. New store slice: a component-subrow toggle and a focus signal

Add to `EditorState` (`editor/src/state/store.ts:24`, interface near the other UI flags
like `viewportHidden` at `store.ts:49`):

- `showComponentSubrows: boolean` — the toggle, **default `false`** so the outliner stays
  clean. Plain UI state, never gated on `sceneVersion`; persist to `localStorage` keyed by
  project path alongside the phase-5 expand-state policy.
- `toggleComponentSubrows(): void` — flips it.
- `focusComponent: string | null` — a transient "jump the Inspector to this component"
  signal (new field). `setFocusComponent(name: string | null): void` sets it.

Implement in the store body (`store.ts:123` region for setters). On `resetSceneState`
(`store.ts:152`) clear `focusComponent` (leave `showComponentSubrows` as a persisted UI
preference, not scene state). Keep `focusComponent` out of the poll entirely — it is a
one-shot UI intent, consumed and cleared by the Inspector (subsection 4).

### 2. Derive the subrow list from `componentsBySelected` (zero fetches)

In `HierarchyTree.tsx`, add a module-scope helper next to the existing tree builders:

```ts
// Components hidden from subrows: parenting is edited via the tree, not as raw fields,
// and the world matrix is a derived cache. Mirrors InspectorPanel's filter intent.
const SUBROW_HIDDEN = new Set<string>(["RelationshipComponent", "WorldTransform"]);

function subrowNames(components: Record<string, unknown>): string[] {
  return orderedComponentNames(components).filter((n) => !SUBROW_HIDDEN.has(n));
}
```

Reuse the canonical order: import (or re-export) `COMPONENT_ORDER` /
`orderedComponentNames` from `InspectorPanel.tsx:36,56` rather than duplicating the array
— export them from `InspectorPanel.tsx` if not already exported so the subrow labels and
the Inspector sections stay in lockstep. The exact registered names to filter follow the
engine registrations from phases 1-2 (`RelationshipComponent` in
`scene_edit_components.cpp`, `WorldTransform` is unregistered so it never reaches
`inspect` — keep it in the set defensively). Confirm the wire names against a live
`inspect` payload during implementation; filter by the registered JSON key, not a guess.

Subrows are read from `store.componentsBySelected.components` (the `InspectResult` shape,
`store.ts:29`) **only for the selected entity**. No other entity ever triggers an
`inspect`; the poll (`store.ts:305`) is unchanged.

### 3. Render subrows in `TreeRow` behind the toggle

In `TreeRow` (`HierarchyTree.tsx`), after the entity's own row and its child entity rows,
conditionally render the component leaves:

- Gate on `showComponentSubrows && entity.id === selectedId && isExpanded(entity.id)` —
  subrows appear only when the toggle is on, this row is the selected entity, and the row
  is expanded. Non-selected rows render no subrows (and issue no fetch).
- Source the names via `subrowNames(componentsBySelected.components)`; if
  `componentsBySelected` is null or its id does not match `selectedId` (mid-poll race),
  render nothing.
- Each subrow is a non-`EntityRef` leaf: indent one level deeper than child entities
  (reuse the phase-5 depth-indent), no twistie, a muted/italic label distinct from entity
  rows (a `cn()` variant of `HierarchyPanel.tsx:109-114`), `role="option"` but visually
  marked read-only. Key it `` `${entity.id}:${name}` `` so it never collides with an
  `EntityRef` id.
- Click handler: `selectEntity(entity.id)` (keeps the entity selected — it already is) +
  `setFocusComponent(name)`. Do **not** call any control command; selection is unchanged,
  so no `select`/`inspect` round-trip is needed.

Place the toggle control in the Hierarchy panel header (`HierarchyPanel.tsx:84-89`, beside
`CreateMenu`) as a small icon/checkbox bound to `toggleComponentSubrows`. Keep it
sidebar-anchored — no portal or floating layer over the viewport rect (the X11 child
paints on top; this is the same constraint phase-5 honors for drag visuals).

### 4. Focus the Inspector section on subrow click

The `focusComponent` signal drives the Inspector. In `InspectorPanel.tsx`:

- Subscribe to `focusComponent` (`useEditorStore((s) => s.focusComponent)`).
- Give each rendered component section a stable ref/`id` keyed by component name (the
  `names.map` render loop following `orderedComponentNames`, `InspectorPanel.tsx:186`).
- In an effect, when `focusComponent` is set and matches a present section, `scrollIntoView`
  that section (and optionally apply a brief highlight class), then call
  `setFocusComponent(null)` to consume the one-shot. If the component is absent (e.g. the
  selection changed), clear the signal without scrolling.

This is presentation-only: no new component is fetched, no command is issued, and
`selectionVersion` is untouched, so the poll does not re-inspect on a subrow click.

### 5. No engine / control / schema changes

This phase touches no DTO struct in `control_dto.cppm`, no `tools/gen-control-dto/gen.ts`
catalog entry, no `registerCommand`, no `se` formatter, and no generator run (the editor's
`bun run gen:protocol` is a shim over `gen.ts`; nothing here regenerates its five outputs).
The wire contract is exactly what phase 5 shipped (`list-entities` with `parentId`,
`inspect` for the selected entity). The generated `command-manifest.generated.json` /
`openrpc.generated.json`, the `tools/check-control-schema/check.ts` contract test, and the
`se` CLI therefore stay untouched and in sync by construction — call this out in the PR so
a reviewer does not expect schema churn.

## Done when

- [ ] With the toggle **on** and an entity selected and expanded, its components render as
      read-only leaf subrows beneath the entity row, ordered by `COMPONENT_ORDER`
      (`InspectorPanel.tsx:36`).
- [ ] Clicking a subrow leaves the entity selected (no `selectionVersion` change) and
      scrolls/focuses the matching section in the Inspector.
- [ ] `RelationshipComponent` and `WorldTransform` never appear as subrows (filtered by
      `SUBROW_HIDDEN`), matching the Inspector's parenting-is-edited-via-the-tree rule.
- [ ] With the toggle **off** (default), the tree renders the plain entity hierarchy with
      no subrows.
- [ ] No additional `inspect` (or any control) call is issued for non-selected entities:
      the reconcile poll's call pattern is byte-for-byte unchanged. Verify in `tests/e2e`
      by asserting the validation-clean log shows no extra `inspect` per non-selected
      entity, and/or by an editor-side assertion that `client` issues no command on subrow
      render/click beyond the existing selected-entity inspect.
- [ ] Engine + tree behavior is unchanged: round-trip a parented scene through
      `writeScene`/`readScene` and `se reparent` then `se inspect` still pass exactly as in
      phase 5 (this phase touches no engine code, so this is a regression guard).
- [ ] `bun run check` (gen:protocol + `tsc --noEmit`) passes; `bun run lint`/`bun run format`
      clean. The contract test (`tools/check-control-schema`) and `make e2e` stay green
      with no schema diff.

## Risks / seams

- **Must not regress to per-entity inspects.** The single load-bearing constraint: subrows
  are selected-entity-only, sourced from `componentsBySelected` (`store.ts:29`). Any path
  that fetches `inspect` for a non-selected row reintroduces the N-inspect poll pressure
  phase-0 rejected. Guard the render so non-selected rows produce zero subrows and zero
  control traffic.
- **Read-only navigation, not a second edit surface.** Subrows must not expose field
  editors — editing stays in the Inspector to avoid two divergent edit surfaces and the
  coalescer/`dragActive` machinery the Inspector owns (`InspectorPanel.tsx:11-13`).
- **Component-name drift.** `SUBROW_HIDDEN` filters by the engine's registered JSON keys
  (`RelationshipComponent`); if a phase-1/2 rename lands, this set and the equivalent
  Inspector filter must track it. Reuse `orderedComponentNames` rather than a second
  hardcoded order so a new component appears (or is excluded) in one place.
- **Focus signal lifecycle.** `focusComponent` is a one-shot; if the Inspector fails to
  consume and clear it (component absent, selection raced), it must clear without
  scrolling, or a stale signal fires on the next render. Keep it out of the poll and
  out of `sceneVersion` gating.
- **X11 overlay constraint.** The toggle and any subrow hover/active affordance stay in the
  sidebar DOM; no `setDragImage`, portal, or floating layer over the reparented viewport
  rect (same z-order rule phase-5 follows).
- **Expand-state interaction.** Subrows live under the expand state phase-5 already keeps
  outside the version-gated poll; a scene mutation must not collapse the tree or drop
  subrows beyond what an actual selection change implies.
