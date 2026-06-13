# Phase 08 — split drops

**Status:** COMPLETED

## Goal

Enable the `{ kind: "split" }` half of `DropTarget`: dropping a torn tab on a leaf's edge
band splits that leaf and docks the panel beside it. **Requirement 2's strong reading —
the Material panel literally side-by-side with the Timeline — lands here.**

## What exists to build on

- `splitLeaf` already exists and is unit-tested in the pure model (phase 03); `normalize`
  already merges same-orientation nesting and collapses single-child branches.
- The drag layer (phase 06) already computes zones per hovered leaf and renders the single
  transitioned overlay; `acceptsSplits` is already in the registry snapshot.
- `DockRoot` (phase 07) renders arbitrary trees, so a freshly split tree renders without
  new code; the settle subscriber covers the mutation.
- Each main tab renders its own dock tree and only the active island carries
  `[data-dock-leaf]`, so the split registry is empty on every non-active island and a split
  can never cross main tabs — disjoint per-kind `DockPanelId` spaces make it unexpressible.
- The centralized settle subscriber only re-glues the active island's subsurface on a split;
  an inactive island's preview host sits at 0×0 and is untouched (degenerate rects skipped).
- Zone-math conventions (README research, VS Code `editorDropTarget.ts`): edge bands at
  1/3 of the leaf extent pick the split direction; the overlay previews the *resulting*
  region by filling 50% of the leaf on that side; center remains merge (100%).

## Work

1. **Zone math:** extend the per-leaf hit-testing — outer-third edge bands on leaves with
   `acceptsSplits` produce `{ kind: "split", leafId, edge }`; the overlay fills the
   corresponding 50%. Merge (center) and strip-insert behavior unchanged.
2. **Commit:** the drop calls `movePanel(id, { kind: "split", … })` → `splitLeaf`. The new
   sibling starts at 50% of the split leaf (VS Code behavior); cross-leaf structural drops
   **snap** — the transitioned overlay already previewed the destination; group-level FLIP
   stays a phase-09 polish option.
3. **Viewport rules:** the Scene dock tree's locked viewport leaf accepts no merges
   (already) but its *siblings* may split; splitting against that viewport leaf's own edges
   inserts a sibling beside it in the parent branch — never over it. Respect px minimums: a
   split that would violate the Scene viewport's 520 px min (`Layout.tsx:48`) or a sidebar
   min is an invalid target — no zone renders (Unreal convention: illegal zones never
   appear). The asset-editor island has its OWN locked `preview` leaf (minSize 30) with its
   own splits in phase-10; Scene and asset-editor leaves never mix, so these rules apply only
   within the Scene tree.
4. **Empty-band splits:** the phase-06 window-edge reveal bands keep meaning "dock into
   that region as a tab" — recreating the well-known leaf if it was collapsed (the
   phase-07 recreate-on-drop semantic); no split semantics there.

## Verify

- `bun test` (split + normalize round-trips: split, move the tab back out, tree collapses
  to the original shape) + `bun run check`; `make prepare-for-commit` clean.
- Manual via `make run`:
  - **Material dropped on the Timeline leaf's right edge: a horizontal split inside the
    bottom region, both visible side-by-side; state intact; ratios drag and persist;
    survives reload.** (The Timeline leaf here is the Scene bottom-dock leaf hosting
    `panels/TimelinePanel`, which wraps the shared
    `components/timeline/{TimelineTransport,TimelineSurface}` — NOT the asset editor's
    `assetTimeline` leaf, which docks in its own island in phase-10.)
  - Split each edge of a normal leaf; verify the 50% overlay preview matches the result.
  - Splits adjacent to the viewport: viewport re-glues, min widths hold, no occlusion.
  - Tear the split-off panel back out: the split collapses (`normalize`), neighbor
    reclaims space, viewport re-glues.
  - No zone renders where a split would violate minimums (shrink the window to force it).
  - On a non-Scene main tab, no split zones appear during a torn drag — the inactive
    island's leaves never enter the registry.
