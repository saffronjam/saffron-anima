# Phase 0 — Parenting foundation: per-node spawn for unskinned models

**Status:** NOT STARTED

**Depends on:** —

## Goal

Give an unskinned, multi-node glTF a live transform hierarchy to drive. Today such a model is
flattened: every primitive is baked into world space via `cgltf_node_transform_world` and the whole
thing spawns as a single `MeshComponent` entity — there are no per-node entities and no local
`TransformComponent`s for a future node-TRS animation to write onto. This phase deletes that
flatten-into-world bake and instantiates a per-node entity forest instead (one entity per
`ImportedNode`, local TRS un-baked, `RelationshipComponent.parent` wired, `relinkHierarchy` after).

This is purely the import/spawn gap. The compose math already exists and is unchanged:
`localMatrix` (scene.cppm:852) already prefers a `PoseOverrideComponent` over the
`TransformComponent` for **any** entity, and `updateWorldTransforms` (scene.cppm:914) already walks
roots-first composing `parentWorld * localMatrix`. Once the forest exists with live locals, Phase 4's
node-TRS evaluator can drive any node by attaching a `PoseOverrideComponent` to it — with zero new
compose code.

A single-node static model must still collapse to exactly one entity (no regression), and no node
transform may end up baked into vertices for the multi-node case.

## Why this is the right shape (NO LEGACY)

- The flatten path is **deleted**, not skipped or kept "for unskinned". Keeping it alive next to the
  forest path would double-transform a node (baked world in vertices *and* a live local on the
  entity). The canonical design records this as the highest-regression-risk change, guarded by the
  single/multi/animated self-test trees.
- `ImportedModel.nodes` becomes populated for skinless models too (it was previously a de-facto skin
  marker because only the skin gate filled it). Every reader that branched on
  `ImportedModel.nodes.empty()` as a skin proxy must switch to `hasSkin`, or it will misclassify a
  skinless forest as skinned.
- One `spawnNodeForest` helper is the single code path for "instantiate the node entities + wire
  parents"; both `spawnModel` and `spawnSkinnedModel` call it instead of each hand-rolling the loop.

## Grounding (real files / symbols)

- `engine/source/saffron/geometry/geometry.cppm`
  - The unskinned bake to delete: `data->skins_count == 0` branch, the `sawMeshNode` loop
    (lines ~879–898) calling `appendPrimitive(prim, nodeTransform, /*applyNodeTransform=*/true)`
    with `cgltf_node_transform_world(&node, matrix)` at line ~891; the `appendPrimitive`
    `applyNodeTransform` body bakes position+normal at lines ~807–828.
  - The skin gate that currently populates `model.nodes` (lines ~943–989); `ImportedModel.nodes`
    (line ~181); `ImportedNode` struct (lines ~115–122).
  - The skin-proxy read at line ~1994 (`first->nodes.size() == second->nodes.size()`) and the
    `applyNodeTransform` parameter on `appendPrimitive`.
- `engine/source/saffron/assets/assets.cppm`
  - `spawnSkinnedModel` node-forest loop (lines ~4824–4843); `spawnModel` (lines ~4972–4982);
    `ModelSpawnInput` (lines ~139–151, already carries `nodes`/`hasSkin`);
    `instantiateModel` (lines ~4989–5063) reconstructs `result.nodes` from metadata at line ~5048;
    `runInstantiateSelfTest` (line ~5068).
- `engine/source/saffron/scene/scene.cppm`
  - `TransformComponent` (42), `RelationshipComponent` (52), `PoseOverrideComponent` (123),
    `localMatrix` (852), `composeWorldMatrix` (864), `updateWorldTransforms` (914),
    `relinkHierarchy`, `setParent`, `runSceneHierarchySelfTest` (1689, ends 1837),
    `worldMatrix`/`worldTranslation` (883/892).
- `engine/source/saffron/host/host.cppm:1315` calls `runSceneHierarchySelfTest()` at startup.

## Steps (ordered)

### A. Geometry: delete the flatten bake, populate `nodes` for skinless models

1. In `importGltfModel` (geometry.cppm), in the `data->skins_count == 0` branch, **stop baking node
   transforms into vertices**: change the `sawMeshNode` loop so `appendPrimitive` is called with
   `applyNodeTransform = false` and `nodeTransform = glm::mat4(1.0f)`. Each primitive's vertices then
   keep their authored object-space positions/normals; the node's TRS lives on its entity instead.
