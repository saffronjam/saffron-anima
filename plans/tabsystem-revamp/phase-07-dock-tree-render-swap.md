# Phase 07 — DockRoot: render the tree

**Status:** COMPLETED

## Goal

Swap `Layout`'s hard-coded region composition for a recursive `DockRoot` that renders a
`DockLayout` tree: branch → `ResizablePanelGroup` + `ResizableHandle`s, leaf →
`TabStrip size="dock"` + host-claiming body. Hierarchy, Assets, and the Viewport join the
tree as leaves; the interim `persistent` flags drop; the left trio unpins. The largest
phase, deliberately de-risked by 01 (rrp spike), 05 (hosts), and 06 (drag) — it changes
*rendering*, not the model, the drag layer, or panel content.

`DockRoot` is **parameterized by dockspace kind** from the start. This phase renders the
**Scene** tree only (`DockRoot space="scene"` driving `dockLayouts["scene"]`), proving the
machinery on the larger island first. The asset-editor island
(`AssetEditorWorkspace.tsx`) is migrated onto the SAME `DockRoot` in **phase-10** — it is a
first-class island, not out of scope. The per-kind `dockLayouts` model, both disjoint
`DockPanelId` unions, and the assetEditor default factory already ship from phase-03, so
phase-10 is a render swap, not a model change.

## Per-main-tab scoping

Each dockspace-bearing main tab is its own independent island. Scene panels
(`inspector, environment, render, stats, profiler, material, timeline, hierarchy, assets,
viewport`) and asset-editor panels (`skeleton, preview, clips, assetTimeline`) are two
**disjoint** `DockPanelId` unions (phase-03), so a Scene panel id can never resolve into the
asset-editor tree and vice-versa. Everything `DockRoot` touches here is confined to the
active island: `data-dock-leaf` targets are tagged only on this `DockRoot`'s leaves,
`movePanel`'s reachable leaves are the Scene tree's, and the `openPanel` terminal fallback
appends to the Scene root. Only one main tab is mounted at a time, so at most one dockspace
is active and only its leaves are drop/merge/split targets. Cross-main-tab moves are
unexpressible by construction — disjoint id types + active-island-only registry — never a
runtime guard.

## What exists to build on

- `app/Layout.tsx` today: pixel-width `aside` sidebars with hand-rolled resizers, two rrp
  groups (`useDefaultLayout` ids `saffron.layout.left/right:`, persistence wiring `:72-81`),
  conditional right/bottom regions, `clampSidebarWidth` against `VIEWPORT_MIN_WIDTH = 520`
  (`:48`). Default percents: hierarchy `45` (`:164`), left-tabs `55` (`:173`), viewport `72`
  (`:194`), assets `28` (`:200`). Pixel widths `SIDEBAR_DEFAULT_WIDTH = 280` (`:45`),
  `RIGHT_SIDEBAR_DEFAULT_WIDTH = 320` (`:49`).
- The phase-01 spike's verdict on dynamic rrp structure + the chosen reconciliation
  mechanism (imperative `GroupImperativeHandle.setLayout` via `useGroupRef`, or keyed remount
  by structure hash + rAF-force settle).
- The viewport contract: exactly one `viewport-host` div as the rect source
  (`ViewportPanel.tsx:525`) — and it is the rect source **only within the Scene dockspace**;
  the asset-editor presents its own subsurface preview, so "exactly one viewport-host" is true
  per-island, not app-wide. Its ResizeObserver + the debounced end tier handle splitter drags.
- `viewportHidden` stays out of the dock system's hands. Its driver is the
  `subsurfaceVisible` effect (`App.tsx:198-205`), where `subsurfaceVisible = sceneTabActive ||
  activeKind === "assetEditor"` (`App.tsx:78`) — BOTH islands keep the subsurface live. The
  modal park paths (`ProjectStartupModal`, the asset View modal) also toggle it; the dock swap
  must not add a new driver.
