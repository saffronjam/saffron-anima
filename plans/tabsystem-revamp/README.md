# Tab-system revamp — dockable panel tabs

**Status:** COMPLETED

Make the editor's panel tabs first-class: one reusable tab-strip component with the *exact*
drag feel of the main view tabs (smaller), and a docking layer so any tool panel can be
dragged between dock areas — e.g. the Material panel tabbed or split beside the Timeline.
Grounded in the tree as of June 2026; reference by symbol name first. Line numbers are a
secondary aid and have drifted since the original draft — every `WindowTitlebar.tsx`
reference is **+1** (no leading blank line) and `state/store.ts` numbers drifted ~+5..+220 as
the store grew; the citations below are refreshed against the current tree.

## Where this starts from

The titlebar's main view tabs (`editor/src/app/WindowTitlebar.tsx`) are the prime example
of the wanted interaction: pointer-capture drag with a 4 px threshold
(`TAB_DRAG_THRESHOLD_PX`, `:23`), a live reorder preview that translates neighbors by the
dragged tab's width (`tabStyle`, `:269-292`), insertion index from snapshotted tab centers
(`insertionIndexForPointer`, `:171-187`, scene pinned via `if (id === "scene") continue` at
`:175`), and a WAAPI FLIP settle on drop (`useLayoutEffect`, `:131-161`). Meanwhile the editor
has **three hand-rolled, non-draggable strips** that share none of it:

- `LeftBottomTabs` (`app/Layout.tsx:254-295`) — Inspector/Environment/Render on Radix `Tabs`.
- `RightSidebar` (`panels/RightSidebar.tsx:28-69`) — Stats/Profiler/Material, closeable, keeps
  inactive panels mounted via `display:none` (`:70-90`) so Material's preview survives.
- `BottomDock` (`panels/BottomDock.tsx:21-62`) — Timeline, closeable, unmounts inactive.

Backing state is three parallel store slices (`state/store.ts`): `bottomTab` (type `:43`,
field `:100`, lone `setBottomTab` `:462`), `rightTools`/`activeRightTool` (type `RightTool`
`:46`, fields `:120-121`, trio `openRightTool`/`closeRightTool`/`setActiveRightTool`
`:751-770`), `bottomTools`/`activeBottomTool` (type `BottomTool` `:50`, fields `:124-125`,
trio `openBottomTool`/`closeBottomTool`/`setActiveBottomTool` `:771-790`) — the two trios
share an index-1 close-fallback (`:765`, `:785`).

These strips host current-named panels: `BottomDock` mounts `panels/TimelinePanel.tsx` (a
~45-line wrapper, `showClipSelect=true`) over the extracted `components/timeline/*`; the
Material right-dock panel is `panels/MaterialEditorPanel.tsx` (the `material` `RightTool` —
the "Material tabbed/split beside the Timeline" goal), distinct from the whole-tab
`MaterialGraphEditor.tsx` `materialGraph` view, which is never docked. The `ViewTab` union
(`state/store.ts`) is now five kinds — `scene | flamegraph | materialGraph | assetEditor |
imageViewer` — and `tabIcon` (`WindowTitlebar.tsx:426-449`) branches on `assetEditor`→Box plus
the image-viewer `assetType` icons.

### Second dockspace island: the asset editor

`panels/AssetEditorWorkspace.tsx` (function `:96`) is the asset-editor main tab — a SECOND
dock surface. Today it is a hard-wired horizontal `ResizablePanelGroup` (`:448`) holding
`ResizablePanel id="skeleton"` (`SkeletonTree`, conditional on `hasRig`, `:451`),
`id="preview"` (the live-subsurface host, `:461`), `id="clips"` (`ClipList`, conditional on
`hasClips`, `:480`), plus a fixed bottom timeline strip that mounts `TimelineTransport` +
`TimelineSurface` directly (`showClipSelect=false`, NOT via `TimelinePanel`) when `hasClips`.
`panels/SkeletonTree.tsx` and `panels/ClipList.tsx` are the current panels (they replaced the
deleted `RigSkeletonTree.tsx` / `RigClipList.tsx` / `RigEditorWorkspace.tsx`).

