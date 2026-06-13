# Phase 03 — dock model, store unification, centralized settle

**Status:** COMPLETED

## Goal

Introduce the pure dock-layout tree (`state/dockLayout.ts`) with unit tests, replace the
three parallel tab/tool store slices with a **per-main-tab-keyed** `dockLayouts` map, and
centralize the viewport re-glue: a single store subscriber fires `emitLayoutSettled` on every
dock mutation, retiring the per-site emitters in the same change. **Zero visual change** — the
Scene regions render exactly as before, now reading from the Scene tree's three well-known
leaves.

The model is keyed **per dockspace kind from the start**: `DockSpaceKind = "scene" |
"assetEditor"`, with a `DockLayout` tree per kind. This phase migrates the **Scene** render
sites only; the asset-editor render migration is **phase-10**. But the model, both disjoint
`DockPanelId` spaces, both default factories, and the centralized subscriber ship here, so
phase-10 is a render swap, not a model change. Only `scene` and `assetEditor` own a dock tree;
`flamegraph` / `materialGraph` / `imageViewer` are single-purpose workspaces with no tree.

## What exists to build on

- Scene slices to be replaced (`state/store.ts`): `BottomTab` (`:43`) + `bottomTab` (`:100`) +
  `setBottomTab` (`:462`); `RightTool` (`:46`) + `rightTools`/`activeRightTool` (`:120-121`);
  `BottomTool` (`:50`) + `bottomTools`/`activeBottomTool` (`:124-125`); the duplicated action
  trios `openRightTool`/`closeRightTool`/`setActiveRightTool` (`:751-770`) and
  `openBottomTool`/`closeBottomTool`/`setActiveBottomTool` (`:771-790`). Close-fallback
  contract: active falls to the index−1 neighbor (`:765`, `:785`).
- Read/write sites to migrate (Scene island):
  - `panels/Topbar.tsx:291` (`openBottomTool("timeline")`), `:306-312` (wrench menu →
    `openRightTool` for stats/profiler/material; subscriptions `:47-48`).
  - `components/AlarmBadge.tsx:31` (`openRightTool(target)`).
  - `panels/HierarchyTree.tsx:503` (`setBottomTab("inspector")` deep-link; subscription
    `:493`).
  - `app/Layout.tsx`: `rightToolsOpen`/`bottomToolsOpen` selectors (`:55-56`), the
    `bottomToolsOpen` settle effect (`:88-91`), `LeftBottomTabs`'s `setBottomTab` +
    rAF-settle handler (`:258-261`).
  - `panels/RightSidebar.tsx` / `panels/BottomDock.tsx` (their whole state surface).
  - The metrics-poll gate `rightTools.includes("stats")` (`store.ts:1517`).
- `resetSceneState` (declared `store.ts:333`, defined `:864-889`) resets `viewTabs` but leaves
  tool state alone — keep that: it must not touch `dockLayouts`.
- The asset-editor island (`panels/AssetEditorWorkspace.tsx`, function `:96`): a hard-wired
  horizontal `ResizablePanelGroup` (`:448`) with `id="skeleton"` (`SkeletonTree`, hasRig,
  `:451`), `id="preview"` (live subsurface, defaultSize 67 / minSize 30, `:461`), `id="clips"`
  (`ClipList`, hasClips, `:480`), plus a fixed bottom strip mounting `TimelineTransport` +
  `TimelineSurface` directly (showClipSelect=false, hasClips). Its panels become the assetEditor
  `DockPanelId`s `skeleton` / `preview` (locked) / `clips` / `assetTimeline`. `SkeletonTree.tsx`
  and `ClipList.tsx` are NEW files (they replaced the deleted `RigSkeletonTree` / `RigClipList`
  / `RigEditorWorkspace`).
- The settle bus: `app/layoutBus.ts` (`emitLayoutSettled` `:27-31`), consumed via
  `lib/useSubsurfaceBounds.ts:148` (`onLayoutSettled`), mounted by BOTH `ViewportPanel` (Scene)
  and the asset-editor preview pane.