2. Once nothing passes `applyNodeTransform = true`, **delete the parameter and its body**:
   - Remove the `applyNodeTransform` argument from `appendPrimitive`.
   - Delete the `normalTransform` setup (geometry.cppm:806–810) and the two
     `if (applyNodeTransform)` position/normal bake blocks (geometry.cppm:817–828).
   - Delete the now-unused `cgltf_node_transform_world` call (geometry.cppm:891) and the local
     `nodeTransform` `memcpy` it fed.
   This is a real deletion (NO LEGACY), not a dead `if (false)`.
3. **Populate `model.nodes` for skinless models.** Factor the node-decode loop currently inside the
   skin gate (geometry.cppm:947–989 — name, parent index, TRS-from-matrix-or-components) into a small
   local helper, e.g. `decodeNodeForest(data)` returning `std::vector<ImportedNode>`, and call it
   **unconditionally** so `model.nodes` is filled regardless of `hasSkin`. The skin gate keeps using
   the same `model.nodes` it already had; the skinless path now gets the forest it lacked. Do **not**
   touch animation decode in this phase (still skin-gated; Phase 2 lifts that).
4. **Single-node static stays one entity.** Do not force a forest for the trivial case. Compute the
   keep-forest decision once and stash the intent so spawn can collapse:
   - Add a helper, e.g. `auto keepNodeForest(const ImportedModel&) -> bool` (or an inline predicate
     in spawn) defined as `nodes.size() > 1 || !animations.empty()`. A single-node static model
     (`nodes.size() <= 1 && animations.empty()`) is *not* a forest.
   - To keep a single-node static model visually identical after we stopped baking, the **one
     surviving entity must carry that node's transform**. Two acceptable ways, pick the simpler:
     (a) leave `model.nodes` with the single node and let `spawnModel` apply that node's local TRS to
     the one `MeshComponent` entity; or (b) for the single-mesh-node case continue baking *only* that
     node's transform into the mesh as today. Prefer (a) — it removes the bake entirely and the live
     local matches the forest semantics. Either way the acceptance "geometry carries its node
     transform — no regression" must hold: a single-node model with a non-identity node TRS renders in
     the same world placement as before.
5. **Kill the `nodes.empty()` skin proxy.** Grep the geometry module (and the whole tree) for any use
   of `ImportedModel.nodes.empty()` / `nodes.size()` as a stand-in for "is this skinned" and replace
   it with `hasSkin`:
   - geometry.cppm:1994 compares `first->nodes.size() == second->nodes.size()` alongside
     `first->hasSkin == second->hasSkin`; keep the size compare only if it is a genuine structural
     equality check (it now legitimately differs for skinless forests), otherwise lean on `hasSkin`.
   - Run `grep -rn "nodes.empty()\|->nodes.size()\|\.nodes\.size()" engine/source` and audit each hit:
     a skin decision → `hasSkin`; a real forest-size check → leave it.

### B. Assets: one `spawnNodeForest` helper, both spawn branches call it

6. Add a free helper in assets.cppm (anonymous namespace, beside `spawnSkinnedModel`):

   ```cpp
   // Instantiates one entity per ImportedNode with its local TRS on TransformComponent
   // (NOT baked into geometry), wires RelationshipComponent.parent by uuid, and returns the
   // per-index entity + uuid handles. Caller runs relinkHierarchy after attaching mesh/skin.
   struct NodeForest { std::vector<Entity> entities; std::vector<Uuid> uuids; };
   auto spawnNodeForest(Scene& scene, const std::vector<ImportedNode>& nodes) -> NodeForest;
   ```

   Body = the current `spawnSkinnedModel` loop (assets.cppm:4824–4843): create an entity per node,
   set `TransformComponent.translation/rotation(=quatToEulerZYX(node.rotation))/scale`, collect the
   `IdComponent.id` uuids, then in a second pass set `RelationshipComponent.parent` from the parent
   index → parent uuid. No `relinkHierarchy` inside the helper (the caller does it after wiring
   mesh/skin so it resolves joint handles in the same pass).
7. **`spawnSkinnedModel` calls the helper.** Replace its inline node loop (4824–4843) with
   `NodeForest forest = spawnNodeForest(scene, result.nodes);` and use `forest.entities` /
   `forest.uuids` for the bone/mesh-node/container wiring below it. Behavior is identical — this is
   the dedupe.
