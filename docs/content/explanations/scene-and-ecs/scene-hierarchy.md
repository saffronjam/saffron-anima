+++
title = 'Scene hierarchy'
weight = 8
math = true
+++

# Scene hierarchy

The world is an ECS that bakes in no parent/child structure. The hierarchy is one component:
`Relationship` holds a durable parent `Uuid` (`0` means root) plus two runtime caches, the resolved
`parent_handle` and a `children` vector of live handles. Only the parent uuid is ever serialized or
copied; ECS handles are index+generation pairs that do not survive a reload, so the caches are
derived state, rebuilt by `relink_hierarchy` after any structural change (load, reparent, copy).

Every entity carries a `Relationship` — `create_entity` seeds a root one alongside
Id/Name/Transform — so the whole scene is one forest and any entity can be reparented without first
opting in.

## World transforms

`Transform` stores the local TRS. The world matrix is a second, derived component: once per frame,
`update_world_transforms` walks the forest roots-first through the `children` caches and writes
`WorldTransform { matrix: parent_world * local_matrix }` on every transformable entity. Consumers read
the cache through `world_matrix` / `world_translation` / `world_rotation` instead of re-deriving
ancestry per call. `local_matrix` prefers an animation `PoseOverride` (composed from its quaternion
directly, no Euler round-trip) over the authored `Transform`, so Edit-mode preview stays
non-destructive.

Two properties carry the design:

- ECS query order is unspecified, so the pass never iterates a query for ordering; the children
  caches are the only source of parent-before-child order.
- the composition keeps the full `Mat4`, so a non-uniformly scaled parent still yields a correct
  `normal_matrix = transpose(inverse(mat3(world)))` downstream.

`WorldTransform` is deliberately left unregistered, the same pattern as `IdComponent`:
`serialize_entity` only emits components that have a registry row, so a derived per-frame matrix
never lands in a scene file.

`compose_world_matrix` is the exact, uncached variant that walks the parent chain on demand. It
exists for the one place the cache may lag a just-edited local transform: reparent math.

## Reparenting

`set_parent(child, new_parent, keep_world)` is the sanctioned way to change the parent. It refuses
self-parenting and walks the new parent's ancestry to refuse cycles — without that guard, one bad
link makes every tree traversal loop forever. With `keep_world` (the editor convention) the child's
local TRS is rebased so its world transform does not move:

$$ local' = parentWorld^{-1} \cdot childWorld $$

The rebased matrix is decomposed back to translation/Euler/scale by `set_local_from_matrix`. The
Euler angles come from `quat_to_euler_zyx`, the stable $R_z \cdot R_y \cdot R_x$ extraction, because
`glam`'s quaternion-based `Quat::to_euler` is numerically unstable at $\pm 90°$ yaw — exactly the
rotation an editor produces all day. The decompose is TRS-only; a sheared parent loses its shear in
the round trip, which is accepted because `Transform` cannot represent shear anyway.

`destroy_entity` destroys the whole subtree: descendants are gathered through the children caches
first (despawning invalidates handles), the entity is detached from its parent's cache, then
everything is despawned.

## Durability and migration

The component registers like any other, but its serde emits only `{ "parent": <uuid> }` — the
`SceneSerialize` impl for `Relationship` never touches the caches, so the copy-entity
serialize→deserialize round trip cannot duplicate live handles. The scene loader resolves parent
uuids only after every entity exists (`relink_hierarchy` at the end of `scene_from_json`), because a
child's entry may precede its parent in the entities array.

`SCENE_VERSION` is 4. A pre-v3 document has no Relationship key, so `relink_hierarchy` defaults every
legacy entity to root; a dangling parent uuid downgrades to root with a warning instead of failing
the load. The same relink also cuts any parent cycle a hand-edited file carries, so a cyclic parent
written over the wire is reset to root rather than trusted.

## Skeletons ride the same tree

A skeleton is not a special structure: every glTF joint imports as an ordinary entity (`Bone` is just
a filter tag), parented through the same `Relationship` as everything else. The renderable carries a
`SkinnedMesh` — the mesh asset plus the ordered joint list **by uuid** (glTF joint order, parallel to
the inverse-bind matrices) and a non-serialized `bone_handles` cache that `relink_hierarchy` resolves
alongside the parent links.

Skinning consumes the hierarchy's one propagation pass: after `update_world_transforms`,
`joint_matrices` fills the GPU palette with

$$ joint_i = world(bone_i) \cdot inverseBind_i $$

so at bind pose every palette entry is the identity, and posing a joint is just moving an entity. The
skinned node's own transform never composes in (per glTF, joints place the vertices entirely), and
the skinned draw goes through a dedicated PSO that blends the palette per vertex (`vertexMainSkinned`
in `mesh.slang`, the extra `VertexSkin` vertex stream). Reparenting a joint out of its skeleton is
allowed (bones are entities) and simply changes its world matrix, hence the deformation.

## In the code

| What | File | Symbols |
|---|---|---|
| Relationship + world-transform components | `scene/src/component.rs` | `Relationship`, `WorldTransform` |
| Cache rebuild + cycle cut | `scene/src/hierarchy.rs` | `relink_hierarchy` |
| Per-frame flatten + accessors | `scene/src/hierarchy.rs` | `update_world_transforms`, `world_matrix`, `compose_world_matrix`, `local_matrix` |
| Reparent + subtree destroy | `scene/src/hierarchy.rs` · `scene/src/scene.rs` | `set_parent`, `set_local_from_matrix`, `destroy_entity` |
| Skeleton + joint palette | `scene/src/component.rs` · `scene/src/hierarchy.rs` | `SkinnedMesh`, `Bone`, `joint_matrices` |
| Skin import + bone spawn | `geometry/src/types.rs` · `assets/src/spawn.rs` | `ImportedSkin`, `save_mesh_skinned`, `spawn_skinned_model` |
| Skinned draw path | `rendering/src/pipelines.rs` · `assets/shaders/mesh.slang` | `request_mesh_pipeline`, `PsoKey::skinned`, `vertexMainSkinned` |
| Relationship/skin serde | `scene/src/serde.rs` | `SceneSerialize for Relationship`, `SceneSerialize for SkinnedMesh` |

## Related

- [Transform and matrices](../transform-and-matrices/) — the local TRS this hierarchy composes.
- [Serialization](../scene-serialization/) — the uuid-keyed document and the resolve pass.
- [Component registry](../component-registry/) — why unregistered derived components stay out of files.