- `tools/ci/check.sh` runs the frontend gate (`bun run build` at `:47`); there is **no editor
  test harness** anywhere yet (`editor/package.json` has `check` at `:9`, no `test` script).

## Work

### 1. `state/dockLayout.ts` — pure, DOM-free, kind-agnostic

Two disjoint `DockPanelId` spaces — this disjointness IS the structural no-cross-main-tab
guarantee. A panel id from one kind can never resolve into the other kind's tree:

```ts
export type DockSpaceKind = "scene" | "assetEditor";

export type SceneDockPanelId =
  | "inspector" | "environment" | "render"   // today: BottomTab
  | "stats" | "profiler" | "material"        // today: RightTool (MaterialEditorPanel)
  | "timeline"                               // today: BottomTool (TimelinePanel)
  | "hierarchy" | "assets" | "viewport";     // join the tree in phase 07

export type AssetEditorDockPanelId =
  | "skeleton"          // SkeletonTree, capability-gated on hasRig
  | "preview"           // locked live-subsurface leaf (like viewport)
  | "clips"             // ClipList, capability-gated on hasClips
  | "assetTimeline";    // TimelineTransport + TimelineSurface (showClipSelect=false)

export type DockPanelId = SceneDockPanelId | AssetEditorDockPanelId;

export type DockNodeId = string;

export interface DockLeaf {
  type: "leaf"; id: DockNodeId;
  tabs: DockPanelId[];                       // order = strip order
  activeTab: DockPanelId | null;
  locked?: boolean;                          // viewport/preview: no drops, tab not draggable
  persistent?: boolean;                      // normalize never deletes it, even empty
}
export interface DockBranch {
  type: "branch"; id: DockNodeId;
  orientation: "horizontal" | "vertical";    // alternates per depth (VS Code gridview)
  children: DockNodeId[];
  sizes: Record<DockNodeId, number>;         // percents, rrp v4 layout shape
}
export type DockNode = DockLeaf | DockBranch;
export interface DockLayout { version: 1; rootId: DockNodeId; nodes: Record<DockNodeId, DockNode> }

export type DropTarget =
  | { kind: "tab"; leafId: DockNodeId; index: number }  // without-moving-tab index space
  | { kind: "split"; leafId: DockNodeId; edge: "left" | "right" | "top" | "bottom" };
```

Pure functions beside the types, all operating on **one** `DockLayout` (kind-agnostic):
`insertPanel`, `removePanel`, `movePanel`, `splitLeaf`, `reorderTab`, `normalize` (delete empty
leaves that are neither `locked` nor `persistent`, collapse single-child branches, merge
same-orientation nesting), `validate` (drop unknown panel ids, clamp `activeTab` to membership,
prune `lastLocation` entries whose leaf no longer exists; structural failure ⇒ `null` ⇒ caller
falls back to that kind's default factory), and the `openPanel` resolution chain (below) as a
pure function. None of these know about kind — only the per-kind default factories and the
panel→default-leaf maps differ.

**Per-kind default factories.** Define one factory per `DockSpaceKind`.

- **Scene** (`defaultSceneLayout()`) — exactly three well-known leaves, all `persistent`,
  matching today's regions (the interim contract until phase 07):
  - `leaf:leftBottom` — `{ tabs: ["inspector", "environment", "render"], activeTab: "inspector" }`
  - `leaf:right` — empty
  - `leaf:bottom` — empty
- **AssetEditor** (`defaultAssetEditorLayout()`) — mirrors the current hard-wired
  `AssetEditorWorkspace` arrangement: a horizontal root branch `skeleton | preview | clips`
  with the `preview` leaf `locked`, plus a bottom `assetTimeline` leaf. This defines the
  canonical tree; the panels' actual presence stays capability-gated downstream (skeleton only
  with `hasRig`, clips/assetTimeline only with `hasClips`) via `openPanel`/normalization, never
  a second code path.

