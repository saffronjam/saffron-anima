# Phase 4 — Node-TRS runtime application (evaluator + spawn players)

**Status:** NOT STARTED

**Depends on:** Phase 1 (track model), Phase 3 (spawn builds node forest + seeds components)

## Why

A clip with `Target::Node` tracks must drive plain entity transforms. This phase makes `tickAnimation`
write `PoseOverrideComponent`s onto nodes (not just bones), and attaches a player to non-rigged animated
subtrees at spawn — so `BoxAnimated.gltf` plays. CPU-only; no GPU change (node-TRS just moves
transforms, which the existing static-mesh draw already follows).

## Grounding

- `tickAnimation` (`animation.cpp:603-764`): today iterates `forEach<AnimationPlayerComponent,
  SkinnedMeshComponent>`, seeds rest from bones, samples, writes `PoseOverrideComponent` per bone.
- `localMatrix` (`scene.cppm:852-860`) already prefers `PoseOverrideComponent` for **any** entity, not
  just bones — node-TRS composition is free.
- `updateWorldTransforms` (`scene.cppm:914`) runs after `tickAnimation` (`host.cppm:1493`).
- `sampleClipResolved` (`animation.cpp:308-349`) resolves bone tracks by `targetName`→index; node tracks
  resolve `targetName`→entity in the player's subtree.
- `spawnSkinnedModel` already attaches a player for rigs that ship clips (`assets.cppm:4893-4899`).

## Decisions (locked)

1. **One `tickAnimation`, two iteration cases, one write seam.** Keep the single `tickAnimation`
   function. Add a second `forEach<AnimationPlayerComponent>(scene, ...)` pass that handles players
   **without** a `SkinnedMeshComponent` (the node-TRS case), OR — preferred minimal footprint — restructure
   the existing pass to branch on `hasComponent<SkinnedMeshComponent>`: if rigged, the bone path as
   today; the clip's node tracks (if any) additionally drive nodes; if not rigged, only node tracks
   apply. The *write seam* is identical in both: sample a `JointPose`, write a `PoseOverrideComponent`
   to the target entity. No second evaluator, no second component.
2. **Node target resolution by name, scoped to the player's subtree.** A `Node` track's `targetName`
   resolves to a descendant entity of the player's container (walk `RelationshipComponent.children`,
   match `NameComponent.name`). Build the name→entity map once per tick per player (mirrors the bone
   `nameToIndex` map at `animation.cpp:635-647`). Caches in `AnimationRuntime` keyed by entity uuid
   (like `lastPose`) so the walk is amortized; rebuilt on a structural change. Unresolved node tracks
   are silently skipped (same policy as unresolved bone tracks). This is the id/name-binding model the
   external research recommends (resolve durably by name within the instance subtree; survives reorder).
3. **Where the player attaches for node-TRS.** Spawn (Phase 3 extends both branches): if the model has
   animations and **no skin**, attach an `AnimationPlayerComponent` to the model **container** entity
   (the subtree root from `spawnNodeForest`), defaulting to the first clip, stopped+looping — the same
   defaults the rigged path uses (`assets.cppm:4893-4899`). The container is the natural binding scope
   for node tracks. A skinned model with *also* node tracks keeps its single player on the mesh entity;
   its node tracks resolve against the whole instance subtree.
4. **Non-destructive, reverts on stop.** Node `PoseOverrideComponent`s are removed when the player goes
   inactive (extend `clearOverrides` to also clear driven node overrides for a node-TRS player) so the
   authored `TransformComponent` rest pose is restored — identical contract to bones.
5. **Morph weights written here too.** A `Weights` track samples into `PoseBuffer.weightsLocal`
   (Phase 1) and `tickAnimation` writes a `MorphWeightOverrideComponent` (Phase 3) onto the mesh entity
   — the morph counterpart to the per-bone `PoseOverrideComponent` write. (The GPU consumes it in Phase
   5.) This keeps morph-weight animation on the one evaluator path.

## Edits

- `animation.cpp`: restructure `tickAnimation` to handle rigged + node-TRS players through one write
  seam; add node name→entity resolution (cache in `AnimationRuntime`); write
  `MorphWeightOverrideComponent` for `Weights` tracks; extend `clearOverrides` for node + morph
  overrides. `sampleClipResolved` handles node tracks.
- `animation.cppm`: `AnimationRuntime` gains the per-player node-name→entity cache (uuid-keyed, cleared
  on project load like the others, `:104-115`).
- `assets.cppm`: `spawnModel`/`spawnNodeForest` attach a container player for skinless animated models.
- Self-test: add a node-TRS case to `runAnimationSelfTest` or a host-level check (a 2-node parent/child
  clip; assert the child entity's `PoseOverrideComponent` matches the sampled value at t).

## Verification

- `make engine`; `make prepare-for-commit`.
- Import + spawn `BoxAnimated.gltf`, play via `play-animation` on the container: assert both boxes'
  world transforms animate (child follows parent through the hierarchy), via `sa get-transform` /
  outliner over several frames.
- Stop playback: node `PoseOverrideComponent`s removed, transforms revert to rest.
- Regression: a skinned rig with no node tracks behaves exactly as before.

## Risks

- **Ambiguous node names** within a subtree (glTF allows duplicate names). Resolution: resolve to the
  first match in a stable (depth-first) walk and warn once — consistent with the bone `nameToIndex`
  policy. A future id-based binding can layer on without a second path.
- **Player on the container vs mesh node** must be exactly one place per instance, or two players fight.
  The decision (container for skinless, mesh for skinned) is single-valued; assert one player per
  instance at spawn.
- `updateWorldTransforms` ordering: node overrides must be written before it runs — already true
  (`host.cppm:1493` precedes the render). No change needed; note it so a future reorder doesn't break.
