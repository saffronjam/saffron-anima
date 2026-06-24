# Forest-aware display resolution

**Status:** NOT STARTED

## Why

Opening the static multi-mesh model `GothicCommode_01_1k` fails with
`model 'GothicCommode_01_1k' has no renderable mesh — re-import the asset`, while the single-mesh
`boulder_01_1k` opens fine. The model is valid — it renders correctly once spawned. The failure is a
**resolution bug**, and an audit found the *same* bug reaching a dozen different display surfaces.

### The one root cause

A surface resolves a **single entity** — via `animatable_descendant` / `model_player` /
`resolve_entity` / "first mesh sub-asset" — and then assumes *that* entity carries the
mesh / rig / material / bounds. That assumption holds only for one spawn shape.

The spawn shapes (`engine/crates/assets/src/spawn.rs`) are:

| Shape | What spawns | Where the `Mesh`/`SkinnedMesh` lives | Container carries |
|-------|-------------|--------------------------------------|-------------------|
| **S1** single-identity root | collapses to **one** entity (`is_single_identity_root`) | on that one entity | — (it *is* the mesh) |
| **S2** static multi-node forest | container + N child nodes (`spawn_node_forest`) | on the **child** nodes | nothing |
| **S3** rigged / skinned | `spawn_skinned_model`, one `mesh_entity` + container | `SkinnedMesh` on a **child** `mesh_entity` | `ModelInstance` only |
| **S4** animated forest | `spawn_node_forest` + clip | `Mesh` on **children** | `AnimationPlayer` only |
| **S5** morph | `spawn_node_forest`, morph on first mesh child | `Mesh`/`MorphComponent` on a **child** | maybe `AnimationPlayer` |

Only **S1** (the boulder) collapses so the single resolved entity *is* the mesh carrier. That is the
entire reason the boulder works and nothing else reliably does.

The render gather path is the **one place done right** — `gather_static_draw_list` /
`gather_skinned_draw_list` (`engine/crates/assets/src/render_scene.rs:756,808`) iterate
`for_each::<(&Transform, &Mesh)>` / `SkinnedMesh` over the **whole forest**, which is why these models
*render* fine in-scene. Every cracked surface below should adopt that pattern: **resolve the forest, not
one entity.**

### The two distinct broken sub-paths in the gate

A naive "walk to the first mesh-bearing descendant" fix is **not enough**. The gate
(`commands_asset.rs:2636`) fails through two different branches of `find_animatable`
(`engine/crates/scene/src/hierarchy.rs:510`):

- **S2** (no player): the walk finds no `SkinnedMesh`/`AnimationPlayer`, returns `None`, falls back to
  the container → container has no `Mesh` → reject.
- **S4 / S5** (player on container): `find_animatable` **short-circuits and returns the container the
  moment it sees its `AnimationPlayer`**, *before* ever inspecting the mesh children → container has no
  `Mesh` → reject.

A fix that only extends the no-match fallback repairs S2 but leaves every animated/morph model still
rejected, because the player short-circuit fires first. Both branches must be addressed. This is proven
reachable by an existing passing spawn test, `instantiate_animated_single_morph_node_keeps_its_player`
(`engine/crates/assets/src/spawn_tests.rs`).

## The confirmed cracks (audit result)

| # | Surface | Symbol / file | Shapes | Severity | Symptom |
|---|---------|---------------|--------|----------|---------|
| 1 | Open / preview gate | `enter_asset_preview` `commands_asset.rs:2636` | S2, S4, S5 | **blocker** | model rejected with "no renderable mesh" |
| 2 | Preview bounds / framing | `compute_preview_bounds` `commands_asset.rs:2788` | S2/S4/S5 (and S3 rest-pose) | minor→**blocker once #1 opens** | forest collapses to a 1-unit sphere at the origin; skinned frames off rest pose not joints |
| 3 | Skeleton / bone overlay | `build_skeleton_overlay` `host/overlay.rs` | S3, S4 | **major** | native bone overlay draws nothing for *every* rigged model |
| 4 | Script / editor morph drive | `set_morph_weights` `runtime/bridge.rs`; `morph_entity` `commands_animation.rs` | S5 (clip-less forest) | **major** | `sa.set_morph_weights` + Inspector slider no-op |
| 5 | `material-assign` | `commands_asset.rs:2004-2018` | S2/S4/S5 | **major** | `MaterialAssetComponent` lands on container, never drawn — visual no-op |
| 6 | Collider auto-fit | `fit_collider` `selector.rs:100` → `fit_collider_to_mesh` `physics/world.rs:1080` | S2 (and S4 container) | **major** | `mesh_id == 0` → fit fails, default extents kept |
| 7 | Thumbnail | `build_embedded_job` `thumbnail.rs` | S2/S4/S5 | minor | renders only the first mesh chunk + its material slots — a fragment |
| 8 | `focus` command | `commands_scene.rs` focus handler | all | minor | size-blind 5u pullback aimed at container pivot |
| 9 | Foot-IK / kinematic-bones | `foot_ik_entity` / `set-kinematic-bones` `commands_animation.rs` / `commands_physics.rs` | S2 | latent | attach rig-only component to a bare container (CLI-reachable) |
| 10 | Editor tab keying | `closeViewTab` vs `openAssetEditorForAsset` `AssetsPanel.tsx` | S2/S3/S4/S5 | latent | stale asset-editor tab survives sub-asset deletion |

Cracks **5** and **6** were surfaced by the completeness critic and hand-confirmed against the code
here; they were not run through the audit's adversarial verifier, so Phase 4 re-checks each before the
edit lands. Crack **2**'s S2 facet is shadowed by **1** today (the gate rejects before bounds runs), so
**it must be fixed in the same change as the gate** or it becomes the next blocker the instant the gate
opens.

## Approach

Stop resolving a single entity. Add a small **forest-aware resolution** vocabulary in `saffron-scene`
and route every cracked surface through it. No compat shim, no second code path: each surface's
single-entity resolution is **replaced** and its callers updated in the same change (NO LEGACY).

The render gather already proves the target pattern; the fix is to make the *gate, bounds, overlay,
material-assign, collider-fit, morph drive, and thumbnail* agree with what the renderer already does.

## Phases (dependency-ordered)

1. **`phase-1-scene-substrate.md`** — forest-aware resolvers + bounds union in `saffron-scene`, with
   unit tests over all five shapes. Everything else depends on this.
2. **`phase-2-gate-and-bounds.md`** — cut `enter_asset_preview`'s gate and `compute_preview_bounds`
   over to the substrate, covering **both** gate sub-paths and the forest bounds union. (Unblocks
   GothicCommode + every animated model.)
3. **`phase-3-overlay-rig.md`** — resolve the rig entity (not the container) in `build_skeleton_overlay`.
4. **`phase-4-command-writepaths.md`** — `material-assign`, collider-fit, and morph drive target the
   mesh/morph-bearing forest entities; re-verify cracks #5/#6 first.
5. **`phase-5-thumbnail-forest.md`** — thumbnail renders + frames the whole forest, not the first chunk.
6. **`phase-6-framing-and-editor.md`** — extent-aware `focus`, rig-only command guards, editor tab keying.
7. **`phase-7-tests-fixtures-docs.md`** — S1–S5 fixtures, e2e coverage, docs update so this can't regress.

Each phase ends on the milestone gate (`just engine` + `just prepare-for-commit`); Phases 1–6 each leave
the tree green. Phase 7 closes the test gap the audit flagged (no in-tree multi-mesh-node fixture exists
today, which is why none of these cracks had a failing test).
