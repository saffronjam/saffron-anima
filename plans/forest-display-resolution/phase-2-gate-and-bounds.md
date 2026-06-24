# Open/preview gate + preview bounds cutover

**Status:** COMPLETED — the `enter_asset_preview` gate now uses `model_has_renderable(root)` (covers
both the S2 fallback and the S4/S5 player short-circuit); the rigged/bone block resolves
`model_rig_entity`; `compute_preview_bounds` unions the forest via `model_render_aabb`, fixing the
S2 origin-sphere collapse and the floor/framing. Builds clean; project clippy green.
**Depends on:** phase-1-scene-substrate

## Goal

Make `enter_asset_preview` accept and correctly frame every model shape. This is the headline fix —
it unblocks `GothicCommode_01_1k` (S2) **and** every animated/morph model (S4/S5). Cracks **#1** and
**#2** land **together** because #2's S2 facet is dead code only while #1 rejects S2; opening the gate
without fixing bounds turns the next-open into a mis-framed 1-unit sphere.

## Crack #1 — the gate

`engine/crates/control/src/commands_asset.rs:2636-2643`:

```rust
let animatable = preview.animatable_descendant(root);
let rigged = preview.has_component::<SkinnedMesh>(animatable);
if !rigged && !preview.has_component::<Mesh>(animatable) {
    return Err(Error::command(format!(
        "model '{}' has no renderable mesh — re-import the asset", meta.name)));
}
```

Replace the single-entity probe with the forest predicate:

```rust
if !preview.model_has_renderable(root) {
    return Err(...same error...);
}
```

This covers **both** broken sub-paths at once (the S2 no-match fallback and the S4/S5 `AnimationPlayer`
short-circuit, see README), because `model_has_renderable` walks the subtree for any `Mesh`/`SkinnedMesh`
rather than inspecting one resolved entity.

Then audit the lines *after* the gate that still use `animatable` as if it carried the mesh:

- The `rigged` / bone block (`commands_asset.rs:2646` onward) — `rigged` should come from
  `model_rig_entity(root).is_some()`, and the bone/skin handling should target the rig entity, not
  `animatable_descendant`. Where the block genuinely wants the *animation authority* (the
  `AnimationPlayer` for open-from-clip), keep `animatable_descendant` — but the open-from-clip player
  write (`player.clip = id`) must target the entity that actually has the `AnimationPlayer`, not the first
  pre-order `SkinnedMesh`/player hit (the audit flagged a multi-player mis-target risk; assert the target
  has an `AnimationPlayer` before writing).

## Crack #2 — `compute_preview_bounds`

`engine/crates/control/src/commands_asset.rs:2788-2840` resolves `mesh_entity = animatable_descendant(root)`
and reads one entity's AABB; on a forest `mesh_id` falls to `Uuid(0)` and bounds collapse to
`center = world_translation(container); radius = 1.0`. Replace the body with
`model_render_bounds(root)` from phase 1 (forest union, joint-palette-aware for skinned). Feed its
center/radius to `frame_preview_camera` and `spawn_preview_floor` as today.

This simultaneously fixes:
- the S2 origin-sphere collapse,
- the S3 rest-pose-vs-joint-palette mis-frame,
- crack **#8**'s preview-floor facet and the `set-asset-preview-options` floor-toggle re-placement
  (`commands_animation.rs:583-591`), since both call `compute_preview_bounds`.

## Tasks

1. Swap the gate predicate to `model_has_renderable`.
2. Repoint the post-gate `rigged`/bone/open-from-clip block at `model_rig_entity` / the verified
   `AnimationPlayer` entity.
3. Replace `compute_preview_bounds`' single-entity body with `model_render_bounds`.
4. Delete any now-dead single-entity helpers left behind (NO LEGACY).

## Verify

- e2e (`tests/e2e`): boot a headless host, import or load a project containing a multi-mesh-node model
  (the GothicCommode case — add a small multi-node glTF fixture if none exists; see phase 7), call
  `enter-asset-preview`, assert success + a sane returned bounds (not radius 1.0 at origin).
- Repeat for an animated model (S4) and a morph model (S5) — both must open.
- Manual: `just run`, open the `dev` project, open `GothicCommode_01_1k` — it shows, framed, on the floor.
- `sa` round-trip: an `enter-asset-preview` then a bounds/preview query reflects the assembled extents.
- `just engine` + `just prepare-for-commit` green.