`persistent` means `normalize` never deletes the leaf; **emptiness** (`tabs.length === 0`) is
what drives Layout's conditional Scene-region unmount, exactly replacing today's
`rightTools.length`/`bottomTools.length` checks. Reveal-band drops (phase 06), `lastLocation`,
and `defaultLeafId` therefore always resolve: their target ids exist even while a region is
unmounted. Phase 07 removes the Scene flags and lets `normalize` collapse empties.

### 2. Tests — the repo's first editor unit tests

`state/dockLayout.test.ts` on `bun test`: every mutation; `normalize` incl. persistent-leaf
skips, single-child collapse, same-orientation merge; `validate` incl. dangling `lastLocation`
pruning and unknown-id drops; the `openPanel` chain incl. the terminal fallback; `reorderTab`
index semantics matching `insertionIndexForPointer`'s without-moving-tab space. Cover **both
kinds**: assert `defaultSceneLayout()` produces the three persistent leaves and
`defaultAssetEditorLayout()` produces the locked-`preview` horizontal tree + bottom
`assetTimeline`, and run `normalize`/`validate` over both trees. There is no editor test
harness today, so this phase introduces it: add `"test": "bun test"` to `editor/package.json`
and a test step **before** `bun run build` at `tools/ci/check.sh:47`.

### 3. Store slice — per-kind keyed

```ts
dockLayouts: Record<DockSpaceKind, DockLayout>;   // one tree per main-tab kind
lastLocation: Partial<Record<DockPanelId, DockNodeId>>;   // Unreal-style re-open memory
openPanel(id: DockPanelId): void;     // focus-or-open, resolution chain below
closePanel(id: DockPanelId): void;    // active falls back to the index-1 neighbor
activatePanel(id: DockPanelId): void; // open panels only
movePanel(id: DockPanelId, target: DropTarget): void;
reorderTab(leafId: DockNodeId, id: DockPanelId, index: number): void;
setBranchSizes(branchId: DockNodeId, sizes: Record<string, number>): void;
resetDockLayout(kind: DockSpaceKind): void;
// selector: isPanelOpen(id)
```

The active kind is **derived from the active `ViewTab`** (`scene` → `"scene"`, `assetEditor` →
`"assetEditor"`; the other three kinds have no tree). Each `DockPanelId` belongs to exactly one
kind, so the store routes every action to the right tree by the id's union membership — there is
no ambiguity and no runtime cross-kind check is possible.

**`openPanel` resolution chain** (each step validated, within the panel's own kind's tree):
already open ⇒ activate. Else `lastLocation[id]` if that leaf exists and is not locked ⇒ the
panel's default leaf if it exists (pre-phase-07 the Scene ones always do — persistent) ⇒ first
non-locked leaf in tree order ⇒ terminal fallback: append a fresh leaf as the root branch's last
child and insert there (`splitLeaf` generalized to accept the root id with a trailing edge). The
pure resolver takes the panel→default-leaf mapping as a parameter; this phase ships one interim
const per kind beside the types (`DEFAULT_LEAF: Record<DockPanelId, DockNodeId>` —
inspector/environment/render → `leaf:leftBottom`, stats/profiler/material → `leaf:right`,
timeline → `leaf:bottom`; skeleton/preview/clips/assetTimeline → their assetEditor default
leaves), which the phase-05 registry's `defaultLeafId` column absorbs. `lastLocation` is written
by `closePanel` from day one (`movePanel` joins in phase 06).

Deep-links and Topbar buttons all use `openPanel` — focus-or-open, never the silently-no-op
`activatePanel`.

Delete the five old Scene fields + two action trios. `ViewTab`/`moveViewTab`
(`store.ts:736-750`, scene pinned at index 0) untouched.

### 4. Centralized settle subscriber (lands here, not later)

One module-level store subscription on `dockLayouts` identity. The store uses plain `create`
(no `subscribeWithSelector` middleware), so the two-arg `(s, prev)` listener form would hand
back `prev === undefined` and `prev.dockLayouts` would throw. Use the captured-`last` single-arg
form the live precedents use (`FrameTimeGraph.tsx:197`, `components/timeline/TimelineSurface.tsx:207`):

