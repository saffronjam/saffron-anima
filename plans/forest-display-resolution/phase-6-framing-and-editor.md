# Framing + editor latent fixes

**Status:** COMPLETED — `focus` frames via `model_render_aabb` (extent-aware distance, falls back to
the entity translation when no mesh); `foot_ik_entity` and `set-kinematic-bones` reject a rig-less
target via `model_rig_entity` (kinematic-bones also now binds the actual `SkinnedMesh` entity, not
`animatable_descendant`); the editor closes the asset-editor tab keyed by both the asset id and its
container id (`AssetsPanel.confirmDeleteAssets`). Engine builds + project clippy green; editor `tsc`
clean. The `foot_ik_round_trips_on_plain_entity` test was rewritten to the new reject-rig-less contract.
**Depends on:** phase-1-scene-substrate

## Goal

Close the remaining minor/latent cracks: extent-aware camera `focus`, rig-only command guards, and the
editor asset-editor tab keying mismatch. None block display, but each is a single-entity / wrong-id
assumption of the same family.

## Crack #8 — `focus` aims at the pivot, size-blind

`engine/crates/control/src/commands_scene.rs` focus handler frames by
`position = world_translation(entity) - forward * 5.0` with no bounds. For a forest selected at its
container (`model_root_of` → container), `world_translation(container)` is the pivot, which can sit off
the visible geometry, and the fixed 5u distance ignores model size.

Fix: frame from `model_render_bounds(entity)` (phase 1) — aim at the bounds center and derive distance
from the radius/fov, the way `frame_preview_camera` already does for previews. Falls back to the entity
translation only when the model has no renderable bounds.

## Crack #9 — rig-only commands attach to a bare container

`foot_ik_entity` (`commands_animation.rs:271`) and `set-kinematic-bones` (`commands_physics.rs:206`)
resolve via `model_player` / direct and unconditionally `add_component` a rig-only component onto a
container that has no skeleton (S2). The runtime never reads it, but it persists on save and shows as a
phantom Inspector card. Note `get-foot-ik` even *mutates* on a read.

Fix: reject a non-rig target — guard on `model_rig_entity(entity).is_some()` (the ragdoll path already
does this via `RagdollMissingComponents`; mirror that). Do not attach `FootIk`/`KinematicBones` to an
entity with no `SkinnedMesh` in its forest, and make `get-foot-ik` read-only (no add on a getter).

## Crack #10 — asset-editor tab close keying

`editor/src/panels/AssetsPanel.tsx`: `openAssetEditorForAsset` opens the tab keyed by the model
**container** id (`getAssetModel` returns `mesh = container_id`), so a sub-asset (mesh/animation) row
opens `assetEditor:<containerId>`. But `confirmDeleteAssets` calls
`closeViewTab(\`assetEditor:${asset.id}\`)` with the **raw clicked sub-asset id**. For a forest (which is
exactly the model kind that exposes multiple openable sub-assets) `asset.id != containerId`, so deleting a
sub-asset leaves a stale tab open against a now-deleted asset.

Fix: key both open and close by the same id. Resolve the deleted asset's container id (the engine already
exposes it via `getAssetModel`/the catalog `container`) before `closeViewTab`, or key tabs by the clicked
asset id consistently on both sides. Verify the open side (`openAssetEditorForAsset`) and `enter-asset-
preview`'s stored `preview_asset` id agree, so list-icon → preview round-trips don't desync.

## Verify

- `focus` on a large/off-pivot forest frames the whole model on screen (was clipped/tiny/off-center).
- `sa set-foot-ik <static-container>` is rejected; `get-foot-ik` no longer mutates.
- Editor: open a forest's mesh/clip sub-asset in the asset editor, switch tabs, delete that sub-asset →
  the tab closes (no stale preview).
- `just engine` + `just editor` + `just prepare-for-commit` green.
