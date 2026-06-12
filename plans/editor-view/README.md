# rig editor — a Persona-style asset editor view

**Status:** NOT STARTED

This plan adds a **rig editor**: a full work-area editor tab that opens a rigged mesh (or one of its
animation clips) from the Assets panel and lets you **view, inspect, and preview it outside the scene**
— a live 3D preview of the rig deforming, a skeleton tree, the rig's clip list, and a scrubbable
timeline. It is the engine's equivalent of UE5's Persona (the Animation Editors), Unity's
import-settings preview, and Godot's Advanced Import Settings — every major engine isolates
*asset-level* rig/clip inspection from the open level, and ours currently cannot: double-clicking an
asset today shows a static 512 px bind-pose PNG (`AssetWorkspace`, `editor/src/app/App.tsx:249-262`),
and an `Animation` asset has no preview at all (`control_commands_asset.cpp:401-403`).

The cross-engine layout convention this implements (UE5 literally; Godot's import dialog is the same
shape rotated into a modal; Unity splits it between Scene view and Inspector):

```
+----------------------------------------------------------------------+
| Toolbar: asset name · status · overlay toggles                       |
+---------------+--------------------------------------+---------------+
| Skeleton tree |        3D preview viewport           | Clip list +   |
| (bones, left) |  (isolated preview scene, center)    | details       |
+---------------+--------------------------------------+---------------+
| Timeline: transport · ruler · lanes · playhead · footer              |
+----------------------------------------------------------------------+
```

**v1 is a viewer/inspector, not an authoring tool.** Keyframe authoring, notify/event tracks,
retargeting, sockets, and a standalone skeleton asset are out of scope (see the deferred list). The
timeline's `diamonds` lane mode (`editor/src/lib/timelineCanvas.ts:48`) is already stubbed for the
authoring follow-up.

## The three load-bearing decisions

These were settled by research (UE5/Unity/Godot anatomy + codebase verification + a measured
PNG-poll benchmark); the phases assume them.

1. **The preview stage is a separate `Scene`, routed through `activeScene` — the play-mode pattern.**
   `SceneEditContext` already holds `scene` + `std::optional<Scene> playScene` with `activeScene(ctx)`
   as the single sanctioned accessor (`scene_edit_context.cppm:236-243`, `sceneedit/AGENTS.md`).
   Adding `std::optional<Scene> previewScene` + one branch retargets the render path, compute
   skinning, the animation evaluator, and the entire entity-addressed control surface for free.
   A hidden preview entity in the authored scene is disqualified: `sceneToJson` serializes every
   `IdComponent` entity with no transient flag (`scene.cppm:1122-1137`), so a preview rig would leak
   into `project.json`; the separate scene is leak-proof because `save-project` passes
   `ctx.sceneEdit.scene` explicitly (`control_commands_asset.cpp:1305`).

2. **Pixels reach the editor by reusing the one viewport subsurface — not by PNG polling.**
   Measured: a `preview-render`-shaped request does three full `device.waitIdle()` stalls inside the
   frame loop (`renderer_thumbnail.cpp:313-314`, `:684`, `:721-722`) with a serial ceiling of
   ~12–22 Hz that degrades to ~5–12 Hz in a real session — and every poll hitches the live viewport.
   The subsurface emitter is not a singleton: any component may call `set_viewport_bounds`
   (`lib.rs:411-444`), and the parked dock's 0×0 rect no-ops via the `computeBounds` null guard
   (`ViewportPanel.tsx:38-51`). The rig tab keeps the subsurface visible inside its own pane and the
   engine publishes the preview scene through the existing shm ring — monitor-refresh scrubbing with
   zero presenter/transport changes. PNG stays for one-shot stills. A *second concurrent* live view
   (scene + rig simultaneously) stays out of scope; it needs at minimum the `plans/dmabuf-viewport`
   transport **plus** engine-side multi-view work (second offscreen chain, per-view scene
   addressing) that no plan currently owns.

