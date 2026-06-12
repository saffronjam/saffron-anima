# rig editor — a Persona-style asset editor view

**Status:** COMPLETED — built on top of `plans/saffron-models` (model import, the `.smodel` container,
the asset scan). Kept as a build record. The shipped editor was subsequently generalized from a
rigged-only **rig editor** into the capability-aware **Asset editor** that opens *every* model (static
or rigged) — the `get-rig`/`enter-rig-preview` wire became `get-asset-model`/`enter-asset-preview`, the
panels gate on an `AssetCapabilitiesDto`, and the docs reference is
`docs/content/explanations/ui-and-editor/asset-editor.md`. The phase prose below predates that rename
and still uses the original "rig editor" naming; read it as history, not the current API.

This plan adds a **rig editor**: a full work-area editor tab that opens a rigged mesh (or one of its
animation clips) from the Assets panel and lets you **view, inspect, and preview it outside the scene**
— a live 3D preview of the rig deforming, a skeleton tree, the rig's clip list, and a scrubbable
timeline. It is the engine's equivalent of UE5's Persona (the Animation Editors), Unity's
import-settings preview, and Godot's Advanced Import Settings: every major engine isolates *asset-level*
rig/clip inspection from the open level. Double-clicking a rigged mesh or one of its clips opens a Rig
editor tab with a live preview, a skeleton tree, a clip list, and a scrubbable timeline.

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

3. **The rig is asset-persisted in the mesh's `.smodel` container.** The container's MetadataChunk
   holds the node hierarchy + skin (the rig), and the animation clips and materials are **sub-assets
   of the same container**, so the clip↔mesh↔material association is intrinsic — same file, one asset.
   The rig editor reads the rig via `get-rig {asset}`, sourced from the container metadata + its
   animation sub-assets, and spawns the preview via `instantiateModel`. The hard constraints carry
   over: no `ProjectVersion` bump (`assets.cppm:681-685`), no `.smesh` v3 (`geometry.cppm:1190-1193`)
   — `.smodel` is a new magic/version.

## Dependency on `plans/saffron-models`

Build `plans/saffron-models` first, in full. It owns the model import, the `.smodel` container, and
the asset scan: the container's MetadataChunk persists the node hierarchy + skin, the mesh, materials,
and animation clips are sub-assets of one container, and `instantiateModel` spawns a model into a
scene. This plan builds the rig editor view on top of that foundation — the preview scene spawns via
`instantiateModel` into `previewScene`, `get-rig {asset}` reads the bone tree from the container
metadata + its animation sub-assets, and the skeleton tree, clip list, and timeline consume those.

## Coordination with `plans/tabsystem-revamp`

Build on today's `ViewTab` system now; do **not** gate on the revamp (untracked, NOT STARTED, opens
with a go/no-go spike). Its phase 03 explicitly leaves `ViewTab`/`moveViewTab` untouched. Four
constraints keep the rig editor revamp-proof, baked into the phases:
- no new global tool-slice trios in the store (internal tab state stays local to the workspace);
- no hand-rolled draggable tab strips (plain `Tabs variant="line"` until the shared TabStrip lands);
- never mount the dock's `TimelinePanel` instance in the workspace — factor shared components both
  can render (phase 8);
- internal panels tolerate keep-mounted/`display:none` hosting (the `RightSidebar.tsx:70-90` policy).

## What "done" looks like

- Double-clicking a rigged mesh asset (or an animation clip asset) opens a **Rig editor** tab: the
  live preview shows the rig in its own furnished preview scene, the skeleton tree lists its bones,
  the clip list shows the clips imported with it, and the timeline plays/scrubs/loops the active
  clip at full frame rate — all without the asset ever being spawned into the user's scene, and
  without dirtying it: entering and leaving the preview leaves the authored scene byte-identical
  through a save round-trip.
- The rig data survives as assets: the rig + its clips live in the mesh's `.smodel` container, so
  re-opening the project (or deleting the spawned entities) re-derives them from the file.
- Every new engine state is reachable from the `se` CLI; the contract test covers the new commands;
  the docs site explains the editor view and the rig asset model.

## Phases

Each phase file carries a `**Status:**` line (`NOT STARTED` / `IN PROGRESS` / `COMPLETED`). Mark a
phase `COMPLETED` only when validation-clean (`make engine` + `make prepare-for-commit`; editor
phases also `bun run check` + `bun run lint`; wire/runtime phases also `make e2e` + the contract
test). Phases are dependency-ordered.

| Phase | Delivers | Status |
|---|---|---|
| [1 — rig query commands](phase-01-rig-query-commands.md) | `get-rig` / `list-clips` read the rig and its clips from the `.smodel` container metadata. | COMPLETED |
| [2 — the preview scene](phase-02-preview-scene.md) | `previewScene` on `SceneEditContext` + the `activeScene` branch; `enter`/`exit-rig-preview` spawning via `instantiateModel`; authored-scene byte-identity. | COMPLETED |
| [3 — preview furnishing + chrome](phase-03-preview-furnishing-and-chrome.md) | floor + lighting + environment seeding; edit-chrome off in preview; rig-keyed skeleton overlay + highlighted-joint channel; rig framing. | COMPLETED |
| [4 — the subsurface bounds hook](phase-04-subsurface-bounds-hook.md) | extract `useSubsurfaceBounds(hostRef)` from `ViewportPanel` (pure refactor) so a second host rect can drive the subsurface. | COMPLETED |
| [5 — the rigEditor ViewTab + workspace shell](phase-05-rig-editor-viewtab.md) | `ViewTab` variant keyed by the rig + `openRigEditorTab`; workspace `<main>` with the subsurface in its preview pane; enter/exit lifecycle; orbit; open-from-Assets routing. | COMPLETED |
| [6 — the skeleton tree panel](phase-06-skeleton-tree-panel.md) | left panel: the rig's bone hierarchy from `get-rig`; clicking a bone highlights the joint via the highlight channel, never scene selection. | COMPLETED |
| [7 — clip list + details panel](phase-07-clip-list-and-details.md) | right panel: the rig's linked clips (click to switch the previewed clip), clip/rig details, empty/edge states. | COMPLETED |
| [8 — timeline extraction](phase-08-timeline-extraction.md) | parameterize `TimelinePanel`'s store couplings into shared transport/canvas components; the dock Timeline behaves identically. | COMPLETED |
| [9 — the timeline in the rig editor](phase-09-timeline-in-rig-editor.md) | mount the shared timeline against the preview rig: scrub/play/loop/step at full rate through the existing coalescer pipeline. | COMPLETED |
| [10 — animation-asset affordances](phase-10-animation-asset-affordances.md) | animation tiles/tabs get real icons + duration badges; rigged-mesh double-click routes to the rig editor (`rigged` on the DTO). | COMPLETED |
| [11 — docs, e2e hardening, gate](phase-11-docs-e2e-hardening.md) | the full-flow e2e (open → scrub → close → byte-identical scene), docs pages + hub rows, `make check` green, polish pass. | COMPLETED |

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
