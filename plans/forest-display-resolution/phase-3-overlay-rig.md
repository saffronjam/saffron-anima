# Skeleton/bone overlay rig resolution

**Status:** COMPLETED — `build_skeleton_overlay` resolves `model_rig_entity(target)` instead of
gating on `SkinnedMesh` on the container; positive test
`skeleton_overlay_draws_for_a_rig_on_a_child_of_the_selected_container` added and the negative test
updated. The redraw prerequisite is done: `pick` reclassified as mutating in `is_read_only_command`
(it sets selection); `pick-skeleton-joint` left read-only (verified it only projects, no mutation).
**Depends on:** phase-1-scene-substrate

## Goal

Make the native skeleton/bone overlay draw for rigged models. Today it draws **nothing for every
standard rig** (S3/S4), because it gates on `SkinnedMesh` on the **container** the selection/preview
hands it, while `spawn_skinned_model` always puts `SkinnedMesh` on a **child** `mesh_entity`.

## Crack #3

`engine/crates/host/src/overlay.rs`, `build_skeleton_overlay` (within `build_scene_edit_overlay`),
~line 509:

```rust
if !scene.valid(target) || !scene.has_component::<SkinnedMesh>(target) { return; }
```

Both entry paths feed `target` = the model **container root**:
- preview: `target = editor.preview_root_entity`, set to the spawn root at `commands_asset.rs:2710`;
- normal: `target = editor.selected`, set by picking to `model_root_of(hit)` (`commands_scene.rs:815`) =
  the `ModelInstance` container.

`spawn_skinned_model` (`engine/crates/assets/src/spawn.rs:273-364`) adds `SkinnedMesh` + `bone_handles`
to a child `mesh_entity` and returns a separate bare container. So the gate is always false → early return.

The asymmetry confirms it is a code bug, not data: the sibling `pick-skeleton-joint` command
(`commands_animation.rs:519-574`) resolves bones by uuid through `preview_bone_by_node` and works — you
can click a joint, but the overlay meant to show it is blank.

## Fix

Resolve the rig entity before reading `bone_handles`:

```rust
let Some(rig) = scene.model_rig_entity(target) else { return; };
// read bone_handles / joints from `rig`, not `target`
```

`model_rig_entity` (phase 1) walks the subtree for the `SkinnedMesh` carrier. For S2 (no rig) it returns
`None` and the overlay correctly draws nothing — that case is *not* a regression, an unrigged model has no
skeleton. Make sure any subsequent reads in the function (`bone_handles`, joint transforms,
`skeleton_overlay` config) use `rig`.

## Prerequisite — reactive redraw on selection (from `rendering-performance`)

The native overlay only rebuilds on a **rendered** frame. Under the reactive loop, viewport selection
via `pick` (`commands_scene.rs:757`) mutates selection but is allow-listed **read-only**
(`registry.rs:578`), so clicking a rig requests no redraw and the overlay won't appear until another
trigger. Before/with this phase, reclassify `pick` / `pick-skeleton-joint` as mutating (or make
`selection_version` a redraw reason in `host/src/layer.rs:render_activity_reasons`). This also fixes the
existing gizmo overlay not repainting on viewport-click select. Coordinate with the
`rendering-performance` owner — it is their surface. See the README "Interaction with
rendering-performance" section.

## Verify

- The audit noted `skeleton_overlay_off_emits_nothing` (`overlay.rs:1805`) only covers the off case and a
  no-rig selection — and bakes in the wrong "self-gates" assumption. Add a test that builds a selection
  where `SkinnedMesh` sits on a **mesh-child under a selected container** and asserts the overlay emits
  joints/segments. Keep the unrigged-selection case asserting empty.
- Manual: `just run`, select any imported rig, confirm the bone overlay renders; confirm the GothicCommode
  (S2) still shows no skeleton (correct).
- `just engine` + `just prepare-for-commit` green.
