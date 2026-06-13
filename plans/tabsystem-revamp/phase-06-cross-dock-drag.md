# Phase 06 — cross-dock tabbed drag (the checkpoint)

**Status:** COMPLETED

## Goal

Drag a tab out of one strip and drop it into another — across today's three fixed sites
**within the Scene main tab**, while they are still composed by `Layout`. Tear-out, cursor
ghost, drop-zone overlay, tab-merge drops, empty-region reveal targets, and the dock
persistence key. **Checkpoint: the Material panel (`MaterialEditorPanel`, the `material`
`DockPanelId`) tabs in beside the Timeline in the bottom dock, and the arrangement survives
a reload.** The bottom-dock Timeline leaf is now `panels/TimelinePanel` composing the shared
`components/timeline/{TimelineSurface,TimelineTransport}`; its `DockPanelId` stays `timeline`.
This vertical↔horizontal move (a side-dock tab into the horizontal bottom dock) is exactly
the wanted interaction. Requirement 2's tabbed-together reading lands here; splits are
phase 08. The asset-editor island gets the same drag layer in phase 10 — this phase proves
the machinery on Scene first.

## What exists to build on

- `useTabStripDrag` with the dormant `onTearOut` option (phase 02); `movePanel` +
  `DropTarget` (phase 03); `DockPanelsHost` so the moved panel keeps state (phase 05).
- Platform findings (README): hit-testing must be manual — `setPointerCapture` retargets
  all pointer events to the source tab, so candidate targets never see `pointerover`
  (w3c/pointerevents#566); `document.elementFromPoint` works under capture (phase 01
  spike); the ghost/overlay must be `pointer-events: none`.
- Zone math conventions (README research): strip rect → insert-at-index; body center →
  merge with a 100% translucent fill; tear threshold ≈ one strip height with hysteresis
  (Chrome tab strip); overlay only appears over a hovered group, never at drag start
  (VS Code `editorDropTarget.ts`).
- `layoutBus.ts`'s outside-Zustand module pattern — the model for the drag registry.

## Work

### 1. Tag the dock sites

The three site containers get `data-dock-leaf="<leafId>"` while still inside `Layout`'s
fixed slots — these are **Scene-island leaf ids**, applied only inside the Scene main tab's
`DockRoot`/`Layout`, never on the titlebar and never on another main tab's leaves. **This is
the no-cross-main-tab boundary:** the titlebar carries no `[data-dock-leaf]`, so a torn dock
tab over the main top bar finds no target — no overlay, no-drop cursor, drop cancels.
Cross-main-tab docking is **structurally unexpressible**, conditioned on three facts together:
the titlebar is never tagged; the two dockspace kinds have **disjoint `DockPanelId` types**
(`scene` vs `assetEditor`, defined in phase 03), so a Scene panel id can never resolve into
the asset-editor tree; and the registry is built only from the active island's leaves (§2).
The asset-editor island (`AssetEditorWorkspace.tsx`) gets the same drag layer in phase 10,
with its own scoped registry over its own leaves.

### 2. `components/dock/dockDrag.ts` — the drag-scoped registry

Module-scope, mirroring `layoutBus`: on tear-out, snapshot every **currently-mounted**
`[data-dock-leaf]` into `{ leafId, bodyRect, stripRect, stripCenters, acceptsTabs,
acceptsSplits }`. Because only one main tab is mounted at a time, this snapshots only the
**active dockspace's** leaves — an inactive island's leaves are unmounted and never enter
the registry, so a cross-main-tab tear has no candidate target. Re-measure on
`window.resize`; **registry geometry exists only while a drag is torn** (built on tear-out,
discarded on drop/cancel). Hit-testing per `pointermove`: point-in-rect against the
registry, with `document.elementFromPoint(x, y)?.closest('[data-dock-leaf]')` as the
fallback/cross-check.

### 3. Tear-out lifecycle (extends the hook's state machine)

armed → (4 px) → reorder → escape when the pointer leaves the strip rect vertically by
more than ~one strip height (32 px, with 8 px hysteresis) → **torn**:

1. Reorder preview clears (neighbors FLIP back); the source tab renders ghosted in place —
   **no store mutation until drop**, so every cancel path is free.
2. A fixed-position mini-tab ghost (`pointer-events: none`, `bg-card border-border
   shadow-lg`) follows the cursor at app root — DOM composites fine over the viewport hole.
3. Re-entering any registered strip's rect reverts to **reorder** in that strip
   (re-snapshot its centers); neighbors part via the same transform preview.
4. **Drop:** exactly one store mutation — `movePanel(id, target)`; the landing strip's FLIP
   settles the tab under the cursor; the settle subscriber re-glues the viewport.
