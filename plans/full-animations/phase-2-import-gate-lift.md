# Phase 2 — Import gate lift + sparse-accessor decode + node/morph channels

**Status:** NOT STARTED

**Depends on:** Phase 1 (the track model + .sanim format must exist to decode into)

## Why

This is the clean cutover the goal demands: the skin-only animation gate is **lifted** (one decode
path, no dual path), and the three channel skips (morph-weights, non-joint, sparse) are **replaced**
with real decode. After this phase, `BoxAnimated.gltf` (node-TRS, no skin) and `AnimatedMorphCube.gltf`
(morph weights, sparse deltas) both import their animation data.

## Grounding (the exact code to change)

- The gate: `if (data->skins_count > 0 && sawSkinnedPrimitive && !sawUnskinnedPrimitive)`
  (`geometry.cppm:944`). Inside it: node forest import (`:947-990`), skin payload (`:991-1019`),
  animation decode (`:1024-1106`).
- The three skips (all inside the decode loop):
  - morph-weights: `cgltf_animation_path_type_weights` → `continue` (`:1043-1048`).
  - non-joint: joint-lookup fails → `continue` (`:1049-1064`).
  - sparse: `sampler.input->is_sparse || sampler.output->is_sparse` → `continue` (`:1066-1072`).
- Accessor reads already use `cgltf_accessor_read_float/uint` (`:815,:841,:1089,:1094,:1002`).
- `geometry.cppm:807-828` bakes the node transform into vertices when `applyNodeTransform` (the
  unskinned flatten path).

## Decisions (locked)

1. **Node forest + animation decode move OUT of the skin gate; the skin payload stays gated.**
   Restructure (`:944`):
   ```
   // always: build model.nodes from data->nodes (move :947-990 out)
   // gated on skin: build model.skinDesc + model.skin + model.hasSkin (keep :991-1019)
   // always: decode animations (move :1024-1106 out, with the new per-channel routing below)
   ```
   `ImportedModel.nodes` is now populated unconditionally (its public shape changes — every consumer
   that assumed nodes-iff-skin is updated: `spawnModel`/`spawnSkinnedModel` in Phase 0/3, the `.smodel`
   metadata writer `importedNodesToJson` at `assets.cppm:2929`).
2. **`is_sparse` skip deleted; one generic sparse-accessor decode added.** cgltf's
   `cgltf_accessor_read_float/uint` already resolve sparse accessors transparently into the dense
   element they read — verify against `third_party/cgltf/cgltf.h` (`cgltf_accessor_read_float` walks
   `sparse`), then the skip is simply removed: the existing per-key read loop (`:1087-1095`) works for
   sparse samplers unchanged. If a manual path is needed for the *morph delta* accessors (Phase 3),
   implement one `readAccessorDense(accessor) -> std::vector<float>` helper: init from base bufferView
   or zeros, overlay `sparse.indices`/`sparse.values`, assert strictly-increasing indices `< count`,
   `Err` on violation (no exceptions). One helper, used by both anim samplers and morph deltas.
3. **Per-channel routing replaces the two `continue`s.** In the decode loop, for each channel:
   - `weights` path → build a `Weights` `AnimTrack` (`Target::Node`, `targetName` = mesh node name,
     `morphCount` = primitive target count) by copying the SCALAR output stream verbatim (N·M, or
     3·N·M for CUBICSPLINE).
   - T/R/S targeting a **skin joint** → `Target::Bone` track as today (`:1074-1100`).
   - T/R/S targeting a **non-joint node** → `Target::Node` track, `joint = -1`, `targetName` = node
     name. (The old `:1059` skip is gone.)
   One clip holds whatever mix of bone/node/weights tracks the glTF declares — exactly as authored.
4. **Animated unskinned models keep their node forest and are NOT baked.** Add the rule from Phase 0:
   when the model has animations (or a node forest with >1 node), import each primitive in node-local
   space (`applyNodeTransform = false`) and rely on the spawned node hierarchy to place it. A static
   unskinned model with no animation still bakes (flatten) as today — but that is the *same* decision
   point, not a second path: the flag is `bool keepNodeForest = !data->animations_count == 0 ||
   data->nodes_count > 1` computed once.
5. **NO LEGACY:** the `data->skins_count > 0 && ...` animation branch and its three skips no longer
   exist after this phase. There is one decode path producing bone+node+weights tracks.

## Edits

- `geometry.cppm` `importGltfModel`: restructure the gate (`:944`), delete the three skips
  (`:1043-1072`), add the per-channel routing, add `readAccessorDense` (sparse-safe), add the
  `keepNodeForest` rule gating `applyNodeTransform`.
- `toTrackPath` (`:515`): add the `cgltf_animation_path_type_weights` → `Path::Weights` case.
- `ImportedModel`/`ImportedMesh`: morph-delta fields are Phase 3; this phase only carries node + anim.
- Update every consumer that read `nodes` only when skinned (`assets.cppm` `importedNodesToJson`/
  `importedNodesFromJson` at `:2929,:2971` already round-trip nodes — confirm they handle a node-only,
  skinless model).

## Verification

- `make engine`; `make prepare-for-commit`.
- Import `BoxAnimated.gltf` headless (`sa import-model` / the import path): assert
  `ImportedModel.nodes.size() > 0`, `animations.size() == 1`, and the clip has `Target::Node` T/R/S
  tracks bound by node name.
- Import `AnimatedMorphCube.gltf`: assert the clip has one `Weights` track with `morphCount == 2` and
  `values.size() == 2 * keyCount` (LINEAR) — proves the weights stream + sparse read.
- Import an existing skinned rig (regression): the bone tracks still decode identically (same clip
  shape as before the gate lift).

## Risks

- **Double-transform** if a model is both flattened (baked verts) and given a node forest. The
  `keepNodeForest` rule (decision 4) is the single guard — verify on a static OBJ (must still bake) and
  `BoxAnimated` (must not bake).
- cgltf sparse behavior: confirm `cgltf_accessor_read_float` resolves sparse before deleting the skip
  (read `cgltf.h`); if it does not, the `readAccessorDense` helper is mandatory, not optional.
- `ImportedModel.nodes` now non-empty for skinless models — any code path that branched on
  `nodes.empty()` as a skin proxy is now wrong; grep and fix (use `hasSkin`).
