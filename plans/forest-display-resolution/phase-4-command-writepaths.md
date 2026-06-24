# Command/script write-path resolution

**Status:** NOT STARTED
**Depends on:** phase-1-scene-substrate

## Goal

Three command/script write-paths resolve a single entity and write to the **container**, while the
renderer/runtime reads from the **mesh-bearing children** — so the write is a silent no-op on forests.
Route each to the forest's mesh/morph-bearing entities.

> **Re-verify first.** Cracks #5 (`material-assign`) and #6 (collider-fit) came from the completeness
> critic and were hand-confirmed here but **not** run through the audit's adversarial verifier. Before
> editing, confirm the editor actually sends the container id (not a child mesh id) for each — check
> `editor/src` material assignment + collider-fit call sites and the selection id they pass. If the
> editor already targets a mesh-bearing entity, downgrade/skip that item rather than "fixing" a phantom.

## Crack #5 — `material-assign`

`engine/crates/control/src/commands_asset.rs:2004-2018`:

```rust
let entity = resolve_entity(ctx, &params.entity)?;       // container for forests
...
scene.add_component(entity, MaterialAssetComponent::default());   // never rendered
scene.with_component_mut::<MaterialAssetComponent, _>(entity, |m| m.material = mat_id);
```

`resolve_entity_materials` (`engine/crates/assets/src/render_material.rs:158`) reads
`MaterialAssetComponent` off the **same entity that carries the `Mesh`**. On S2/S4/S5 that is a child, so
a material assigned to the container is never drawn.

Fix: apply the material to every entity in `model_mesh_entities(entity)` (or to the resolved mesh-bearing
entity when the selection is already a leaf). Decide the intended semantics — "assign to whole model"
(all mesh entities) is the natural editor action; if per-submesh/per-node assignment is wanted, target the
specific picked child. Match what the editor's material UI means by the selection. For S1 the resolved
entity *is* the mesh entity, so behavior is unchanged there.

## Crack #6 — collider auto-fit

`engine/crates/control/src/selector.rs:100` `fit_collider` passes the resolved entity straight to
`fit_collider_to_mesh` (`engine/crates/physics/src/world.rs:1080`), which reads `Mesh`/`SkinnedMesh` only
off that entity; `mesh_id == 0` on a container → early `return false` ("no resolvable mesh to fit the
collider to"). Fit fails on every S2 model.

Fix: resolve the mesh source from the forest (e.g. fit to the union AABB of `model_mesh_entities`, or to
the specific mesh child) before cooking. Keep the honest `false` only when the model genuinely has no
renderable mesh.

## Crack #4 — script/editor morph drive

`engine/crates/runtime/src/bridge.rs` `set_morph_weights` and
`engine/crates/control/src/commands_animation.rs` `morph_entity` both do: target = `e` if it has
`MorphComponent`, else `scene.animatable_descendant(e)`. For a clip-less morph forest the fallback returns
the bare container (no `MorphComponent`), the count probe errors, and the write is dropped — so
`sa.set_morph_weights` and the Inspector morph slider do nothing on that shape.

Fix: the fallback should resolve the **morph-bearing entity in the forest**, not the animation authority.
Add `model_morph_entity(root) -> Option<Entity>` to the phase-1 substrate (first subtree entity with a
`MorphComponent`) and use it in both `bridge.rs` and `morph_entity`. Confirm the Inspector path: the
audit found the Morph section renders only when the morph child is *selected* (so the common path already
hits the `has MorphComponent` short-circuit) — the fix mainly hardens the script/`sa` and
container-selected paths. Verify against `tests/e2e/morph*.test.ts`.

## Verify

- `sa material-assign <forest-container> <mat>` then a render/inspect shows the material applied to the
  model's meshes (was a no-op).
- `sa`/control collider auto-fit on a forest container produces non-default extents.
- e2e morph: drive `set-morph-weights` on a morph model via the container and assert the weights take.
- `just engine` + `just prepare-for-commit` green.