```ts
let lastDockLayouts = useEditorStore.getState().dockLayouts;
useEditorStore.subscribe((s) => {
  if (s.dockLayouts !== lastDockLayouts) {
    lastDockLayouts = s.dockLayouts;
    requestAnimationFrame(() => emitLayoutSettled({ force: true }));
  }
});
```

It subsumes the two existing Scene re-glue paths this migration would otherwise orphan —
`LeftBottomTabs`'s tab-change rAF emit (`Layout.tsx:258-261`) and the `bottomToolsOpen` effect
(`Layout.tsx:88-91`, keyed on lengths that no longer exist) — retire both in this same change.
Region emptiness now lives inside `dockLayouts.scene`, so the subscriber fires on exactly the
same transitions, and on every future mutation (drops, opens, closes, resets, loads) for free.

The settle bus reaches **both** islands, and over-emitting is harmless cross-tab: a
`dockLayouts` mutation fired while a different main tab is active leaves the inactive island's
subsurface host at 0×0, and `computeBounds` (`useSubsurfaceBounds.ts:27-40`) skips degenerate
rects. The debounced end tier in `ViewportPanel` also absorbs extra emits. The rrp
`onLayoutChanged` chains and the pixel-resizer pointerup emit stay until phase 07.

### 5. Migrate every Scene caller

Topbar/AlarmBadge → `openPanel(...)` (Topbar `:291` timeline, `:306-312`
stats/profiler/material; AlarmBadge `:31`); HierarchyTree deep-link (`:503`) →
`openPanel("inspector")`; Layout selectors → leaf-emptiness selectors;
LeftBottomTabs/RightSidebar/BottomDock read their leaf's `tabs`/`activeTab` and call
`activatePanel` (their tabs are open by definition) / `closePanel`; metrics gate
(`store.ts:1517`) → `isPanelOpen("stats")` (polling keyed on *open*, not active —
`display:none` stays free). Per NO-COMPAT, the old slices and every caller are cut over in this
same change; no superseded path survives beside `openPanel`.

The asset-editor RENDER migration is **phase-10** — `AssetEditorWorkspace` keeps its hard-wired
`ResizablePanelGroup` until then. The assetEditor tree, default factory, and `DockPanelId` union
ship here so phase-10 is a pure render swap.

## Why cross-main-tab moves are unexpressible

No runtime guard is needed. (a) `SceneDockPanelId` and `AssetEditorDockPanelId` are disjoint
TypeScript unions, so a Scene panel id cannot index into `dockLayouts.assetEditor` (or vice
versa) — the type system rejects it. (b) `dockLayouts` holds one tree **per kind**, never a
single global tree, so the two islands' trees never share a node id space. (c) only one main tab
is mounted at a time, and the drag-scoped `[data-dock-leaf]` registry (phase 06) is snapshotted
only from the active dockspace's leaves, so an inactive island's leaves are never drop/merge/
split targets. Combined, a torn tab can resolve only within its own island.

## Verify

- `bun test` (new) + `cd editor && bun run check`; `make prepare-for-commit` clean;
  `tools/ci/check.sh` frontend step runs the tests before `bun run build`.
- Zero visual change on Scene. Manual via `make run`:
  - All three Scene strips behave as before (open/close/switch; right sidebar and bottom dock
    appear/disappear on first-open/last-close).
  - Hierarchy SubRow deep-link still lands on Inspector; AlarmBadge still opens Stats.
  - **Viewport re-glue:** switching left tabs and opening/closing the bottom dock still
    re-glues the subsurface (the two retired emitters' transitions, now via the subscriber).
  - The asset-editor tab still renders via its hard-wired layout (its render migration is
    phase-10) and its preview subsurface still tracks live.
- `bun test` asserts both default factories and `normalize`/`validate` over both kinds' trees.