8. **`spawnModel` uses the forest for unskinned multi-node / animated models** the way
   `spawnSkinnedModel` does:

   ```cpp
   auto spawnModel(Scene& scene, std::string name, const ModelSpawnInput& result) -> Entity
   {
       if (result.hasSkin)
       {
           return spawnSkinnedModel(scene, std::move(name), result);
       }
       const bool forest = result.nodes.size() > 1 || !result.animations.empty();
       if (!forest)
       {
           Entity entity = createEntity(scene, std::move(name));
           if (result.nodes.size() == 1) { /* apply the single node's local TRS */ }
           addComponent<MeshComponent>(scene, entity).mesh = result.mesh;
           applyImportedMaterials(scene, entity, result);
           return entity;
       }
       // forest path: spawnNodeForest, attach MeshComponent to the mesh-bearing node(s),
       // wrap under a container root (mirror spawnSkinnedModel's container block), relinkHierarchy.
   }
   ```

   - The forest path mirrors `spawnSkinnedModel`'s container wrap (assets.cppm:4901–4920): create a
     container entity named after the model, reparent every root node under it, then
     `relinkHierarchy(scene)`. A model instance stays a single selectable/destroyable subtree.
   - Attach `MeshComponent` to the node(s) that carry the mesh. For v1 the importer bakes all
     primitives into one `Mesh` (one mesh sub-asset), so attach the single `MeshComponent` to the
     node identified as the mesh-bearing node (the skinless analogue of `skinDesc.meshNode`); if the
     importer does not record a mesh node for the skinless case, attach it to the first node that had
     a glTF mesh, or to the container as a documented fallback. Whichever rule is chosen, encode it
     once and assert it in the self-test (multi-node tree carries exactly one `MeshComponent`).
   - No `AnimationPlayerComponent` is attached this phase (no skinless animation decode yet — Phase 2
     populates `result.animations` for skinless models and Phase 4 attaches the player). The
     `!result.animations.empty()` half of the forest predicate is wired now so the decision is
     computed once and ready; for this phase `result.animations` is empty for skinless models, so the
     forest is taken on `nodes.size() > 1` alone.
9. **`instantiateModel` already feeds `result.nodes`** (assets.cppm:5048) and `result.hasSkin`
   (5049–5052), so a baked container of an unskinned multi-node model now reconstructs the forest for
   free. Verify the reconstructed `ModelSpawnInput.nodes` is non-empty for a multi-node skinless
   container and add coverage in `runInstantiateSelfTest` (see Tests).

### C. Verify the module DAG and error model are respected

10. No new module edges. `Saffron.Geometry` keeps decoding into POD `ImportedModel`; `Saffron.Assets`
    keeps the spawn logic. Fallible paths return `std::expected`/`Result<T>` as today
    (`importGltfModel` already returns `Err(...)`); the new helper is infallible (pure scene
    mutation) and needs none.

## Frontend (Timeline / Clips / Inspector)

None. The outliner already renders the hierarchy from `RelationshipComponent`, so a multi-node static
model simply shows up as a tree instead of a single row. No editor code changes; do **not** add a
parallel UI. (Spot-check that the outliner renders the new forest correctly during the gate run.)

## Control commands

None. This phase adds no new drivable engine state — node-TRS playback and its commands arrive in
Phase 4/7. The existing scene/outliner already exposes the spawned hierarchy.

## Performance

No per-frame cost change in kind. A multi-node static model now spawns N entities with N world
matrices composed by the existing `updateWorldTransforms` — negligible and already exactly the
skinned-model cost (skinned imports have spawned the same forest all along). Import-time work is the
same node decode that the skin path already ran. No new GPU work, no new buffers, no render-graph
changes.

## Docs

None authored this phase. Note in the Phase 9 docs edit list that
`docs/content/explanations/geometry-and-assets/` (the geometry-import / mesh-format explanation) must
record the spawn change: an unskinned multi-node glTF spawns a per-node entity forest with live local
transforms rather than a single flattened mesh, and node transforms are no longer baked into vertices
for that case. (`smesh-format.md` itself is untouched here — vertex/index layout is unchanged; only
which space the positions are stored in for the multi-node case differs, and that is an import policy,
not a format change.)

## Tests

