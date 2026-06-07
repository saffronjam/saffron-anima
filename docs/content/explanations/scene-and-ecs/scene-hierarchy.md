+++
title = 'Scene hierarchy'
weight = 8
math = true
+++

# Scene hierarchy

The scene is an entt registry, and entt bakes in no parent/child structure. The hierarchy is one
component: `RelationshipComponent` holds a durable parent Uuid (0 means root) plus two runtime
caches, the resolved `parentHandle` and a `children` vector of live handles. Only the parent uuid
is ever serialized or copied; entt handles are index+version pairs that do not survive a reload,
so the caches are derived state, rebuilt by `relinkHierarchy` after any structural change (load,
reparent, copy).

Every entity carries a `RelationshipComponent` — `createEntity` emplaces a root one alongside
Id/Name/Transform — so the whole scene is one forest and any entity can be reparented without
first opting in.

## World transforms

`TransformComponent` stores the local TRS. The world matrix is a second, derived component: once
per frame, `updateWorldTransforms` walks the forest roots-first through the `children` caches and
writes `WorldTransformComponent{ parentWorld * transformMatrix(local) }` on every transformable
entity. Consumers read the cache through `worldMatrix` / `worldTranslation` / `worldRotation`
instead of re-deriving ancestry per call.

Two properties carry the design:

- entt views are unordered, so the pass never iterates a view for ordering; the children caches
  are the only source of parent-before-child order.
- the composition keeps the full mat4, so a non-uniformly scaled parent still yields a correct
  `normalMatrix = transpose(inverse(mat3(world)))` downstream.

`WorldTransformComponent` is deliberately unregistered, the same pattern as `IdComponent`:
`serializeEntity` skips storages with no registry row, so a derived per-frame matrix never lands
in a scene file.

`composeWorldMatrix` is the exact, uncached variant that walks the parent chain on demand. It
exists for the one place the cache may lag a just-edited local transform: reparent math.

## Reparenting

`setParent(scene, child, newParent, keepWorld)` is the sanctioned way to change the parent. It
refuses self-parenting and walks the new parent's ancestry to refuse cycles — without that guard,
one bad link makes every tree traversal loop forever. With `keepWorld` (the default, and the
editor convention) the child's local TRS is rebased so its world transform does not move:

$$ local' = parentWorld^{-1} \cdot childWorld $$

The rebased matrix is decomposed back to translation/Euler/scale by `setLocalFromMatrix`. The
Euler angles come from `extractEulerAngleZYX`, not `glm::eulerAngles`: the engine composes
rotations as `Rz * Ry * Rx`, and the quaternion-based extraction is numerically unstable at ±90°
yaw — exactly the rotation an editor produces all day. The decompose is TRS-only; a sheared
parent loses its shear in the round trip, which is accepted because `TransformComponent` cannot
represent shear anyway. The same rebase serves the editor's
[preserve-children mode](../../ui-and-editor/gizmo/#preserve-children), which holds each direct
child's world pose while a parent alone is transformed.

`destroyEntity` destroys the whole subtree: descendants are gathered through the children caches
first (`registry.destroy` invalidates handles), the entity is detached from its parent's cache,
then everything is destroyed bottom-up.

## Durability and migration

The component registers like any other, but its serde emits only `{ "parent": <uuid> }` — the
generated `relationshipComponentToJson` / `FromJson` pair never touches the caches, so the
copy-entity serialize→deserialize round trip cannot duplicate live handles. The scene loader
resolves parent uuids only after every entity exists (`relinkHierarchy` at the end of
`sceneFromJson`), because a child's entry may precede its parent in the entities array.

`SceneVersion` is 3. A v1/v2 document has no Relationship key, so `relinkHierarchy` defaults every
legacy entity to root; a dangling parent uuid downgrades to root with a warning instead of failing
the load. Raw `set-component` writes of a Relationship trigger the same relink, so a cyclic parent
written over the wire is cut back to root rather than trusted.

## Skeletons ride the same tree

A skeleton is not a special structure: every glTF joint imports as an ordinary entity
(`BoneComponent` is just a filter tag), parented through the same `RelationshipComponent` as
everything else. The renderable carries a `SkinnedMeshComponent` — the mesh asset plus the
ordered joint list **by uuid** (glTF joint order, parallel to the inverse bind matrices) and a
non-serialized `boneHandles` cache that `relinkHierarchy` resolves alongside the parent links.

Skinning consumes the hierarchy's one propagation pass: after `updateWorldTransforms`,
`jointMatrices` fills the GPU palette with

$$ joint_i = world(bone_i) \cdot inverseBind_i $$

so at bind pose every palette entry is the identity, and posing a joint is just moving an
entity. The skinned node's own transform never composes in (per glTF, joints place the
vertices entirely), and the skinned draw goes through a dedicated PSO that blends the palette
per vertex (`vertexMainSkinned`, a second `VertexSkin` vertex stream, the palette on set 2
binding 1). The path is gated by `set-skinning`; v1 skinned draws render in the scene pass
only — no shadow casting, prepass, motion vectors, or ray-traced occlusion. Reparenting a
joint out of its skeleton is allowed (bones are entities) and simply changes its world matrix,
hence the deformation.

## In the code

| What | File | Symbols |
|---|---|---|
| Relationship + world-transform components | `scene.cppm` | `RelationshipComponent`, `WorldTransformComponent` |
| Cache rebuild + cycle cut | `scene.cppm` | `relinkHierarchy` |
| Per-frame flatten + accessors | `scene.cppm` | `updateWorldTransforms`, `worldMatrix`, `composeWorldMatrix` |
| Reparent + subtree destroy | `scene.cppm` | `setParent`, `destroyEntity` |
| Skeleton + joint palette | `scene.cppm` | `SkinnedMeshComponent`, `BoneComponent`, `jointMatrices` |
| Skin import + bone spawn | `geometry.cppm` · `assets.cppm` | `ImportedSkin`, `saveMeshSkinned`, `spawnSkinnedModel` |
| Skinned draw path | `renderer_pipelines.cpp` · `mesh.slang` | `requestMeshPipeline(skinned)`, `vertexMainSkinned` |
| Generated serde | `scene_component_serde.generated.cpp` | `relationshipComponentToJson`, `skinnedMeshComponentToJson` |
| Self-tests | `scene.cppm` | `runSceneHierarchySelfTest`, `runSceneSerializationSelfTest` |

## Related

- [Transform and matrices](../transform-and-matrices/) — the local TRS this hierarchy composes.
- [Serialization](../scene-serialization/) — the uuid-keyed document and the resolve pass.
- [Component registry](../component-registry/) — why unregistered derived components stay out of files.
