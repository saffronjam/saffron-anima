# Phase 4 — the preview scene

**Status:** NOT STARTED

## Goal

The engine-side stage: a **separate preview `Scene`** owned by `SceneEditContext`, entered and left
over the control plane, holding nothing but the previewed rig (+ phase 5's furnishing). Because it
routes through `activeScene`, the render path, compute skinning, the animation evaluator, and every
entity-addressed command retarget to it with no new wiring — and the authored scene cannot leak,
because `save-project` serializes `ctx.sceneEdit.scene` explicitly. This is the play-mode pattern,
replicated.

## What exists to build on

- `SceneEditContext` already holds `Scene scene` + `std::optional<Scene> playScene`
  (`scene_edit_context.cppm:188`, `:214`); `activeScene(ctx)` is the single sanctioned accessor
  branching on `playState` (`:236-243`; `sceneedit/AGENTS.md`: "the only sanctioned scene
  accessor").
- The host renders exactly one scene per frame through it: `tickAnimation` on `activeScene`
  (`host.cppm:838-842`), `renderScene(app.renderer, live, ...)` (`:879-903`). `renderScene` is fully
  Scene-parameterized (`assets.cppm:2424-2425`); the render graph holds no scene pointer.
- Compute skinning follows the passed scene (`forEach<TransformComponent, SkinnedMeshComponent>`
  inside `renderScene`, `assets.cppm:2603-2663`); the motion/RT caches key by entity uuid, and
  preview entities mint fresh uuids — no collisions.
- The open/close checklist: `enterPlay` (`scene_edit_play.cpp:75-106`) / `stopPlay`
  (`scene_edit_play.cpp:143-163`) — duplicate/drop the scene, re-resolve selection by uuid, drop
  smoothing queues, bump `sceneVersion`.
- Spawning: `spawnSkinnedModel` (`assets.cppm:2095-2169`) consumes an `ImportResult` assembled from
  phase 1's `ImportedRig` (nodes + skinDesc + the `RigMaterial` table) plus the mesh uuid and the
  phase-2 linked clip uuids. Note `spawnSkinnedModel` also calls `applyImportedMaterials`
  (`assets.cppm:2155`) — that is why phase 1 persists materials, so the preview is textured, not
  flat white.
- The camera persists into the project: `save-project` writes `doc["editorCamera"]` from
  `ctx.sceneEdit.camera` (`control_commands_asset.cpp:1305-1306`, `assets.cppm:513-515`) — there is
  one global fly-cam, so framing the preview (phase 5) and orbiting it (phase 7) mutate the saved
  camera; the byte-identity invariant requires restoring it engine-side on exit.
- The state-sync seam: `get-selection` carries `animationVersion`
  (`control_commands_scene.cpp:909-925`); the editor's poll gates `refreshAnimation(selectedId)` on
  it (`store.ts:1410-1416`). The animation commands resolve entities against `activeScene`
  (`playerOf`, `control_commands_animation.cpp:83-96`) and set `previewInEdit`
  (`:215`, `:244`) — so **select the preview rig and the whole existing pipeline works unchanged**.

## Work

### 1. `previewScene` + the `activeScene` branch

`std::optional<Scene> previewScene` on `SceneEditContext`, plus `previewAsset` (`Uuid`, what is
being previewed), **`Entity previewRigEntity`** (the spawned rig mesh entity — the handle phase 5's
overlay and phase 4's enter-result both need; a context field, not just a wire value), and a
`SceneEditCamera savedCamera` for the stash/restore (below). Extend `activeScene` — never add a
second accessor (`sceneedit/AGENTS.md`).

**Mutual exclusion — guard every command that touches the authored scene, not just `play`.** The
preview keeps `playState == Edit`, so the existing `playState != Edit` guards do **not** cover it.
Today `new-project` (`control_commands_asset.cpp:420`), `open-project` (`:459`), `load-scene`
(`:1260`), `load-project` (`:1320`), and `reload-project` (`:1347`) only `Err("stop play first")` —
they would run mid-preview, replace `ctx.sceneEdit.scene` while `activeScene` still returns the
stale `previewScene` (referencing the old catalog's uuids), and the freshly loaded project would
render nowhere. `delete-asset` (`:737-746`) likewise guards only `playState != Edit` but mutates
`ctx.sceneEdit.scene` **directly** (clears usages, drops a GPU ref) — deleting an asset mid-preview
edits the *authored* scene under the preview, a byte-identity risk the headline e2e may not catch.
And `import-model` has no play guard at all and spawns into `activeScene` (`:502`); `assign-asset` /
`set-asset-material` (`:799`, `:898`) similarly touch `activeScene` — during preview these land in
the preview scene and are silently dropped on exit. Extend every such guard to also reject (or
auto-`exit-rig-preview`) while `previewScene` is engaged, and pin `import-model`'s behavior under
preview (reject, or spawn into `ctx.sceneEdit.scene` explicitly). Symmetrically, `enter-rig-preview`
is rejected while `playState != Edit`, and `play` while previewing — all with clear error strings
the editor toasts.

### 2. `enter-rig-preview {asset}` / `exit-rig-preview`

Enter: resolve the asset (mesh or clip → its rig via the phase-2 links), `loadRigAsset`, construct a
fresh `Scene`, assemble the `ImportResult` (rig + materials + mesh uuid + linked clip uuids) and
`spawnSkinnedModel` into it, `relinkHierarchy`, **store the spawned rig mesh entity in
`previewRigEntity`** and select it, set the player's clip to the requested one (when opened from a
clip asset). **Stash `ctx.sceneEdit.camera` into `savedCamera`** before phase 5 frames it. Exit:
drop the scene, **restore `ctx.sceneEdit.camera` from `savedCamera`**, clear `previewRigEntity`,
restore the authored scene's selection by uuid (the `stopPlay` checklist: smoothing queues,
selection re-resolve). Both bump `sceneVersion` + `animationVersion` so the editor's
Hierarchy/Inspector/Timeline refetch across the swap. The camera stash/restore lives **engine-side**
so the byte-identity invariant holds even for CLI-driven enter/exit with no editor in the loop (a
`set-skeleton-overlay.show` stash mirrors this — phase 5).

**The enter result DTO** (the one new payload later phases consume — authored here): naming, used
consistently across phases 4/7/8/11 — **`rigEntity`** = the spawned rig mesh *entity* uuid,
**`rigMeshId`** = the rig's *mesh asset* uuid (phase 7's tab key); they are different values, do not
conflate. `EnterRigPreviewResult { WireUuid rigEntity; std::vector<RigBoneEntityDto> bones; }` where
`RigBoneEntityDto { i32 index; WireUuid entity; }` maps each `get-rig` bone index to its spawned
preview-scene entity uuid. Phase 8 (bone highlighting) reads `bones`; phase 11 reads `rigEntity` as
its timeline target.

Report the preview state in `get-play-state` (one new field, e.g. `previewAsset: WireUuid`,
0 = none).

### 3. Catalog borrow + runtime caches

The per-frame `live.catalog = &state->assets.catalog` borrow (`host.cppm:882`) follows
`activeScene` already. `AnimationRuntime` caches by clip and entity uuid (`animation.cppm:103-111`)
and is safely shared; clear its per-entity transition/lastPose entries for the preview entities on
exit (mirroring `clearOverrides` behavior) so a re-entered preview starts clean.

## Validation (done criteria)

- `make engine` + `make prepare-for-commit` clean. **Contract test**: `enter-rig-preview` /
  `exit-rig-preview` / `set-skeleton-highlight` are **skip-listed** (they need an imported `.srig`
  the harness can't create — same posture as phase 2); the DTO codegen still lands here and the
  `get-play-state` `previewAsset` field rides its existing fixture; assert the
  `EnterRigPreviewResult` / `get-play-state` shapes via `make e2e` (which imports `leg.gltf`).
- `make e2e` (the headline invariant): save the authored scene to JSON, `enter-rig-preview` on the
  imported leg rig, `play-animation`/`seek-animation`/`get-animation-state` against the preview rig
  (states advance), `exit-rig-preview`, save again — **byte-identical `project.json`** (including the
  `editorCamera` block, proving the engine-side camera restore); `list-entities` before/after
  matches; entering preview twice and exiting always lands back in Edit.
- `make e2e` negative lanes: `play` during preview rejected; `enter-rig-preview` during Play
  rejected; `load-project`/`import-model` during preview either rejected or handled per §1 (assert
  the authored scene is intact afterward, not the preview); `enter-rig-preview` on an unlinked clip
  errors with the stable message.
- Extend `runPlayModeSelfTest` (or a sibling) with the preview invariants: enter/exit leaves
  `playState == Edit`, `previewScene` empty, selection restored (`sceneedit/AGENTS.md` self-test
  rule).
- `docs/`: the scene-and-ecs or editor explanation gains the preview-scene concept (Edit / Play /
  Preview triad).

## Notes / gotchas

- **The preview takes over the single viewport stream** — by design, exactly like play mode. The
  frame published while previewing is the preview scene; the scene tab is parked anyway (phase 7).
- `previewInEdit` gating (`animation.cpp:589-594`): the spawned player must end up previewable —
  `play-animation`/`seek-animation` set the flag, so the editor's first transport action arms it;
  `enter-rig-preview` itself should leave the rig in the stopped rest pose (UE5 opens clips
  paused at frame 0; do the same).
- Keep `enter-rig-preview` idempotent-ish: entering while already previewing a different asset
  swaps the preview (drop + respawn) rather than erroring — that is how the editor switches assets
  with one tab open.
- An asset with no `.srig` errors with the exact string the editor keys the migration affordance on
  (phase 3) — make it stable and tested.
