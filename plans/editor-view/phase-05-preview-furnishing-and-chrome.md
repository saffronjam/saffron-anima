# Phase 5 — preview furnishing + chrome

**Status:** NOT STARTED

## Goal

Make the preview scene **look like a preview**, not an empty void with editor chrome: a floor, sane
lighting and environment, the camera framed on the rig, a **bone overlay keyed to the previewed rig
with a selectable highlighted joint** — and the Edit-mode chrome (billboards, manipulation gizmo,
camera frustums) suppressed. UE5's Preview Scene Settings (floor/HDRI/light rig) is the reference;
one good fixed furnishing is v1 — no profiles. This phase also owns the engine-side overlay change
the skeleton tree (phase 8) needs: today `buildSkeletonOverlay` keys off `editor.selected` and only
draws when *that* entity carries `SkinnedMeshComponent`, with no per-joint highlight — both have to
change for a rig editor where clicking a bone must highlight a joint without killing the overlay.

## What exists to build on

- A fresh `Scene` has only defaults — `spawnSkinnedModel` creates just the rig (phase 4); the
  environment block (`scene.environment`) drives sky/ambient and is serialized per scene, so the
  preview scene's environment is free to differ from the authored one.
- The chrome: `submitSceneEditOverlay(..., bool editChrome)` (`host.cppm:581-594`) draws frustums +
  billboards + the manipulation gizmo only when `editChrome` is true; the call site passes
  `playState == Edit` (`host.cppm:901`). The skeleton overlay draws in **every** state
  (`host.cppm:592`, phase 10 of the animations plan), toggled by `set-skeleton-overlay`
  (`control_commands_animation.cpp:284-303`).
- **The overlay's real shape** (`buildSkeletonOverlay`, `host.cppm:339-355`): it `return`s early
  unless `editor.selected` is valid **and** carries `SkinnedMeshComponent`, and draws every joint
  in one uniform `JointColor` — there is **no** per-joint highlight, and a bone entity (which has
  `BoneComponent`, not `SkinnedMeshComponent`) would make it vanish. This is the gap this phase
  closes for phase 8.
- The camera: `renderCameraView(editor)` picks the fly-cam in Edit, the scene primary camera in
  Play (`scene_edit_play.cpp:165-178`); the camera is `ctx.sceneEdit.camera`, stashed/restored
  engine-side across enter/exit (phase 4). The thumbnail path has the bounding-sphere framing math
  (`renderer_thumbnail.cpp:210-223` — 3/4 view, distance from the bounding radius).
- **There is no grid pass** — verified: no grid shader or geometry anywhere; the floor must be real
  geometry or deferred.
- The mesh-ref cache: `loadMeshAsset` resolves a `MeshComponent` through the catalog
  (`findAsset(assets.catalog, id)` + the entry's path, `assets.cppm:2004-2020`), but
  `assets.meshRefByUuid` (`assets.cppm:51-58`) is checked first — a pre-seeded entry needs no
  catalog row, so a floor primitive can be provided without serializing an asset row.

## Work

### 1. Furnish on enter (extends phase 4's `enter-rig-preview`)

- **Floor**: one flattened-cube entity under the rig (scaled to ~4× the rig's bounding radius,
  neutral material), provided via a **pre-seeded `meshRefByUuid` entry for a reserved cube uuid**
  (no catalog row — a row would serialize into `project.json` and break byte-identity), or the
  same system-mesh path the editor-camera model uses. Name-tag it (`"PreviewFloor"`) for
  debuggability and so any generic entity-list UI can exclude it. Sit it at the rig's min-Y.
- **Lighting/environment**: a directional light entity angled like the thumbnail light
  (`thumbnail.slang`'s `normalize(-0.4, -1.0, -0.5)` is a proven-neutral direction) + the
  environment's procedural sky with moderate ambient — pick values once, validate visually against
  the leg/SimpleSkin rigs.
- **Camera**: frame the rig with the thumbnail math (bounding sphere → 3/4 view at
  `radius / tan(fovy/2) * 1.3`) by writing `ctx.sceneEdit.camera` on enter — **after** phase 4 has
  stashed it into `savedCamera`. The editor's orbit (phase 7) takes over from there; exit restores
  the stash, so the framing never touches the saved `editorCamera`.

### 2. Chrome policy

`editChrome` becomes false while previewing: the call site condition extends from
`playState == Edit` to `playState == Edit && !previewScene` (the preview is conceptually
"Edit-without-chrome"). The skeleton overlay stays available and defaults **on** while previewing
(flip `ctx.sceneEdit.skeletonOverlay.show` on enter, restore on exit) — bones-visible is the
expected first frame of a rig editor (UE5 Character > Bones).

### 3. Rig-keyed overlay + highlighted joint (the engine change phase 8 needs)

`buildSkeletonOverlay` must stop keying off `editor.selected`: while a preview is active, draw the
overlay for the **previewed rig** (`SceneEditContext.previewRigEntity`, the spawned rig mesh entity
stored by phase 4) regardless of the current selection — so selecting/highlighting a bone does not
blank the overlay.
Add a **highlighted-joint index** to `SkeletonOverlayOptions` (−1 = none), drawn in a distinct
tint, set by a new `set-skeleton-highlight { joint }` (or an `index` field on
`set-skeleton-overlay`) — this is the channel phase 8's tree click drives, instead of changing the
scene selection (which would null the selection-keyed `animationState` and break the rig timeline,
phase 11). Outside preview, the overlay keeps its current `editor.selected` behavior.

### 4. Floor toggle

`set-rig-preview-options { floor? }` (or fold into an existing options command if one fits):
UE5's "Show Floor" is the one preview-scene setting worth having on day one. Skeleton overlay
toggles already exist (`set-skeleton-overlay`).

## Validation (done criteria)

- `make engine` + `make prepare-for-commit` clean; fixtures for the new/extended commands.
- `make e2e`: enter preview → screenshot; the frame differs from an empty-scene screenshot (floor +
  rig visible); `set-skeleton-overlay show=false` → screenshot differs (overlay was on);
  no billboards/gizmo pixels (compare against a baseline with chrome on in Edit — assert the
  preview screenshot differs from an Edit-with-selection screenshot of the same rig spawned
  normally); **highlight a joint then select a bone entity → the overlay still renders** (proving
  it keys off the rig, not the selection) and the highlighted joint's screenshot differs; the
  byte-identity save round-trip (phase 4) re-run here now that the camera is framed — proving the
  engine-side camera restore holds with framing applied; validation-clean log.
- Visual sanity on both fixtures (`leg.gltf`, SimpleSkin): mesh lit from a sane angle and **textured
  per the imported material** (not flat white — the `.srig` material payload), floor under the
  feet, bones visible.
- `docs/`: the preview-scene page gains the furnishing + chrome policy.

## Notes / gotchas

- The furnishing entities live in the preview scene only — they can never leak (phase 4's
  byte-identity e2e already proves it), but keep them **name-tagged** so generic UI/entity-list
  surfaces can exclude them (the skeleton tree filters by construction — it renders `get-rig` data,
  not scene entities — so the tag is for debuggability, not the tree).
- Overlay state (`skeletonOverlay`) is global on `SceneEditContext`, not per-scene — save/restore it
  across enter/exit so a user's scene-view overlay preference survives previewing.
- Don't reach for a real grid renderer here; if a proper grid lands later (it is a general editor
  want), the floor toggle simply gains a mode.
- Exposure/tonemap defaults apply to the preview frame as to any frame — if the rig looks blown
  out, tune the light intensity, not the post chain.
