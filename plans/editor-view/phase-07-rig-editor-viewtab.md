# Phase 7 — the rigEditor ViewTab + workspace shell

**Status:** NOT STARTED

## Goal

The editor surface: a new `rigEditor` `ViewTab` kind whose workspace shell hosts the live preview —
the engine subsurface glued into its center pane via phase 6's hook, the preview entered/left with
the tab, the camera saved/restored, and the Assets panel routing rigged assets into it. After this
phase you can double-click the leg rig and **see it live, orbit it, and watch it animate** — the
side panels and timeline are placeholders filled by phases 8/9/11.

## What exists to build on

- The ViewTab recipe, with the materialGraph tab as the live precedent: union variant
  (`store.ts:55-66`, materialGraph at `:58`), open action with the dedupe/focus body
  (`openMaterialGraphTab` `store.ts:488-501`; ids namespaced `kind:<resourceId>`), `closeViewTab`
  (`:502-514`), workspace mount gated by `activeKind` (`App.tsx:223-225`) + body component
  (`MaterialGraphWorkspace` `App.tsx:282-295`), `tabIcon` branch
  (`WindowTitlebar.tsx:425-444`), re-title sync in `setAssetList` (`store.ts:434-445`), reset in
  `resetSceneState` (`store.ts:645-666`).
- Viewport parking: the `sceneTabActive` effect (`App.tsx:190-197`) parks on any non-scene tab —
  the rig tab needs the exemption; the `viewportHidden` → `client.setViewportHidden` bridge is
  `:199-205`.
- The open flow: `AssetsPanel` passes `onView={openAssetTab}` (`AssetsPanel.tsx:832`) and the grid
  context-menu equivalent `onViewAsset={openAssetTab}` (`:862`); tiles fire `onView` on double-click
  (`AssetTile.tsx:196`); the single-asset "View" `ContextMenuItem` is `AssetsPanel.tsx:992-995` (the
  batch "View (N)" item at `:965-974`); delete closes tabs by reconstructed id
  `closeViewTab(\`asset:${asset.id}\`)` in `confirmDeleteAssets` (`AssetsPanel.tsx:614`). Nothing
  branches on `AssetEntry.type` today.
- Engine lifecycle: `enter-rig-preview`/`exit-rig-preview` (phase 4 — the camera is stashed/restored
  **engine-side**, so the editor does not manage it), `get-rig` (phase 2), the serialized control
  wire + coalescer rule (`editor/AGENTS.md`); orbit drives `set-camera`
  (`control_commands_scene.cpp:1239-1277`, with `get-camera` at `:1235-1237`).
- Shell conventions (`editor/AGENTS.md` + the MaterialGraphEditor structure): `<main>` with
  `min-h-0 flex-1 overflow-hidden bg-background`, toolbar strip `border-b border-border px-3 py-2`,
  `ResizablePanelGroup` splits, semantic tokens only, `notifyError(errorText(err))`, module-level
  cache for tab-switch survival.

## Work

### 1. Store + tab strip — key the tab by the **rig**, not the opened asset

