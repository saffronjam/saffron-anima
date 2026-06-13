# Phase 09 — polish + docs

**Status:** COMPLETED

**Implemented:** the tab context menu (`DockTabContextMenu` — per-destination Move-beside /
split / Close, the no-drag fallback, wired into `TabStrip` for dock tabs); the dead-code sweep
(the legacy `BottomTab`/`RightTool`/`BottomTool` slices, their actions, the label maps, and the
`RightSidebar`/`BottomDock`/`LeftBottomTabs` Radix markup were all already removed in phases
03/04/07 — verified zero remaining references) plus the refreshed `App.tsx` header (per-kind
dock-island wording, the `Stats` mislabel fixed to Inspector/Environment/Render; `Layout.tsx`
was rewritten in phase 07); and the docs page (`dock-system.md` + the `ui-and-editor` hub row,
with `theme-and-fonts` reconciled to point at it). The docs site builds (`hugo`, rc 0).

**Deferred (the plan's optional / measure-first items):** the tab-overflow scroll + chevron
upgrade (item 2) — the phase-04 `min-w-0` shrink + truncate already degrades legibly and keeps
the centers-snapshot math valid; adding scroll would complicate that math and reads as a
refinement, not a correctness need. The optional group-level FLIP on structural drops (item 3)
and the per-neighbor-width reorder refinement (item 4) are explicitly "take only if it reads
well / measure first" — left for a pass with live visual feedback.

## Goal

Close the UX checklist items deferred from earlier phases, delete what died along the way,
and write the docs page the keep-current rule owes for a new editor concept.

Each main tab is its own dockspace island: the wanted moves are vertical↔horizontal within
one tab (Scene's `scene` tree, the asset editor's `assetEditor` tree), and cross-main-tab
moves are forbidden by construction (disjoint per-kind `DockPanelId` spaces + an
active-island-only `[data-dock-leaf]` registry). Every item below is scoped accordingly.

## What exists to build on

- The UX research checklist (README): keyboard/menu fallback for every drag (VS Code
  "Move View", JetBrains "Move to"), tab overflow (JetBrains one-row + chevron), tab
  context menus (Unity/Unreal).
- The phase-04 overflow guard (shrink + truncate) — legible but unbounded below usable
  widths with many tabs.
- `docs/` conventions: one concept per page, front-matter title = H1, slim
  `What | File | Symbols` table, mermaid for diagrams; hub `_index.md` row in the same
  change (root `AGENTS.md`).

## Work

1. **Tab context menu** (the existing shadcn `ContextMenu` primitive,
   `components/ui/context-menu.tsx` — `ContextMenuTrigger asChild` around the tab; dock
   tabs only): "Move to…" with a destination picker listing every non-locked leaf (plus
   "new split left/right/top/bottom of …") driven by `movePanel` — the
   no-drag/keyboard-accessible fallback for every drag operation — and "Close" for
   closable panels.
2. **Tab overflow upgrade:** when shrink hits the minimum legible width, the strip becomes
   horizontally scrollable (wheel) with a chevron menu listing off-screen tabs (JetBrains
   pattern). The drag machine's centers snapshot must account for scroll offset — snapshot
   in viewport coordinates and re-snapshot on scroll during reorder.
3. **Optional group-level FLIP** on structural drops (record every leaf rect pre-mutation,
   invert + play at group granularity — same technique as the tab settle). Take it only if
   it reads well; the snap-with-overlay-preview is already acceptable.
4. **Per-neighbor-width reorder refinement** (the inherited mixed-width inaccuracy): shift
   each displaced neighbor by the *dragged* tab's width but compute insertion against each
   neighbor's own center — revisit `insertionIndexForPointer` with per-neighbor widths if
   compact strips feel off. Measure first; skip if imperceptible.
5. **Dead code sweep:** anything the per-kind dock cutover stranded. The three legacy
   slice types and everything keyed off them: `BottomTab` (`store.ts:43`), `RightTool`
   (`store.ts:46`), `BottomTool` (`store.ts:50`) plus their store fields/actions (the
   `setBottomTab` action `store.ts:462`, the `openRightTool`/`closeRightTool`/
   `setActiveRightTool` and `openBottomTool`/`closeBottomTool`/`setActiveBottomTool` trios);
   both strip label maps (`RightSidebar.tsx` `TOOL_LABEL:12`, `BottomDock.tsx`
   `BOTTOM_TOOL_LABEL:9`); and in `Layout.tsx` the Radix `Tabs` import (`:25`), the
   `type BottomTab` import (`:43`), and the `LeftBottomTabs` Radix usage (`:254-295`) the
   `DockRoot` swap replaced. Verify against the CURRENT WORKING TREE, not HEAD: the
   asset-editor files (`AssetEditorWorkspace.tsx`, `SkeletonTree.tsx`, `ClipList.tsx`) are
   present and the `Rig*` panels are deleted, so the sweep must not chase the gone files.
   Also refresh the stale-region vocabulary in the header comment blocks — `App.tsx:1-8`
   and `Layout.tsx:1-21` — to the per-kind dock-island wording. PRESERVE the two true notes
   both already carry (asset tabs park the Scene dock via display:none, and the dock remounts
   on the per-project key), and fix the mislabel in `App.tsx:4` that calls the tabbed
   left-bottom group "Stats" — those tabs are Inspector/Environment/Render.
6. **Docs page** under `docs/content/` (explanation section): the dock system — the
   per-kind tree model (`dockLayouts: Record<DockSpaceKind, DockLayout>` keyed
   `scene | assetEditor`), per-main-tab dockspace isolation (the 5-kind `ViewTab` union
   `scene | flamegraph | materialGraph | assetEditor | imageViewer`, disjoint `DockPanelId`
   spaces, active-island-only `[data-dock-leaf]` registry), the asset editor as a first-class
   dock island (phase-10 — SkeletonTree/locked preview/ClipList + the bottom timeline via the
   shared `components/timeline/` surface), the drag domains, the portal host
   (`DockPanelsHost`), the locked live-subsurface leaves (Scene `viewport` + asset-editor
   `preview`), and persistence under the per-project key. Default to a NEW sibling explanation
   page; cross-link the existing `theme-and-fonts` "resizable dock" mention rather than
   deleting it. Add its hub row to `ui-and-editor/_index.md` using that hub's
   `| Page | Covers | Code |` table with `·`-separated symbols, and reconcile it with the
   `theme-and-fonts` (`styles.css` · `Layout.tsx`) and `asset-editor`
   (`AssetEditorWorkspace.tsx`) rows so the dock story reads as one. Run the humanizer pass
   per the docs conventions.

## Verify

- `bun test` + `bun run check`; `make prepare-for-commit` clean; full `make check` once at
  plan completion. (`bun test` presupposes the test harness phase-03 introduces — the
  `"test": "bun test"` script + the `check.sh` test step; it does not exist before then.)
- Manual via `make run`:
  - Move every panel everywhere using only the context menu (no drag) — including to a new
    split; close + reopen via the Panels menu.
  - Overflow: 5+ tabs in the right strip — scroll, chevron menu, reorder across the scroll
    boundary all behave.
  - The docs site builds (`cd docs && hugo`) and the new page renders with a working hub
    link.
  - The docs page covers the asset editor as a dock island (phase-10): per-kind
    `dockLayouts`, the asset-editor `DockPanelId`s (skeleton/preview/clips/assetTimeline),
    and per-main-tab isolation — and its hub row reconciles with the existing `asset-editor`
    and `theme-and-fonts` "resizable dock" rows.
- Mark the plan `COMPLETED` in `README.md` when this phase lands.