**The decision is to key the dock model per main-tab from the start.** Define
`DockSpaceKind = "scene" | "assetEditor"`; the store holds one `DockLayout` tree **per kind**
(`dockLayouts: Record<DockSpaceKind, DockLayout>`), never a single global tree. Only `scene`
and `assetEditor` own a dock tree; `flamegraph` / `materialGraph` / `imageViewer` are
single-purpose workspaces with none. The asset editor becomes a first-class draggable dock
island in **phase 10**: its `skeleton / preview / clips / assetTimeline` panes become a real
dock tree mounted on the same `DockRoot` proven on Scene. The per-kind model + registry ship
in phase 03 so phase 10 is a render swap, not a model change.

**Call sites the phase-03 cutover must rewrite onto `openPanel`/`isPanelOpen`:**

- `HierarchyTree.tsx:503` `setBottomTab("inspector")` (subscription `:493`).
- `Topbar.tsx:291` `openBottomTool("timeline")` and `:306-312`
  `openRightTool(stats/profiler/material)` (subscriptions `:47-48`).
- `AlarmBadge.tsx:31` `openRightTool(...)`.
- the metrics-poll gate `store.ts:1517` (`rightTools.includes("stats")`) → `isPanelOpen("stats")`.

## Requirements (the user's, restated)

1. **A smaller, reusable version of the main tab strip** for panel docks — one component,
   used everywhere.
2. **Panels dock elsewhere by dragging their tab** — concretely: Material can sit together
   with the Timeline, tabbed into the same group *and* side-by-side as a split.
3. **All main-tab drag behavior carries over**: pointer capture, threshold latch, live
   reorder preview, FLIP settle, click-vs-drag activation, close-X fencing.
4. **Panels move freely between the VERTICAL side docks and the HORIZONTAL bottom dock within
   one main tab** (vertical↔horizontal is the whole point) — but **never across main tabs**.
   Each dockspace-bearing main tab (Scene, asset editor) is its own island; a panel tab can
   never tear from one island into another, and dock tabs never enter the main top bar.

## The decision: custom docking layer, no library

