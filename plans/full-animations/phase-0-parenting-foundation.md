# Phase 0 — Parenting / transform-hierarchy foundation for node-TRS

**Status:** NOT STARTED

**Depends on:** —

## Why

Node-TRS animation in real glTFs (`BoxAnimated.gltf`: a parent box + a child box, both animated) only
works if the engine composes a child's world transform from its parent's. That hierarchy is **already
built** — this phase confirms it, hardens the exactly-one seam node-TRS will ride, and adds the
regression net, so later phases write into a known-good foundation rather than discovering a gap.

## Grounding (what already exists)

- `RelationshipComponent` (`scene.cppm:52-57`): durable `parent` Uuid + runtime `parentHandle`/`children`
  caches, rebuilt by `relinkHierarchy` (`scene.cppm:756-847`).
- `TransformComponent` (local TRS, `scene.cppm:42-47`) + `WorldTransformComponent` (cached, `:61-64`).
- `localMatrix(scene, entity)` (`scene.cppm:852-860`) — **already prefers `PoseOverrideComponent` over
  `TransformComponent`.** This is the single seam node-TRS playback writes into (Phase 4): a node gets a
  `PoseOverrideComponent`, and world composition picks it up with zero new code.
- `updateWorldTransforms` (`scene.cppm:914-944`) — roots-first walk, full mat4 compose, runs once/frame
  before render (`host.cppm`, after `tickAnimation`).
- `setParent` (`scene.cppm:1010-1072`), cycle/self-parent guards, keepWorld rebasing.
- `spawnSkinnedModel` (`assets.cppm:4818`) already builds one entity per `ImportedNode` and wires
  `RelationshipComponent.parent` from `node.parent` — the node forest is instantiated as a real subtree.

## Decisions (locked)

1. **No new hierarchy machinery.** Node-TRS rides `localMatrix`'s existing `PoseOverrideComponent`
   preference (`scene.cppm:854`). A driven node = a node with a `PoseOverrideComponent`, identical to a
   driven bone. This is the minimal-footprint cutover: zero new compose path.
2. **`spawnModel` for unskinned models must build the node forest too.** Today `spawnModel`
   (`assets.cppm:4972`) only calls `spawnSkinnedModel` when `hasSkin`; the unskinned branch (read the
   `:4972`+ tail) collapses the model to flat geometry and drops the node forest. For node-TRS, an
   unskinned animated model must instantiate its `ImportedNode` forest as a parented subtree (the same
   loop `spawnSkinnedModel` uses at `:4824-4843`) so node tracks have entities to bind to. This is a
   *prerequisite shape change* tracked here; the player attach itself is Phase 4. **No dual path:** the
   node-forest instantiation is factored into one helper both spawn branches call.
3. **`updateWorldTransforms` ordering is already correct** for node-TRS: it runs after `tickAnimation`
   (`host.cppm:1493`), so a node's freshly written `PoseOverrideComponent` composes the same frame.

## Edits

- `assets.cppm`: factor the node-forest-instantiation loop (`spawnSkinnedModel:4824-4843` + the
  container-wrap at `:4901-4918`) into one free function `spawnNodeForest(scene, nodes, ...) ->
  std::vector<Entity>`; call it from both `spawnSkinnedModel` and the unskinned `spawnModel` branch when
  `result.nodes` is non-empty. The unskinned branch attaches a `MeshComponent` (not
  `SkinnedMeshComponent`) on the mesh node. (Player attach deferred to Phase 4.)
- `scene.cppm`: extend `runSceneHierarchySelfTest` with a 3-deep parent chain where a mid node carries a
  `PoseOverrideComponent`, asserting the leaf's `worldMatrix` reflects the override (proves node-TRS
  composes through the hierarchy before any animation code exists).

## Verification

- `make engine`; `make prepare-for-commit`.
- Headless self-test (`runSceneHierarchySelfTest`) green with the new override-through-hierarchy case.
- Import an unskinned multi-node glTF; assert the spawned subtree has the right parent chain (the
  `BoxAnimated` node forest survives spawn) via an `se` `get-scene` / outliner check.

## Risks

- `spawnModel`'s unskinned branch may today rely on flattening (node transforms baked into vertices at
  import, `geometry.cppm:807-828` `applyNodeTransform`). Lifting to a real node forest means **not**
  baking node transforms for animated unskinned models — Phase 2 must stop applying the node transform
  for an unskinned model that has animations, or the transform is double-applied (baked into verts AND
  on the node). Locked resolution: when a model has node-TRS animation, import geometry in node-local
  space (don't bake), exactly as the skinned path already does (skinned primitives are not baked because
  joints place vertices). One rule: *animated ⇒ keep node forest, don't bake.*