3. **The rig becomes asset-persisted data: a `.srig` sidecar + additive catalog links.**
   Today the skeleton (node forest, joints, inverse binds, skeleton root, mesh node) and the
   mesh↔clip association exist only in the in-memory `ImportResult` (`assets.cppm:109-121`) and as
   spawned scene entities — `.smesh` v2 stores per-vertex joint indices/weights but not the joint
   list (`geometry.cppm:1245-1284`), `.sanim` has no mesh/skeleton uuid (`geometry.cppm:1326-1362`),
   and `AssetEntry` has no cross-references (`scene.cppm:340-350`). The fix: a uuid-named `.srig`
   sidecar beside the `.smesh` (the `.sanim` precedent — own magic/version, no risk to the mesh
   reader) plus additive optional catalog keys (`clips` on mesh rows, `mesh` on animation rows).
   No `ProjectVersion` bump (a bump hard-locks old builds out, `assets.cppm:681-685`) and no
   `.smesh` v3 (old `loadMesh` hard-fails on unknown versions, `geometry.cppm:1190-1193`). Old
   projects migrate by inverting `spawnSkinnedModel` over their already-spawned rigs (phase 3).

## Coordination with `plans/tabsystem-revamp`

Build on today's `ViewTab` system now; do **not** gate on the revamp (untracked, NOT STARTED, opens
with a go/no-go spike). Its phase 03 explicitly leaves `ViewTab`/`moveViewTab` untouched. Four
constraints keep the rig editor revamp-proof, baked into the phases:
- no new global tool-slice trios in the store (internal tab state stays local to the workspace);
- no hand-rolled draggable tab strips (plain `Tabs variant="line"` until the shared TabStrip lands);
- never mount the dock's `TimelinePanel` instance in the workspace — factor shared components both
  can render (phase 10);
- internal panels tolerate keep-mounted/`display:none` hosting (the `RightSidebar.tsx:70-90` policy).

## What "done" looks like

- Double-clicking a rigged mesh asset (or an animation clip asset) opens a **Rig editor** tab: the
  live preview shows the rig in its own furnished preview scene, the skeleton tree lists its bones,
  the clip list shows the clips imported with it, and the timeline plays/scrubs/loops the active
  clip at full frame rate — all without the asset ever being spawned into the user's scene, and
  without dirtying it: entering and leaving the preview leaves the authored scene byte-identical
  through a save round-trip.
- The rig data survives as assets: re-opening the project (or deleting the spawned entities) does not
  lose the skeleton or the mesh↔clip links; old projects migrate with one command.
- Every new engine state is reachable from the `se` CLI; the contract test covers the new commands;
  the docs site explains the editor view and the rig asset model.

## Phases

Each phase file carries a `**Status:**` line (`NOT STARTED` / `IN PROGRESS` / `COMPLETED`). Mark a
phase `COMPLETED` only when validation-clean (`make engine` + `make prepare-for-commit`; editor
phases also `bun run check` + `bun run lint`; wire/runtime phases also `make e2e` + the contract
test). Phases are dependency-ordered; 1–3 land per step of the design.