5. **Cancel** (`Escape`, `pointercancel`, `lostpointercapture`, released over no target):
   ghost FLIPs home; nothing was mutated.

**The cross-instance channel.** The drag state, tab refs, and `styleFor` transforms live in
the *source* strip's `useTabStripDrag` instance — a foreign strip's hook is idle and cannot
render the parting preview itself. `dockDrag.ts` is the channel: while torn, it holds the
drag as a subscribable value (`{ panelId, ghostWidth, hovered: { leafId, index } | null }`,
same module-scope pattern as `layoutBus`). Every `TabStrip` subscribes; when the hovered
`leafId` is its own, it renders the parting `translateX(±ghostWidth)` transforms for a tab
it does not own — purely visual, **no store write**, so cancel stays free. Re-entering the
*source* strip is the exception: it falls back to the hook's native in-strip reorder state.
On drop, the destination strip detects the newly inserted tab and runs the WAAPI settle
from a "before" rect derived from the ghost's final position — not from `settleRef`, which
only covers tabs the strip owned pre-drop.

### 4. `components/dock/DockDropOverlay.tsx`

One overlay div, repositioned per hovered leaf (VS Code's single-overlay pattern):
`top/left/width/height` CSS-transitioned ~120 ms, `bg-primary/15 ring-1 ring-primary`,
always `pointer-events: none`. This phase renders two zone kinds: pointer in the strip
rect → insertion caret at the computed index; pointer in the body → tab-merge, overlay
fills the leaf 100%. A body merge commits
`movePanel(id, { kind: "tab", leafId, index: leaf.tabs.length })` (append), and `movePanel`
always makes the moved panel the destination leaf's `activeTab`. Self-drops that would
no-op are suppressed (no overlay, drop cancels). Locked leaves (`acceptsTabs: false`)
never render a merge zone.

### 5. Empty-region reveal targets

While `leaf:right` / `leaf:bottom` are empty, their regions are unmounted — so during a
torn drag only, thin edge bands appear (right edge of the window, bottom of the center
column) as drop targets for the **well-known persistent leaf ids, which always exist in
the model** (phase-03 contract). A drop inserts the tab; emptiness flipping remounts the
region. Both sides covered; no target can dangle. (Phase 07 drops the `persistent` flags
and replaces this guarantee with recreate-on-drop — see there.)

### 6. Site-pinning and the trio

The Inspector/Environment/Render trio stays `isDraggable: false` (site-pinned) until
phase 07 — their leaf participates as a *target* for merges only if desired; simplest is
`acceptsTabs: false` on `leaf:leftBottom` for now, revisited in phase 07.

### 7. Persistence

`saffron.layout.dock:<projectPath>` → `{ version: 1, layout: DockLayout, lastLocation }`,
written debounced ~300 ms on any dock mutation, loaded on `Layout` mount (the
`<Layout key={projectPath ?? ""}/>` remount, `App.tsx:229`, comment :222-226 / dock div
:227-230, keeps per-project semantics). Load runs `validate` (incl. `lastLocation` pruning);
failure ⇒ default factory. **No-op while `projectPath` is undefined** (session-only before a
project loads — matching `persistSidebarWidth`, `store.ts:1077-1086`, siblings :1109-1118 /
:1141-1168). `movePanel`/`closePanel` update
`lastLocation[id]` so a reopened panel returns where it last lived (Unreal convention).
Accepted behavior change: open panels now persist (a persisted-open Stats resumes metrics
polling at launch).

## Verify

- `bun test` (registry zone math is pure → unit-test it too) + `bun run check`;
  `make prepare-for-commit` clean.
- Manual matrix via `make run`:
  - **Material (`MaterialEditorPanel`) → bottom dock beside Timeline (`TimelinePanel` on the
    shared `components/timeline/*`): tabbed together, state intact (no preview re-fetch),
    survives app restart.** The Timeline leaf's old unmount-inactive policy is now the phase-05
    renderer policy, so it keeps state through the move.
  - Every closable tool moves right↔bottom and back; in-strip reorder still works mid-drag
    (re-entry); each cancel path restores perfectly — Escape, drop on nothing, and capture
    loss (switch workspace mid-drag; a mouse never fires `pointercancel` naturally).
  - Empty-region bands: with the bottom dock closed, dragging a tool to the bottom band
    opens it with the tab; same for the right side.
  - The titlebar is never a target: overlay never appears there, drop over it cancels;
    no leaf from another main tab is ever a drop target (only the active island's leaves
    are in the registry).
  - Last-tab-leaves: source region empties and unmounts (persistent leaf stays in the
    model); viewport reclaims the space and re-glues.
  - Reload round-trip: arrangement + active tabs restored; a corrupted/stale key falls
    back to the default layout.
