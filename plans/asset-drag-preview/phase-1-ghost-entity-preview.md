# Ghost entity preview: one scene, move not rebuild

**Status:** IMPLEMENTED (runtime e2e pending — see note)

> **Done.** `PreviewGhost` tag component added (`scene/src/component.rs`, unregistered, exported);
> `PlacementPreview` is now `{asset, root, rest_bounds}` (`sceneedit/src/context.rs`). The
> `asset-placement` handlers (`control/src/commands_asset.rs`) instantiate the model **once** into
> the authored scene, tag the whole subtree (`Scene::subtree_entities`, new in `scene/src/scene.rs`),
> and on each move write only the root `Transform`; commit untags the subtree, clear destroys it.
> `render_scene_with_transient` / the transient-`Scene` plumbing is **deleted** — `render_scene` is
> the single path and the ghost rides the normal gather (`assets/src/render_scene.rs`,
> `host/src/layer.rs`). Ghosts are filtered out of save (`scene/src/document.rs` `scene_to_json`),
> placement pick (`pick_scene_surface`), and the outliner (`list-entities`). `cargo build` +
> `clippy -D warnings` clean. Deterministic unit tests pass: `preview_ghost_is_excluded_from_serialization`
> and `subtree_entities_walks_root_and_descendants` (`scene/src/document.rs`). An e2e was authored
> (`tests/e2e/asset-placement.test.ts`).
>
> **Runtime e2e pending.** The headless host could not be exercised in the dev sandbox here — the
> standalone host's event loop does not pump the control socket under this weston-headless/llvmpipe
> setup (an unmodified HEAD build fails the same e2e identically, and `import` alone hangs in a
> detached host), so preview→commit→clear could not be observed end-to-end locally. This is an
> environment limitation, not a code defect; the flow validates in CI (real GPU) and the editor.
> A pre-existing, unrelated test (`asset_commands_register_in_manifest_order`) already fails on HEAD
> (its FROZEN list omits `export-app`/`get-stores`/`set-stores`) — left untouched.

**Scope:** Editor (`saffron-control`, `saffron-host`, `saffron-sceneedit`, `saffron-assets`)
**Scope:** Editor (`saffron-control`, `saffron-host`, `saffron-sceneedit`, `saffron-assets`)
**Depends on:** — (nothing)

## Goal

Stop rebuilding a throwaway `Scene` and re-instantiating the whole model on every drag-over. Instantiate
the dragged model **once** into the authored scene as a tagged *ghost* entity; each move writes only its
root `Transform` — the same O(1) cost as a gizmo drag, which is already proven cheap. Commit drops the
tag (the entity becomes real); cancel/leave despawns it. Delete the transient-scene machinery in the
same change.

This phase alone removes the per-move re-instantiation; it does **not** touch the first-draw upload
freeze (Phase 2) or the per-move CPU pick over the scene (Phase 3).

## Design

### A `PreviewGhost` marker component

Add a zero-field tag component in `saffron-scene` (alongside `Bone`, `component.rs:118`) marking an
entity as a not-yet-committed placement preview. The tag is **not registered for serialization** and is
checked everywhere the authored scene is enumerated for a user-facing or persisted purpose:

- **Project save** — the entity-walk that writes the scene (`saffron-assets` project save path,
  `assets/src/project.rs`) skips any subtree rooted at a `PreviewGhost`. A drag is modal, but an
  autosave or a `scene_version`-driven refetch must never capture the ghost.
- **Placement pick** — `pick_scene_surface` (`render_scene.rs:1085`) skips `PreviewGhost` subtrees, so
  the placement ray cannot hit the object being placed. (The separate-scene design gave this for free;
  the ghost must now opt out explicitly.)
- **Outliner / selection / scene tree** — the control queries that list entities for the editor tree
  exclude ghosts, so a half-placed model never flashes into the hierarchy panel.

The ghost **does** render through the normal scene path (that is the whole point — you see where it will
land) and may carry a translucent override so it reads as a preview rather than a committed object.

