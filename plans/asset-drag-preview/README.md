# Asset drag preview — ghost entity, async upload, broadphase pick

**Status:** Phase 1 IMPLEMENTED, Phase 3 IMPLEMENTED, Phase 2 DEFERRED (needs a GPU to validate).
Phases 1 and 3 remove the per-move re-instantiation and the per-move O(triangles) scene pick — the
two costs that scale with the dragged model's node count and the scene's triangle count. Phase 2
(the first-draw upload/BLAS stall) is specified and de-risked (reuse `ThumbnailWorker`) but not
implemented here: it is a runtime/concurrency change with no GPU available in this environment to
validate it. See each phase file's status note.

Dragging a model out of the asset browser and over the viewport is laggy, and the lag scales with the
"size" of the dragged asset. Moving the *same* object after it is dropped is cheap. The two paths are
completely different, and the preview path does three kinds of expensive work the transform path never
touches. This plan removes all three.

## Diagnosis (current code)

Each drag-over event (the editor throttles to ~60/s, `editor/src/panels/ViewportPanel.tsx`) sends
`asset-placement {phase: preview}`. The handler `preview_asset_placement`
(`engine/crates/control/src/commands_asset.rs:156`) → `compute_asset_placement` does, **every move**:

1. **Rebuilds a throwaway `Scene` from scratch** and re-instantiates the entire model graph
   (`Scene::new()` + `instantiate_model`, `commands_asset.rs:197`). The preview is a *separate* scene
   (`PlacementPreview`, `sceneedit/src/context.rs:80`) rendered each frame via
   `render_scene_with_transient` (`assets/src/render_scene.rs:543`).
2. **Runs a full CPU ray–triangle pick against the whole scene** to find the drop point —
   `pick_scene_surface` (`render_scene.rs:1085`): for every mesh it allocates a fresh `Vec<Vec3>` of
   *all* vertices transformed to world space and tests *every* triangle (`render_scene.rs:1133`). It
   already has an AABB broadphase (`ray_aabb_slab`), but the narrowphase is brute force — once the ray
   enters a big mesh's box it pays O(triangles).

Separately, the **first** frame the dragged mesh is drawn, its GPU resolve is a cache miss and runs a
**synchronous** vertex upload + BLAS build that blocks on `submit_and_wait`
(`engine/crates/rendering/src/upload.rs:251`). This is the size-correlated freeze; once cached
(`mesh_by_uuid`, `assets/src/load.rs:218`) it is paid forever, which is exactly why moving the object
*after* the drop is cheap — the bill was already paid during the preview.

By contrast a gizmo move is `set-transform` (`commands_scene.rs:477`) — one `Transform` write, O(1). No
re-instantiation, no pick, no upload.

## Approach

Make the preview behave like the thing that is already proven cheap — a normal scene entity that just
moves — and stop blocking the loop on big uploads.

1. **Ghost entity (Phase 1).** Instantiate the model **once** on drag-enter as a real, tagged entity in
   the authored scene; each move writes only its root `Transform` (the gizmo cost). On drop, drop the
   tag → it becomes a committed entity. On cancel/leave, despawn it. Delete the transient-`Scene` /
   `PlacementPreview` / `render_scene_with_transient` machinery entirely (NO LEGACY).

2. **Async mesh upload + spinner (Phase 2).** Move vertex upload + BLAS build off the loop thread so the
   first draw of a big mesh never stalls. `load_mesh_asset` becomes tri-state (ready / pending /
   failed); the draw path skips a pending mesh and the redraw seam re-arms on completion. The
   `asset-placement` reply carries an `uploading` flag; the editor shows a cursor-anchored spinner only
   if the upload outlives a 100 ms debounce (so simple models never flicker one).

3. **Broadphase pick (Phase 3).** Give `pick_scene_surface` a per-mesh BVH (or reuse the GPU pick) so
   the narrowphase touches only triangles near the ray, not all of them — so dragging into a heavy
   *scene* stays smooth regardless of the dragged object.

Phases are independent wins and land in order. Phase 1 is mostly mechanical and removes the per-move
re-instantiation; Phase 2 is the architectural one and kills the freeze; Phase 3 covers the
heavy-scene case neither of the first two touch.

## Scope

Editor host (`saffron-host`) + `saffron-control` + `saffron-assets` + `saffron-rendering`, and the
editor (`editor/`) for the spinner. The async-upload work (Phase 2) benefits the exported game too —
*any* first-time mesh draw stalls today, not just the preview.

## Phases

| # | File | What |
|---|------|------|
| 1 | `phase-1-ghost-entity-preview.md` | Real tagged ghost entity replaces the transient preview scene |
| 2 | `phase-2-async-mesh-upload.md` | Off-thread upload + BLAS, tri-state resolve, `uploading` flag, debounced spinner |
| 3 | `phase-3-broadphase-pick.md` | Per-mesh BVH (or GPU pick) so placement picking is sublinear in triangles |

## Verification

`just engine` + `just prepare-for-commit` at each phase boundary. The e2e suite (`tests/e2e`, driven
over the control plane) is the home for behaviour tests: assert that an `asset-placement` preview→commit
cycle leaves exactly one new entity, that a cleared preview leaves none, and (Phase 2) that a preview
reports `uploading` then clears. GPU-validate the felt latency by dragging a high-tri asset on the
RTX 3070 Ti.