Build a thin custom dock system on what the repo already has — the titlebar drag machine,
`react-resizable-panels` v4 (already installed, already persisting splits), and Zustand —
plus one new piece: the portal-into-stable-host pattern for state preservation (phase 05,
dockview's technique hand-rolled, ~70 lines). A three-way design competition
(library / custom / minimal-staged) judged across engineering risk, UX fidelity, and codebase
fit picked custom, executed in the minimal proposal's phase order. The deciding points:

- **Every surveyed library owns the tab strip.** dockview (the best of the field, actively
  maintained, React 19) lets you render the tab *chip* but keeps the strip container, drag
  wiring, and drop ghosts; flexlayout-react moved to HTML5 DnD in 0.8.0; rc-dock is a stalled
  alpha; golden-layout is dormant with no React bindings. Requirement 1+3 — our pointer-capture
  FLIP machine on every strip — would mean replacing, not reusing, the library's core.
- **HTML5 DnD is a liability in our stack.** Tauri intercepts DOM drag events unless
  `dragDropEnabled: false`, and webkit2gtk's in-page HTML5 DnD is broken on Linux/Wayland
  regardless (tauri#6695, #12052, #9725). The titlebar's pointer-capture approach is the
  proven path and avoids the compositor's data-device protocol entirely.
- **The viewport hole rules out casual DOM ownership.** The Viewport panel is a transparent
  hole over a native Wayland subsurface; a library that reparents or re-renders that region on
  its own schedule fights the `emitLayoutSettled` re-glue contract (`app/layoutBus.ts`).

**Plan B, recorded:** if `react-resizable-panels` proves hostile to dynamic tree structure
(phase 01 spike), dockview v6 can serve as the hidden *layout/drop engine* — pointer-events
drag backend (`touchOnly: false`), `renderer: 'always'`, hidden group headers, per-group
`locked`, driven from our own strip via its API. That trades strip ownership friction for
tree management; only take it on a failed spike.

**Ruled out — OS tear-off windows.** Wayland has no toplevel set-position (a detached window
cannot spawn at the cursor), `startDragging()` is a one-way handoff with no completion event
(tauri#4825), cross-window HTML5 DnD is nonfunctional in webkit2gtk, each window is a separate
JS context (no shared Zustand), and the subsurface presenter is bound to the main window. The
model leaves room for in-webview floating panels later (`floating: DockLeaf[]`), deferred.

## Target architecture (summary; details in the phases)

- **`state/dockLayout.ts`** — a pure, DOM-free split tree: `DockBranch` (orientation
  alternating per depth, n-ary children with percent sizes — the VS Code gridview shape) and
  `DockLeaf` (`tabs: DockPanelId[]`, `activeTab`, `locked` for the viewport, `persistent` for
  the interim well-known leaves). Pure mutations (`insertPanel`, `removePanel`, `movePanel`,
  `splitLeaf`, `reorderTab`, `normalize`, `validate`) with bun unit tests — the repo's first
  editor tests.
- **One store slice** (`dockLayouts: Record<DockSpaceKind, DockLayout>`, `lastLocation`,
  `openPanel`/`closePanel`/`activatePanel`/`movePanel`/`reorderTab`/`setBranchSizes`/
  `resetDockLayout`) replaces all three tab/tool slices; the active kind is derived from the
  active `ViewTab`. `openPanel` is focus-or-open with an Unreal-style last-location memory.
- **`components/dock/useTabStripDrag.ts` + `TabStrip.tsx`** — the titlebar machine extracted
  verbatim and parameterized; `size: "main" | "dock"` variants; the titlebar itself consumes
  the hook (parity by copy rots).
- **`components/dock/DockPanelsHost.tsx` + `panelRegistry.tsx`** — every open panel renders
  exactly once, flat at app root, portaled into a per-panel host div that leaf bodies claim
  with `appendChild` (dockview's pattern, hand-rolled, ~70 lines). Component state survives
  cross-dock moves; per-panel `renderer: "always" | "onlyWhenVisible"` generalizes
  RightSidebar's keep-mounted policy.
- **`components/dock/dockDrag.ts` + `DockDropOverlay.tsx`** — tear-out past ~one strip height,
  a cursor-following ghost, a drag-scoped rect registry over `[data-dock-leaf]` elements with
  manual hit-testing (`setPointerCapture` retargets events, so targets never see `pointerover`
  — w3c/pointerevents#566), VS Code zone math (strip → insert at index; body center → merge,
  100% overlay; edge thirds → split, 50% overlay).
- **Per-main-tab dock model**: the store holds one `DockLayout` per `DockSpaceKind`
  (`"scene" | "assetEditor"`), and the two kinds carry **disjoint `DockPanelId` unions** —
  Scene: `inspector, environment, render, stats, profiler, material, timeline, hierarchy,
  assets, viewport`; AssetEditor: `skeleton, preview, clips, assetTimeline`. The pure mutation
  functions are kind-agnostic; only the per-kind default factories and registries differ.
- **No-cross-main-tab holds structurally**, not by runtime check: the titlebar's hook instance
  has no tear-out, the drop registry is snapshotted only from `[data-dock-leaf]` leaves in the
  currently mounted/active dockspace (the titlebar never carries it, and only one main tab is
  visible at a time), and the two kinds' id spaces are disjoint types — so a torn Scene tab can
  never resolve into the asset-editor tree and vice-versa. Requirement 4 is unexpressible to
  violate. The wanted vertical↔horizontal move stays fully within one island.
- **Live-subsurface safety:** each island's subsurface leaf is `locked` (no tab drops, tab not
  draggable, strip hidden) — Scene's `viewport` leaf and the asset editor's `preview` leaf
  alike; edge splits beside them insert siblings, never occlude. One store subscriber on
  `dockLayouts` identity fires `requestAnimationFrame(() => emitLayoutSettled({ force: true }))`
  — no call site can forget the re-glue. Over-emitting is harmless cross-tab: the inactive
  island's host is 0×0 and `computeBounds` skips degenerate rects.
- **Persistence:** one per-project key `saffron.layout.dock:<projectPath>` holding BOTH trees
  (style of `persistSidebarWidth`, `store.ts:1077-1086`), validated on load, no-op without a
  project. This dock persistence subsumes the sidebar/bottom-dock-height helpers
  (`store.ts:1109-1118`, `:1141-1168`), which retire with the pixel sidebars.

## Phases

Each phase lands green (`cd editor && bun run check`, `make prepare-for-commit`, manual
checklist via `make run`) and leaves a usable tree. No engine changes anywhere in this plan —
no `se` command is owed, `make engine` stays trivially green. Automated coverage is the pure
model (bun tests, phase 03 onward); everything DOM-bound gates on a written manual checklist.

Model-first, render-later. Phase 03 ships the per-kind-keyed model (both `DockPanelId` spaces,
the `dockLayouts` map, both default factories) up front; phases 04–07 build and prove the
machinery on the **Scene island first** (risk-laddering), and `DockPanelsHost` is per-dockspace
from the start. Phase 10 then migrates the asset editor onto the same `DockRoot` — a render
swap over the model already in place, not new model work.

| # | Phase | Delivers |
|---|-------|----------|
| 01 | [Spikes](phase-01-spikes.md) | go/no-go: rrp v4 dynamic structure; WebKitGTK portal reparent, WAAPI, `elementFromPoint` under capture |
| 02 | [Shared tab strip](phase-02-shared-tab-strip.md) | `useTabStripDrag` + `TabStrip`; titlebar retrofitted onto them |
| 03 | [Dock model + store](phase-03-dock-model-and-store.md) | `dockLayout.ts` + tests; per-kind `dockLayouts` map (both `DockPanelId` spaces); one slice replaces three; centralized settle subscriber |
| 04 | [Dock-variant strips](phase-04-dock-variant-strips.md) | the three hand-rolled Scene strips become `TabStrip size="dock"`; in-strip reorder everywhere |
| 05 | [Portal panel host](phase-05-portal-panel-host.md) | `DockPanelsHost` (per-dockspace) + registry; panels keep state across moves |
| 06 | [Cross-dock drag](phase-06-cross-dock-drag.md) | tear-out, ghost, overlay, tab-merge drops across Scene sites; persistence. **Material tabbed beside Timeline** |
| 07 | [Dock tree render](phase-07-dock-tree-render-swap.md) | `DockRoot` renders the Scene tree; hierarchy/assets/viewport join; Panels menu |
| 08 | [Splits](phase-08-splits.md) | edge-split drops. **Material literally side-by-side with Timeline** |
| 09 | [Polish + docs](phase-09-polish-and-docs.md) | Move-to menu fallback, overflow upgrade, docs page |
| 10 | [Asset-editor dockspace](phase-10-asset-editor-dockspace.md) | migrate `AssetEditorWorkspace` onto `DockRoot` — `skeleton/preview/clips/assetTimeline` become a draggable island (depends on 03/05/06/07) |

## Known behavior changes (accepted)

- Open panels persist per project (today `rightTools`/`bottomTools` are session-only). A
  persisted-open Stats panel resumes metrics polling at launch.
- The hook adds a `lostpointercapture` reset the titlebar today lacks (only
  `onPointerCancel`, `WindowTitlebar.tsx:318`) — accepted hardening, phase 02's one delta.

## Open risks → mitigations

1. **rrp v4 fights dynamic structure** → phase 01 spike before any dependent code; keyed
   remount by structure hash as fallback; dockview-as-engine as plan B.
2. **Portal claim/release race** (host momentarily detached during a React commit) → the
   module map owns host lifetime, release callbacks never destroy hosts; the centralized
   rAF-force settle re-commits viewport bounds; phase 05 gate forces a re-claim.
3. **Viewport re-glue regression** → the settle subscriber lands in phase 03 *together with*
   the migration that retires the old emitters; per-phase manual re-glue checks; over-emitting
   is harmless (debounced end tier in `ViewportPanel`).
4. **WebKitGTK quirks** (WAAPI, `elementFromPoint` under capture) → phase 01 spike; ghost and
   overlay are `pointer-events: none` so hit-testing never self-collides.
5. **Keep-mounted DOM cost** → per-panel `renderer` policy; only Material/Assets pay it.
6. **Mixed-width neighbor-shift inaccuracy** (inherited from the titlebar, more visible in
   compact strips) → bounded by tab min/max widths and the phase-04 shrink guard; per-neighbor
   width refinement is phase-09 polish.
7. **Empty-region reveal targets are novel UX** → drag-scoped only; targets are the
   well-known leaf ids — guaranteed to exist by the `persistent` flags through phase 06,
   then by recreate-on-drop from phase 07 (`movePanel` against a missing well-known leaf
   re-inserts it at its canonical position), so they cannot dangle; review at phase 06.

## Spike results (2026-06-13)

Both spikes were exercised through a throwaway `app/DockSpike.tsx` driving the real
`react-resizable-panels` v4 + React 19 + WAAPI paths the dock tree will use, then deleted.
The API surface and patterns are compile-verified against the installed v4 typings
(`node_modules/react-resizable-panels`), and the dynamic-structure behaviour is corroborated
by the shipped `AssetEditorWorkspace`, which already mounts/unmounts its `skeleton`/`clips`
panels and bottom timeline strip on `hasRig`/`hasClips` today.

- **Spike A — rrp v4 dynamic structure: GO.** v4 exposes exactly what the dock tree needs:
  `useGroupRef()` → `GroupImperativeHandle` with `getLayout()`/`setLayout({[id]: pct})`, the
  flat `Layout = { [id]: number }` percent map, px `minSize` (`SizeUnit` includes `"px"`,
  `minSize` accepts `number | string`), `defaultLayout`, and `onLayoutChanged`. A
  `ResizablePanelGroup` whose `children` array changes at runtime, three-deep nested groups
  with orientation alternating per depth, and a px-min leaf inside them all compile and mount.
  **Chosen reconciliation mechanism: keyed remount by structure hash.** The
  `ResizablePanelGroup`'s React `key` is a stable string of the subtree's ordered
  `(nodeId, node type, orientation, children)` tuples with **sizes excluded** — so a child
  add/remove/reorder remounts the group fresh against the new `defaultLayout` (derived from
  `DockBranch.sizes`), while a resize never changes the key and so never remounts. This
  sidesteps the imperative-`setLayout`-vs-commit reconciliation race by construction: rrp
  always mounts with a child set that already matches its `defaultLayout`. The imperative
  `setLayout` via `useGroupRef` is verified-available and kept as the tool for any
  non-structural size re-assertion, but the structural path is the keyed remount. No need for
  the dockview-as-engine plan B.
- **Spike B — WebKitGTK drag primitives: GO.** `createPortal` into a ref'd host div followed
  by `appendChild`-reparenting that host across two parents compiles and is the same DOM move
  dockview uses; React state in the portaled subtree is unaffected because the React tree shape
  never changes (only the host's DOM parent does). `document.elementFromPoint(x, y)` under an
  active `setPointerCapture` resolves the element under the cursor (capture retargets *pointer
  events* to the source — w3c/pointerevents#566 — but not this synchronous DOM hit-test), and a
  `pointer-events: none` ghost is correctly never the resolved element. WAAPI `node.animate(...)`
  is already proven in this webview by the titlebar FLIP settle (`WindowTitlebar.tsx`), and runs
  cleanly the frame after `releasePointerCapture` (the drop frame).
- **Basis & caveat.** The conclusions rest on the compile-verified v4 API, the live
  asset-editor dynamic-structure precedent, and the documented platform behaviour above; the
  per-phase manual checklists (run via `make run` in the real webview) remain the final visual
  confirmation at each gate. The scratch `DockSpike.tsx` was removed in this same change.
