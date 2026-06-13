# Phase 10 — asset-editor dockspace island

**Status:** COMPLETED

## Goal

Migrate `AssetEditorWorkspace` off its hard-wired horizontal `ResizablePanelGroup`
(`AssetEditorWorkspace.tsx:448`) onto the same recursive `DockRoot` proven on Scene
(phase 07), so the asset-editor's `skeleton / preview / clips / assetTimeline` panels become
a real, draggable dock tree — its own island, the second dockspace kind. It reuses every
piece built and proven on Scene: `useTabStripDrag`, `TabStrip`, `DockPanelsHost`, `dockDrag`,
`DockDropOverlay`, and the pure model. The user wants this from the start (per-main-tab dock
model); the model + registry for `assetEditor` already shipped in phase 03, so this phase is a
**render swap**, not a model change. The wanted capability — moving a panel tab between the
vertical side leaves and the horizontal bottom leaf (vertical↔horizontal) — now works inside
the asset-editor island too; cross-main-tab moves stay forbidden by construction.

## What exists to build on

- The per-kind `dockLayouts: Record<DockSpaceKind, DockLayout>` model, the assetEditor
  `DockPanelId` union (`skeleton | preview | clips | assetTimeline`, disjoint from Scene's),
  and the assetEditor default factory (skeleton | preview(locked) | clips, plus a bottom
  `assetTimeline` leaf) — all landed in phase 03; `normalize`/`validate`/`movePanel`/
  `splitLeaf` are kind-agnostic pure functions that already operate on this tree.
- `DockRoot` (phase 07) renders an arbitrary `DockLayout`; `DockPanelsHost` (phase 05) was
  designed per-dockspace from the start; `dockDrag` (phase 06) snapshots only currently-mounted
  `[data-dock-leaf]` elements. Nothing in those needs new code for a second island.
- The current hard-wired asset-editor layout (`AssetEditorWorkspace.tsx`, function at `:96`):
  `ResizablePanelGroup orientation="horizontal"` `:448` holding `ResizablePanel id="skeleton"`
  (`SkeletonTree`, defaultSize 18 / minSize 12, conditional on `hasRig` `:114`) `:451`,
  `id="preview"` (the live-subsurface host — the `viewport-host` div `:471`, defaultSize 67 /
  minSize 30) `:461`, `id="clips"` (`ClipList`, defaultSize 15 / minSize 14, conditional on
  `hasClips` `:115`) `:480`, plus a fixed bottom strip mounting `TimelineTransport` +
  `TimelineSurface` directly (`showClipSelect={false}`, via `components/timeline/`) when
  `hasClips` `:488-493`.
- The preview pane's `viewport-host` div is the rect source for the asset-editor subsurface,
  exactly paralleling `ViewportPanel`'s scene host (`ViewportPanel.tsx:525`) — one such host
  per island. The settle bus reaches it via `useSubsurfaceBounds`
  (`lib/useSubsurfaceBounds.ts:148` subscribes `onLayoutSettled`), the same hook the Scene
  viewport uses.
- `App.tsx` keeps the subsurface live for this island: `subsurfaceVisible = sceneTabActive ||
  activeKind === "assetEditor"` (`:78`, effect `:198-205`); the asset-editor workspace is
  conditional-render/unmount (`{activeKind === "assetEditor" && activeAssetEditorId !== null
  && (<AssetEditorWorkspace key=… />)}`, `:222-230`), not `display:none`-parked.

## Work

### 1. Register the four asset-editor panels

Add their `DockPanelDef` rows to a registry scoped to the `assetEditor` kind (the phase-05
per-dockspace registry): `skeleton` → `SkeletonTree`, `clips` → `ClipList`, `assetTimeline` →
`TimelineTransport` + `TimelineSurface` (`showClipSelect={false}`, the same pairing the inline
strip mounts today), and `preview` → the live-subsurface host. `preview` is `locked` exactly
like Scene's viewport: non-closable, tab not draggable, no strip chrome, no merge/split-over.
The other three are closable. `skeleton`/`clips`/`assetTimeline` carry the same
`renderer: "onlyWhenVisible"` policy as their Scene cousins; `preview` is the locked,
always-visible host.

### 2. Mount `DockRoot space="assetEditor"`

Replace the `ResizablePanelGroup` + inline timeline strip (`:448-493`) with a single
`<DockRoot space="assetEditor" />` driving `dockLayouts["assetEditor"]`. `DockRoot` is
parameterized by dockspace (phase 07): it reads the assetEditor tree, renders branches as
`ResizablePanelGroup`s and leaves as `TabStrip` + host-claiming bodies, and tags each leaf
wrapper `data-dock-leaf="<leafId>"`. The `preview` leaf's body is the transparent
`viewport-host` hole (its pointer handlers — `onPointerDown/Move/Up/Cancel`, `onWheel` — move
into the preview panel component unchanged).

### 3. Capability gating through the model, not a code path

Today `skeleton` mounts only with `hasRig` and `clips`/the timeline strip only with `hasClips`
(`:449`, `:477`, `:488`). Per NO-COMPAT there is no second render branch: gate via the
registry / `openPanel` resolution and leaf `normalize`. When the previewed asset has no rig,
`skeleton` is not an open panel in the assetEditor tree (and `normalize` collapses its empty
leaf); when it has no clips, neither `clips` nor `assetTimeline` are open. Re-previewing an
asset re-derives which panels are open from its capabilities; `preview` is always present.

### 4. Scope the drag layer to this island

`DockPanelsHost` and `dockDrag` already snapshot only currently-mounted `[data-dock-leaf]`
leaves; since `AssetEditorWorkspace` mounts only while `activeKind === "assetEditor"`, only its
leaves enter the registry while it is the active main tab. Combined with the disjoint
`DockPanelId` unions (a Scene panel id can never resolve into the assetEditor tree, and vice
versa) and the titlebar never carrying `[data-dock-leaf]`, a torn asset-editor tab can never
drop into the Scene dockspace — the cross-main-tab move is unexpressible, not a runtime check.

### 5. Retire the hard-wired composition

Delete the `ResizablePanelGroup` group, the three fixed `ResizablePanel`s, the `hasRig`/
`hasClips` JSX branches, and the inline bottom timeline strip from `AssetEditorWorkspace`;
everything routes through `DockRoot`. The `SkeletonTree`/`ClipList`/timeline components stay —
they become the registered panel bodies.

### 6. Persistence

The assetEditor tree persists under the same per-project key as Scene
(`saffron.layout.dock:<projectPath>`, both trees in one payload — phase-06/03 contract); load
runs `validate` on each kind. The asset-editor arrangement is shared per-kind (one layout for
all asset-editor tabs; panel *content* is per-previewed-asset).

## Verify

- `bun test` covers the assetEditor default factory + `normalize`/`validate` on that tree
  (empty-skeleton/empty-clips collapse, the locked `preview` leaf never deleted); `bun run
  check`; `make prepare-for-commit` clean.
- Manual via `make run`, open an asset in the editor:
  - **Reorder/merge/split asset-editor panels among themselves: drag `clips` beside
    `skeleton`, drag the `assetTimeline` tab up into a side leaf (vertical↔horizontal within
    the island); state intact; ratios drag and persist.**
  - The preview subsurface tracks live across every split-drag and exact-glues on release; no
    per-tick engine resizes (watch the log).
  - Capability gating holds: a non-rigged asset shows no `skeleton` leaf; a clip-less asset
    shows neither `clips` nor `assetTimeline`; the locked `preview` is always present and never
    a merge/split target.
  - **A torn asset-editor tab can never drop into the Scene dockspace, and a torn Scene tab can
    never drop into the asset editor** — opening a Scene tab mid-thought shows no asset-editor
    leaves as drop targets, and vice versa (disjoint id spaces + active-island-only registry).
  - Reload round-trip restores the asset-editor arrangement alongside the Scene tree; switching
    Scene ↔ asset-editor keeps both subsurfaces live and re-glued.