- Dead code to retire: `persistBottomDockHeight`/`loadBottomDockHeight`
  (`store.ts:1141-1168`, no callers); the sidebar-width helpers `persistSidebarWidth`/
  `loadSidebarWidth`/`persistRightSidebarWidth`/`loadRightSidebarWidth` (`store.ts:1077-1136`),
  still CALLED from `Layout.tsx`, so they die only when the pixel sidebars die — in this phase.

## Work

1. **`components/dock/DockRoot.tsx`** — recursive and dockspace-parameterized (`space:
   DockSpaceKind`, reading `dockLayouts[space]`): a `DockBranch` renders a
   `ResizablePanelGroup` (orientation from the node) whose children render in
   `ResizablePanel`s sized from `branch.sizes`, `onLayoutChanged` → `setBranchSizes` →
   (debounced) persist + `emitLayoutSettled()`; a `DockLeaf` renders the strip + the
   host-claiming body (locked leaf: strip hidden). Each leaf wrapper carries
   `data-dock-leaf="<leafId>"` — the phase-06 attribute moves off `Layout`'s fixed slots, or
   the drag layer silently loses every target. The attribute is applied only on the mounted
   island's leaves, so an inactive island never enters the `dockDrag` snapshot. The Scene tab
   mounts `<DockRoot space="scene" />`, replacing `Layout`'s hard-coded composition.
2. **The Scene default tree** reproduces today's Scene layout exactly: root horizontal →
   [left vertical: hierarchy / leftBottom-trio leaf] · [center vertical: viewport / assets
   (/ bottom when occupied)] · [right when occupied]. Default percents match today's
   (`72/28` center `:194/:200`, `45/55` left `:164/:173`); the root branch converts today's
   pixel sidebars (`SIDEBAR_DEFAULT_WIDTH` 280 left / `RIGHT_SIDEBAR_DEFAULT_WIDTH` 320 right,
   `:45/:49`) to percents at first mount against the window width, with px `minSize` doing the
   real guarding.
3. **Viewport leaf:** `locked` — non-closable, tab not draggable, no strip chrome, no merge
   drops; px `minSize` 520 replaces `VIEWPORT_MIN_WIDTH` (`Layout.tsx:48`); px minimums on the
   sidebar leaves preserve the cannot-collapse-while-attaching guarantee (spiked at 1280×720
   in phase 01). The Scene viewport leaf keeps the subsurface LIVE — it never parks while Scene
   is active. Edge splits *beside* the viewport remain legal (phase 08) — they insert siblings,
   never occlude.
4. **Unpin the trio:** Inspector/Environment/Render become draggable (`closable: false`
   stands — a closed-nowhere structural panel cannot exist), and the interim
   `acceptsTabs: false` on `leaf:leftBottom` is lifted — the trio's leaf becomes a normal
   merge target. The `openPanel` terminal fallback (append a leaf to the Scene root) becomes
   reachable — exercise it in the bun tests.
   **Empty-region collapse — implemented choice (deviation from the draft):** rather than
   dropping `persistent` and adding recreate-on-drop, the three well-known leaves
   (`leaf:leftBottom`/`leaf:right`/`leaf:bottom`) STAY `persistent` and `DockRoot` simply
   does not render an empty non-locked leaf (skips it from its parent's `ResizablePanelGroup`
   children, renormalizing the rendered sizes). The UX is identical — an empty right/bottom
   region collapses and the viewport reclaims the space — but the reveal-band and
   `openPanel`/`movePanel` targets never dangle (the persistent leaves always exist in the
   model), so no recreate-on-drop model code is needed and the mechanism is the same one
   phases 03–06 already use (emptiness drives rendering). `normalize` still collapses
   non-persistent emptied leaves (e.g. `leaf:hierarchy` after Hierarchy is tabbed elsewhere).