### Extend `runSceneHierarchySelfTest` (scene.cppm:1689)

Add three trees that prove node-TRS composes through the existing hierarchy **before any animation
code exists**. Use the existing `expect`/`nearEqual` helpers and `worldMatrix`/`worldTranslation`.

1. **Single-node static → one entity carrying the node transform.** Build a one-node model intent
   (or directly: a single entity with a non-identity `TransformComponent`), spawn/compose, and assert
   it is a single entity whose `worldMatrix` equals its local `transformMatrix` (no container, no
   children). Mirrors the acceptance "single-node static still spawns exactly one entity".
2. **Multi-node static → a forest, child world = parent × child local.** Build parent→child entities
   with distinct non-identity locals, `setParent`, `updateWorldTransforms`, and assert
   `worldMatrix(child) == worldMatrix(parent) * transformMatrix(child local)` and the expected leaf
   world translation. (The existing parent/child/grandchild block at 1716–1732 already demonstrates
   the pattern — add an assertion explicitly framed as the spawned-forest contract.)
3. **3-deep chain with a mid-node `PoseOverrideComponent`.** Build root → mid → leaf with authored
   locals, attach a `PoseOverrideComponent` to **mid** with a translation/rotation distinct from its
   `TransformComponent`, `updateWorldTransforms`, and assert the **leaf** `worldMatrix` reflects the
   override (i.e. equals `worldMatrix(root) * localMatrix(mid via override) * localMatrix(leaf)`),
   and that mid's authored `TransformComponent` is untouched. This proves node-TRS composes before
   any animation evaluator exists, since `localMatrix` already prefers the override.

Where a test needs the actual spawn path (not hand-built entities), prefer driving
`spawnModel`/`spawnNodeForest` with a synthetic `ModelSpawnInput` (as `runInstantiateSelfTest`
already does for `ImportedModel`) so the forest-vs-collapse decision itself is exercised.

### Extend `runInstantiateSelfTest` (assets.cppm:5068)

Add a **multi-node skinless** case alongside the existing flat/rigged ones: an `ImportedModel` with
`hasSkin = false` and two nodes (root + child, distinct TRS), no skin. Bake → `instantiateModel` and
assert: the instance is a forest (a container root with the node subtree, not a lone `MeshComponent`
entity), exactly one `MeshComponent` in the subtree, the child's parent uuid resolves to the root,
and `ModelInstanceComponent` marks the container root. This pins the new `spawnModel` forest branch
and the `instantiateModel` reconstruction together.

### e2e

No new `tests/e2e` case required this phase (no control surface added). The two self-tests above run
inside the present-only host at startup (`host.cppm:1315`) and fail the smoke if broken. The
present-only static-scene smoke must stay clean.

## Acceptance criteria

- A multi-node unskinned glTF spawns a per-node entity forest with live local `TransformComponent`s
  under a single container root; **no node transform is baked into vertices** for that case (the
  `applyNodeTransform=true` path and `cgltf_node_transform_world` call are gone from geometry.cppm).
- A single-node static model still spawns exactly one entity, placed in the same world position as
  before (its node transform is carried on that entity's `TransformComponent` — no double-transform,
  no regression).
- `runSceneHierarchySelfTest` passes the single / multi / animated-node (parent-override compose)
  cases, including the mid-node `PoseOverrideComponent` leaf-world assertion.
- `runInstantiateSelfTest` passes the new multi-node skinless instantiate case.
- No code branches on `ImportedModel.nodes.empty()` (or `nodes.size()`) as a skin proxy; skin
  decisions use `hasSkin`.
- One `spawnNodeForest` helper is the sole node-instantiation path; `spawnSkinnedModel` and the
  `spawnModel` forest branch both call it (no duplicated node loop).
- `make engine` builds clean and `make prepare-for-commit` (format + lint) is warning-clean for this
  change; the present-only static-scene smoke is clean.

## Hand-off to Phase 1 / 2 / 4

- Phase 2 lifts the animation-decode gate so `result.animations` is populated for skinless models;
  the `spawnModel` forest predicate already includes `!animations.empty()`, so an animated skinless
  model takes the forest path automatically once decode lands.
- Phase 4 attaches an `AnimationPlayerComponent` to the skinless animated subtree and drives node
  `PoseOverrideComponent`s through `tickAnimation` — the live local `TransformComponent`s this phase
  creates are exactly what it overrides.