`{ id: string; kind: "rigEditor"; rigMeshId: string; title: string; closable: true }` in the union;
`openRigEditorTab(rigMeshId, title)` with id `rigEditor:<rigMeshId>`. **Tab identity is the resolved
rig (the mesh uuid)**, not the clicked asset — so a mesh and any of its clips open/focus the *same*
tab (the Persona model the README cites: one editor per character), and the one-`previewScene`
engine constraint can never be violated by two tabs of the same rig. **The caller resolves
asset → rig mesh id before opening** — a mesh asset uses its own id; a clip asset's rig is **not**
on its `AssetEntryDto` (no `mesh` field — phase 2 keeps the link off the DTO), so resolve it with a
one-shot async `get-rig { asset: clipId }` (which accepts a clip and returns its `mesh`, phase 2)
and open on the result; a `get-rig` error opens the migrate error state (phase 12). Same
dedupe/focus body as `openMaterialGraphTab`; re-title branch in `setAssetList` (keyed on the mesh
asset's name); `tabIcon` branch (lucide `Bone` or `PersonStanding`); close on the rig mesh asset's
delete alongside the `asset:` id in `confirmDeleteAssets` (`AssetsPanel.tsx:614`).

### 2. Workspace shell + subsurface

`RigEditorWorkspace { rigMeshId }` mounted at `App.tsx` next to the other workspaces, **keyed
`key={rigMeshId}`** so React remounts it (and runs cleanup) when switching between two different rig
tabs — `activeKind` stays `"rigEditor"` across that switch, so an `activeKind`-only effect would
*not* fire and the engine would keep previewing rig A under rig B's panels. Null-guard "Asset not
found" body. Internal layout: toolbar (asset name, status span, overlay/floor toggles slot) →
horizontal `ResizablePanelGroup`: left panel (skeleton tree placeholder, `defaultSize≈18`), center
**preview pane**, right panel (clips/details placeholder, `≈22`) → bottom timeline placeholder
strip. The preview pane hosts a transparent div (`viewport-host` pattern) consuming
`useSubsurfaceBounds`; all surrounding chrome paints opaque `bg-background`.

### 3. Lifecycle + parking

- Parking: `App.tsx:190` becomes "park unless `sceneTabActive || activeKind === 'rigEditor'`".
- Enter/exit keyed on the **active rig id**, not just `activeKind`: the effect depends on
  `[activeKind, activeRigMeshId]` (or, equivalently, lives in the `key={rigMeshId}` workspace as a
  mount effect with cleanup). On entering a rig tab (scene/other → this rig): `enter-rig-preview
  { asset: rigMeshId }`. On leaving (this rig → non-rig, close, or switch to a different rig):
  `exit-rig-preview`. Switching rig A → rig B is a remount: B's cleanup exits A, B's mount enters B.
  The camera is stashed/restored **inside `enter`/`exit`** (phase 4) — the editor does **not** call
  `get-camera`/`set-camera` for the lifecycle (that was the leak vector). Errors surface as toasts;
  an enter failure (no `.srig`) renders the workspace error state with the phase-3 "Migrate from
  scene" action.
- Orbit input on the preview pane: pointer-drag → coalesced `set-camera` yaw/pitch around the
  framed target; wheel → dolly. This mutates `ctx.sceneEdit.camera`, which `exit-rig-preview`
  restores from its stash — so orbiting never dirties the saved `editorCamera`. Keep it minimal
  (the fly-cam already works; orbit is the asset-viewer-appropriate model, matching every engine's
  preview).

### 4. Open routing

In the Assets panel's view flow, resolve asset → rig mesh id before opening:
`asset.type === "animation"` → `get-rig { asset: clipId }` then `openRigEditorTab(result.mesh,
rigMeshName)`; a `get-rig` error (no rig / unlinked clip) opens the rig editor's error state with
the migrate affordance. Mesh assets: full type-aware routing (rigged-mesh double-click → rig
editor) is phase 12 (it needs the persisted `rigged` flag synchronously in the click handler); v1
here adds the context-menu "Open in Rig editor" for mesh assets while double-click keeps the image
viewer.

## Validation (done criteria)

- `bun run check` + `bun run lint` clean; `make engine` untouched or clean.
- Manual (`make run`): double-click the imported leg clip → tab opens, live rig visible on the
  floor with bones, orbit works; switch to Scene tab → viewport re-glues to the dock, authored
  scene intact, camera restored; back → preview re-enters; close tab → exited.
- `make e2e`: the enter/exit lifecycle is already covered headless (phase 4); add a store-level
  test only if the project grows component tests — otherwise the manual matrix above + phase 13's
  full-flow e2e cover it.
- `docs/`: the editor explanation gains the rig-editor tab (open paths, what it shows).

## Notes / gotchas

- **One non-degenerate host at a time** is the invariant (phase 6) — the dock host is hidden while
  the rig tab is active, and workspaces unmount on switch; never render the preview host display:none
  with a live rect.
- **Tab identity is the rig (mesh uuid), not the asset** — this is why opening a clip and then its
  mesh focuses one tab, and why the single-`previewScene` engine constraint is structurally safe.
  The `key={rigMeshId}` remount is the mechanism that turns a rig-A → rig-B switch into a real
  exit/enter; an `activeKind`-keyed effect would silently keep previewing A.
- The engine resizes its offscreen chain once per tab switch (the rig rect ≠ dock rect) — same cost
  as a dock-split drag end; accepted.
- Respect the revamp constraints (README): internal tab state local, no draggable strips, plain
  `Tabs variant="line"` if the side panels need tabs.
- The preview survives Scene-tab round-trips by *re-entering* (cheap spawn) rather than keeping the
  preview scene alive while the scene tab renders — one stream, one active scene; keeping it alive
  would render the authored scene anyway.