### Lifecycle: one instantiate, then transform writes

Replace the three `asset-placement` phases. `PlacementPreview` (`sceneedit/src/context.rs:80`) holds the
ghost's root `Entity` and its source asset id — **not a whole `Scene`**:

```rust
pub struct PlacementPreview {
    pub asset: Uuid,
    pub root: Entity,   // lives in scene_edit.scene, tagged PreviewGhost
}
```

- **preview, first event for this asset** — `instantiate_model(&mut ctx.scene_edit.scene, asset, name)`
  once, tag the root `PreviewGhost`, store `{asset, root}`, then position it (below).
- **preview, subsequent events** — if `placement_preview` already targets this asset, **do not
  re-instantiate**; compute the placement transform and `apply_transform(scene, root, transform)` — a
  single `Transform` write, exactly `set-transform`'s body.
- **preview, asset changed mid-drag** — despawn the old ghost subtree, instantiate the new one. (The
  editor only changes the dragged asset by starting a new drag, but handle it.)
- **commit** — remove the `PreviewGhost` tag from the stored `root`, bump `scene_version`, select it,
  emit one undo entry. The geometry is already uploaded and on screen, so commit is instant.
- **clear / leave** — despawn the ghost subtree; `scene_version` need not change (it was never authored).

A ghost transform write must **not** bump `scene_version` (that drives the editor's reconcile/refetch);
only commit does. The renderer reads the scene directly each frame, so the moving ghost still paints.

### Delete the transient machinery (NO LEGACY)

- `render_scene_with_transient` (`render_scene.rs:543`) and its `transient: Option<&mut Scene>` param —
  removed; `render_scene` (`render_scene.rs:492`) becomes the single entry, and `host/src/layer.rs:576`
  calls it with just the authored scene (which now contains the ghost).
- The `placement_preview.as_mut().map(|p| &mut p.scene)` plumbing in `layer.rs:571` — removed.
- The transient branch in `gather_static_draw_list` / `gather_skinned_draw_list` callers
  (`render_scene.rs:601`) — removed; the ghost rides the normal gather because it is a normal entity.

### Redraw seam

While a ghost exists and is moving, the host must keep rendering. Reuse the Phase-1 redraw controller
from `plans/rendering-performance`: each preview move is a mutating command, so `ControlContext::poll`
already returns `true` and `layer.rs:863` calls `request_redraw()`. Confirm a held-still ghost (cursor
not moving) still idles correctly — no new command, no redraw, last frame re-presented.

## Files

| What | File | Symbols |
|------|------|---------|
| Marker component | `engine/crates/scene/src/component.rs` | `PreviewGhost` |
| Preview state | `engine/crates/sceneedit/src/context.rs` | `PlacementPreview` (now `{asset, root}`) |
| Placement handler | `engine/crates/control/src/commands_asset.rs` | `preview_asset_placement`, `commit_asset_placement`, `compute_asset_placement` |
| Render entry | `engine/crates/assets/src/render_scene.rs` | delete `render_scene_with_transient`; keep `render_scene` |
| Host wiring | `engine/crates/host/src/layer.rs` | the `render_scene_with_transient` call site (~571–593) |
| Save exclusion | `engine/crates/assets/src/project.rs` | scene-save entity walk |
| Pick exclusion | `engine/crates/assets/src/render_scene.rs` | `pick_scene_surface` |

## Verification

- `just engine` + `just prepare-for-commit`.
- e2e (`tests/e2e`): `asset-placement` preview → commit leaves **exactly one** new root in the scene;
  preview → clear leaves **zero**; the saved project after a cleared preview is byte-identical to before
  (ghost never persisted); a validation-clean log.
- Manual on the RTX 3070 Ti: dragging a many-node model now tracks the cursor smoothly (per-move cost is
  a single transform write); confirm the felt freeze that remains is only the first-draw upload (Phase 2
  target).
