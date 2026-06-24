# Forest-aware resolution substrate

**Status:** COMPLETED — `model_mesh_entities`/`model_has_renderable`/`model_rig_entity`/
`model_morph_entity` added to `saffron-scene` (`hierarchy.rs`); the forest bounds union landed in
`saffron-assets` as `model_render_aabb` + `scene_render_aabb` (it needs the asset server for mesh
boxes, so it lives there rather than in `saffron-scene` — the one deviation from this doc), and
`scene_render_bounds` now delegates to it. Unit test `model_resolvers_cover_every_spawn_shape` covers
S1–S5.
**Depends on:** — (nothing)

## Goal

Give `saffron-scene` one vocabulary for "the renderable entities of a model", so every display surface
resolves the **forest** instead of guessing a single entity. This is the substrate phases 2–6 route
through. No surface changes yet; this phase only adds the resolvers + their bounds helper + tests.

## What exists today

- `animatable_descendant(root)` / `find_animatable` (`engine/crates/scene/src/hierarchy.rs:505,510`) —
  returns the first descendant with `SkinnedMesh` **or** `AnimationPlayer`, else `root`. This is the
  *animation authority* resolver. It is correct for *that* job and must stay, but it has been misused as
  a *mesh carrier* resolver — which is the whole bug.
- `model_root_of` / `model_player` (`hierarchy.rs:534,543`) — up-walk to `ModelInstance`, then
  `animatable_descendant`. Keep.
- `scene_render_bounds` (`engine/crates/control/src/commands_asset.rs:327`) — already unions every `Mesh`
  + `SkinnedMesh` over a scene. This is the **reference pattern**; its core moves down into `saffron-scene`
  so both the control crate and any other caller share it.

## Design

Add to `saffron-scene` (in `hierarchy.rs` or a new `model.rs` module, whichever matches the crate's
layout — check before adding a file):

- `model_mesh_entities(&self, root: Entity) -> Vec<Entity>` — every entity in `root`'s subtree carrying a
  `Mesh` or `SkinnedMesh`. Pre-order walk over the `Relationship.children` caches (call after
  `relink_hierarchy`). This is the canonical "what does this model actually render" answer.
- `model_has_renderable(&self, root: Entity) -> bool` — `!model_mesh_entities(root).is_empty()`. The gate
  predicate (phase 2).
- `model_rig_entity(&self, root: Entity) -> Option<Entity>` — the first subtree entity carrying
  `SkinnedMesh` (the *rig carrier*, distinct from the *animation authority* `animatable_descendant`). The
  overlay (phase 3) needs this. Returns `None` for an unrigged model.
- `model_render_bounds(&self, root: Entity) -> Option<Aabb>` — union of each mesh entity's world AABB,
  using the **joint palette** for `SkinnedMesh` entities (mirror `render_scene.rs:1034-1050`'s skinned-
  bounds fit and `scene_render_bounds`) and `world_aabb_from_corners(world_matrix, mesh_aabb)` for static
  `Mesh`. This is what phase 2's `compute_preview_bounds` and phase 6's `focus` consume. Decide where the
  per-mesh AABB comes from — `scene_render_bounds` already has the lookup; lift the shared core so there
  is exactly one implementation (NO LEGACY: do not leave `scene_render_bounds` as a second copy — make it
  call the new helper).

Naming/placement is provisional — match the crate's conventions when implementing. The contract is what
matters: a forest-walking mesh-entity resolver, a renderable predicate, a rig-carrier resolver, and a
forest bounds union, all in `saffron-scene` so no downstream surface needs its own walk.

## Tasks

1. Implement the four resolvers above in `saffron-scene`.
2. Fold `scene_render_bounds`' core into `model_render_bounds` and have the control-crate function call
   it (one implementation, not two).
3. Keep `animatable_descendant`/`model_player` as-is — they remain the animation-authority resolvers;
   only the *mesh/rig/bounds* misuses move to the new helpers.

## Verify

- Unit tests in `saffron-scene` constructing each shape S1–S5 by hand (mirror the existing
  `spawn_tests.rs` fixtures) and asserting:
  - S1 collapsed: `model_mesh_entities` = `[that entity]`, `model_has_renderable` = true.
  - S2 static forest: `model_mesh_entities` = all child mesh nodes (len ≥ 2), `model_has_renderable` =
    true, `model_rig_entity` = `None`.
  - S3 rigged: `model_rig_entity` = the `mesh_entity`, `model_mesh_entities` includes it.
  - S4 animated forest / S5 morph: `model_has_renderable` = true even though the container carries the
    `AnimationPlayer` and no mesh.
  - `model_render_bounds` for S2 is the union (not a 1-unit sphere at the origin); for S3 it tracks the
    joint palette, not the rest-pose mesh-node matrix.
- `just engine` + `just prepare-for-commit` green.
