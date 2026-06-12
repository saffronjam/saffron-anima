# Phase 04 — dock-variant strips everywhere

**Status:** NOT STARTED

## Goal

Replace the three hand-rolled strips with `TabStrip size="dock"` driven by the phase-03
slice. Requirements 1 and 3 land for the small strips: identical drag mechanics at dock
size, including in-strip reorder — which none of the three strips has today. **Strip-only
diff**: each site keeps its current content-rendering policy until phase 05 unifies it.

## What exists to build on

- `TabStrip` + `useTabStripDrag` (phase 02), already proven on the titlebar.
- The three strips:
  - `LeftBottomTabs` (`app/Layout.tsx:254-295`) — Radix `Tabs`/`TabsList`/`TabsContent`;
    content unmounts when inactive (Radix default).
  - `RightSidebar` (`panels/RightSidebar.tsx:28-69` strip, `:70-90` content) — keeps every
    open tool mounted, hidden via `display:none`, so Material's preview survives switches.
  - `BottomDock` (`panels/BottomDock.tsx:21-62` strip, `:63` content) — unmounts inactive.
- The phase-03 slice: strips read `leaf.tabs`/`leaf.activeTab`, call `activatePanel`,
  `closePanel`, `reorderTab`.
- Titles/icons: `TOOL_LABEL` (`RightSidebar.tsx:12-16`), `BOTTOM_TOOL_LABEL`
  (`BottomDock.tsx:9`) — fold into one shared map (the phase-05 registry will absorb it).

## Work

1. **`LeftBottomTabs`**: keep the Radix `TabsContent` bodies (or equivalent conditional
   render — same unmount-inactive policy), but render the strip as
   `TabStrip size="dock"` with `drag: { domain: "dock", isDraggable: () => false }` for now
   (the trio is site-pinned until phase 07) and `closable: false` items. Tab activation
   calls `activatePanel` (strip tabs are open by definition — phase 03 reserves `openPanel`
   for deep-links/Topbar/menus); the central subscriber covers the rAF settle.
2. **`RightSidebar`**: strip → `TabStrip size="dock"`, items closable, `onReorder` →
   `reorderTab("leaf:right", …)`. Content: unchanged keep-mounted `display:none` blocks.
3. **`BottomDock`**: same, against `leaf:bottom`; content stays unmount-inactive.
4. **Overflow guard**: the `dock` variant's `min-w-0` shrink + truncate (phase 02) is what
   makes an overfull strip (e.g. four tools dragged into the ~320 px right sidebar later)
   degrade legibly without clipping out of the centers math. Verify it here with many tabs.
5. Delete the now-dead hand-rolled strip markup and the local label maps if fully absorbed.

## Verify

- `bun test` + `cd editor && bun run check`; `make prepare-for-commit` clean.
- Manual via `make run`, per strip:
  - Activate/close parity with before; close-fallback to the index−1 neighbor.
  - **In-strip reorder now works** in the right sidebar and bottom dock (≥2 tabs open):
    4 px latch, neighbor preview, FLIP settle — indistinguishable from the titlebar feel.
  - Left trio: not draggable (site-pinned), switching tabs still re-glues the viewport.
  - Material panel still keeps selected material + preview across right-sidebar tab
    switches (the `display:none` policy is untouched).
  - Overfull strip: tabs shrink + truncate, no clipping, reorder still lands correctly.