| Phase | Delivers | Status |
|---|---|---|
| [1 — `.srig` rig sidecar format](phase-01-rig-sidecar-format.md) | `SRigHeader` + `saveRig`/`loadRig` in Geometry (node forest + skin desc, the `.sanim` precedent); `importModel` writes it beside the `.smesh`; round-trip self-test. | NOT STARTED |
| [2 — catalog links + rig commands](phase-02-catalog-links-and-rig-commands.md) | additive `clips`/`mesh` catalog keys; `get-rig {asset}` returning the bone tree + linked clips; `list-clips {asset}` actually filters; DTOs + codegen + contract fixtures. | NOT STARTED |
| [3 — rig migration from spawned scenes](phase-03-rig-migration-from-scene.md) | `migrate-rigs`: the inverse of `spawnSkinnedModel` — synthesize `.srig` + links from already-spawned rigs so old projects open the editor without re-import. | NOT STARTED |
| [4 — the preview scene](phase-04-preview-scene.md) | `previewScene` on `SceneEditContext` + the `activeScene` branch; `enter`/`exit-rig-preview` (spawn from `.srig`, select, engine-side camera stash, version bumps); the bone→entity result table; mutual exclusion of **every** scene-replacing command; authored-scene byte-identity e2e. | NOT STARTED |
| [5 — preview furnishing + chrome](phase-05-preview-furnishing-and-chrome.md) | floor (pre-seeded mesh ref) + lighting + environment seeding; edit-chrome off in preview; rig-keyed skeleton overlay + a highlighted-joint channel; rig framing within the camera stash. | NOT STARTED |
| [6 — the subsurface bounds hook](phase-06-subsurface-bounds-hook.md) | extract `useSubsurfaceBounds(hostRef)` from `ViewportPanel` (pure refactor, no behavior change) so a second host rect can drive the subsurface. | NOT STARTED |
| [7 — the rigEditor ViewTab + workspace shell](phase-07-rig-editor-viewtab.md) | `ViewTab` variant keyed by the **rig** (mesh uuid) + `openRigEditorTab`; workspace `<main>` with the subsurface in its preview pane; parking exemption; `key={rigMeshId}` enter/exit lifecycle; orbit; open-from-Assets routing. | NOT STARTED |
| [8 — the skeleton tree panel](phase-08-skeleton-tree-panel.md) | left panel: the rig's bone hierarchy from `get-rig`; clicking a bone highlights the joint via the highlight channel (phase 5) — never scene selection, so the timeline stays live. | NOT STARTED |
| [9 — clip list + details panel](phase-09-clip-list-and-details.md) | right panel: the rig's linked clips (click to switch the previewed clip), clip/rig details, empty/edge states. | NOT STARTED |
| [10 — timeline extraction](phase-10-timeline-extraction.md) | parameterize `TimelinePanel`'s four store couplings into shared transport/canvas components; the dock Timeline behaves identically. | NOT STARTED |
| [11 — the timeline in the rig editor](phase-11-timeline-in-rig-editor.md) | mount the shared timeline against the preview rig: scrub/play/loop/step at full rate through the existing coalescer pipeline. | NOT STARTED |
| [12 — animation-asset affordances](phase-12-animation-asset-affordances.md) | animation tiles/tabs get real icons + duration badges; rigged-mesh double-click routes to the rig editor (`rigged` on the DTO); unlinked clips land on the migrate error state. | NOT STARTED |
| [13 — docs, e2e hardening, gate](phase-13-docs-e2e-hardening.md) | the full-flow e2e (open → scrub → close → byte-identical scene), docs pages + hub rows, `make check` green, polish pass. | NOT STARTED |

## Explicitly OUT (deferred)

- **Keyframe authoring** (key lanes, dopesheet/curves editing, record mode) — the timeline's
  `diamonds` mode is the prepared seam; authoring is its own plan.
- **Notify/event tracks**, montage-style sectioning, sync markers.
- **Retargeting** (bone mapping across rigs), a standalone `AssetType::Skeleton`, skeleton
  compatibility sharing — UE5's whole compatibility regime is deliberately not copied; if sharing
  ever matters, key it off a skeleton *signature* (bone-name/topology hash) per the research.
- **Sockets / attachments**, physics-asset editing (the `BonePhysicsComponent` reserved metadata
  stays inert until the Jolt plan).
- **A second concurrent live viewport** (scene + preview at once) — needs the dmabuf transport
  plus unowned engine-side multi-view work; the rig tab *takes over* the single stream exactly
  like play mode takes over the viewport.
- **Editing the rig itself** (bone add/remove/rename, rest-pose edits) — viewer first.
- **Preview-scene profiles** (UE5's named lighting rigs); a single sane furnishing is v1.