5. **Bottom-dock timeline leaf:** the Scene `timeline` `DockPanelId` maps to
   `panels/TimelinePanel` (`showClipSelect=true`), the 45-line wrapper that composes the
   extracted shared `components/timeline/{TimelineTransport,TimelineSurface}` and builds a
   `TimelineTarget` from the scene selection. This is the Scene bottom-dock canvas; the
   asset-editor's `assetTimeline` leaf (phase-10) hosts `TimelineTransport`+`TimelineSurface`
   directly (`showClipSelect=false`). The two canvases are distinct instances that never
   co-exist. `SkeletonTree`/`ClipList` are asset-editor `DockPanelId`s (phase-10), NOT in the
   Scene union and NOT in the Scene Panels menu.
6. **Panels menu:** a registry-driven section in the Topbar wrench menu (the
   `openBottomTool`/`openRightTool` items at `panels/Topbar.tsx:291`/`:306-312`) — one
   `openPanel(id)` item per closable Scene panel — plus **Reset layout** → `resetDockLayout()`
   + rAF-force settle. Every panel now has a no-drag reopen path; flipping a structural panel
   to `closable: true` later is a one-field registry change. The menu lists only the active
   island's panels (Scene here).
7. **Retire** the pixel-sidebar resizers and the four width-persistence helpers
   (`store.ts:1077-1136`), the rrp `useDefaultLayout` ids, and the dead bottom-dock-height
   helpers (`store.ts:1141-1168`); sizes now live solely in `DockBranch.sizes` under the
   phase-06 per-project key.
8. **Cutover (NO-COMPAT).** This phase depends on 03/05/06. The superseded `BottomTab` /
   `RightTool` / `BottomTool` store slices and EVERY caller are retired and rewritten onto
   `openPanel` in the same change — no second code path survives:
   - `Topbar.tsx:291` `openBottomTool("timeline")` and `:306-312`
     `openRightTool(stats/profiler/material)` → `openPanel(...)`.
   - `AlarmBadge.tsx:31` `openRightTool(...)` → `openPanel(...)`.
   - `HierarchyTree.tsx:503` `setBottomTab("inspector")` → `openPanel("inspector")`.
   Delete the old slices, the lone `setBottomTab` (`store.ts:462`), and the unreachable strip
   plumbing they fed.

## Verify

- `bun test` (terminal fallback, normalize-collapse, recreate-on-drop reveal-band paths) +
  `bun run check`; `make prepare-for-commit` clean.
- Manual via `make run`, the full viewport checklist:
  - Split-drag every handle around the viewport: subsurface tracks live, exact-glues on
    release; no per-tick engine resizes (watch the log).
  - **vertical↔horizontal move (the whole point):** drag a tab from a vertical side leaf
    (e.g. Inspector) into the horizontal bottom dock and back — it merges into the target
    strip; its emptied source leaf collapses (`normalize`). Drag Material beside the Timeline
    in the bottom dock — the two sit side-by-side in one horizontal leaf.
  - **cross-main-tab move is forbidden:** while a tab is torn in the Scene island, no
    asset-editor leaf is ever a drop target (it is unmounted and untagged); switching to a
    non-Scene main tab shows zero Scene `[data-dock-leaf]` targets — the move is unexpressible,
    not blocked by a check.
  - Reopen Inspector via the Hierarchy deep-link (`openPanel`) → it lands at `lastLocation`.
  - Open/close right and bottom regions via drops and closes; project reload; 1280×720
    window: viewport never collapses below its 520 px min, sidebars hold their px minimums.
  - Persisted round-trip incl. a hand-reorganized Scene tree; a tree with stale
    `lastLocation` entries loads with them pruned (`validate`).
  - Viewport park across all five `ViewTab` kinds: **`scene` and `assetEditor` keep the
    subsurface LIVE** (`subsurfaceVisible`, `App.tsx:78`); only `flamegraph`, `materialGraph`,
    and `imageViewer` park it. Returning to Scene restores it (the Scene dock is
    display:none-not-unmounted, `App.tsx:222-230`; the asset-editor is conditional-render).
