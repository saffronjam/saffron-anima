# Phase 2 — Import gate lift + correct sparse decode + node/morph channels

**Status:** NOT STARTED

**Depends on:** Phase 0 (per-node spawn forest for unskinned models), Phase 1 (generalized
`AnimTrack` with `Target{Bone,Node}` + `Path::Weights` + `targetName` + `morphCount`, `.sanim` v2,
the mode-keyed sampler that handles the N-wide morph-weights stream and the 3·N·M CUBICSPLINE layout).

## Goal

One decode path. Today `importGltfModel` (`geometry.cppm:725`) imports the node forest and the
animation clips only *inside* the skin-only gate at `geometry.cppm:944`
(`data->skins_count > 0 && sawSkinnedPrimitive && !sawUnskinnedPrimitive`), and even there it skips
three channel kinds (morph-weights, non-joint node, sparse sampler). This phase lifts node-forest
import and animation decode out of that gate so they run for *every* glTF, leaves only the skin payload
gated, and replaces the three skips with real per-channel decode. The single load-bearing correctness
fix is switching the sampler/morph reads from `cgltf_accessor_read_float` (which returns 0 for a sparse
accessor — verified `cgltf.h:2357`) to `cgltf_accessor_unpack_floats` (which inits from base-or-zero,
applies the sparse overlay, and bounds-checks — `cgltf.h:2375`, sparse second pass at `cgltf.h:2420`).

After this phase: **BoxAnimated** imports node-TRS tracks (`Target::Node`, bound by name) and
**AnimatedMorphCube** imports a `Weights` track (`morphCount == 2`, CUBICSPLINE). Sparse morph deltas
and sparse samplers (MorphStressTest / MorphPrimitivesTest) decode to non-zero values. No GPU
deformation or runtime evaluation lands here — that is Phases 3–5; this phase is importer-only.

This phase honors NO LEGACY: the three skips and the skin-only gate are *deleted*, not left as a
fallback. Every importer/asset consumer that assumed nodes/animations exist only for a rig is updated in
the same change.

## Background grounding (verified, current tree)

- The skin-only gate body spans `geometry.cppm:944`–`1107`. Inside it:
  - node-forest import: `geometry.cppm:947`–`990` (fills `model.nodes`).
  - skin payload: `geometry.cppm:991`–`1019` (`model.skinDesc`, `model.skin`, `model.hasSkin = true`).
  - animation decode: `geometry.cppm:1024`–`1106` with the three skips at `:1043` (weights), `:1059`
    (non-joint), `:1066` (sparse).
- `appendPrimitive` (`geometry.cppm` `~750`–`877`) takes `applyNodeTransform`; the unskinned multi-node
  branch (`:879`–`909`) baked `cgltf_node_transform_world` into vertices with `applyNodeTransform = true`.
  **Phase 0 already deleted that flatten path and replaced it with a per-node forest spawn.** Confirm
  Phase 0 landed before starting — this phase relies on `model.nodes` being honored at spawn for
  unskinned models (`spawnModel` at `assets.cppm:4972` routes to `spawnSkinnedModel` only when
  `result.hasSkin`; Phase 0 is what makes the unskinned-with-nodes path build a forest).
- `toTrackPath` (`geometry.cppm:515`) maps only `rotation`/`scale`/`translation`; it must gain the
  `cgltf_animation_path_type_weights` case.
- cgltf validates that every primitive in a mesh has the same `targets_count` (`cgltf.h:1681`) and that a
  weights channel's component count is `target_node->mesh->primitives[0].targets_count` (`cgltf.h:1786`).
  We still validate defensively — a malformed file can slip past `cgltf_validate`.
- `cgltf_num_components` (`cgltf.h:879`) gives the per-element float count for an accessor type; pair it
  with `cgltf_accessor_unpack_floats(acc, nullptr, 0)` (returns required float count, `cgltf.h:2379`) to
  size the dense buffer.

## Decisions (locked)

1. **Node forest + animation decode move OUT of the skin gate; only the skin payload stays gated.**
   `ImportedModel.nodes` becomes populated unconditionally — every consumer that assumed nodes-iff-skin
   is updated (step 5). The skin payload condition is unchanged.
2. **`is_sparse` skip deleted; one sparse-correct `readAccessorDense` helper added.** The read primitive
   switches from `cgltf_accessor_read_float` to `cgltf_accessor_unpack_floats` for any accessor that may
   be sparse (sampler inputs/outputs; Phase-3 morph deltas reuse the same helper). `read_float` on a
   sparse accessor returns 0 — deleting the skip without switching primitives would silently produce
   all-zero keys.
3. **Per-channel routing replaces the three skips.** Each channel routes to one of: a `Weights` track
   (`Target::Node`, `Path::Weights`, `morphCount`); a `Target::Bone` T/R/S track (target is a skin
   joint); or a `Target::Node` T/R/S track with `joint = -1` (any other node). A malformed weights/sampler
   layout, or a weights channel on a zero-target mesh, is an `Err` that aborts the import — not a skip.
4. **No double-transform invariant (`keepNodeForest` no-bake rule).** When `model.nodes` is non-empty (a
   real forest exists), no primitive was world-baked: the live local TRS carries the placement. Static
   single-mesh OBJ keeps baking into one mesh (`importObjModel`, `geometry.cppm:1127`, unchanged). The
   two are mutually exclusive — a model is *either* a baked single mesh *or* a node forest, never both.
5. **NO LEGACY:** after this phase the `data->skins_count > 0 && …` animation branch and its three skips
   no longer exist. One decode path produces bone + node + weights tracks.

## Steps (ordered, dependency-first)

### 1. Add the `readAccessorDense` helper

In the anonymous namespace next to `toTrackPath`/`toTrackInterp` (`geometry.cppm:~514`–`540`), add:

```cpp
// Reads a whole accessor into a dense float vector, applying any sparse overlay
// (cgltf_accessor_read_float returns 0 on sparse accessors — see cgltf.h:2357).
auto readAccessorDense(const cgltf_accessor& acc) -> Result<std::vector<f32>>;
```

Implementation:
- `const cgltf_size need = cgltf_accessor_unpack_floats(&acc, nullptr, 0);` (sizes the buffer).
- `std::vector<f32> out(need);`
- `const cgltf_size got = cgltf_accessor_unpack_floats(&acc, out.data(), need);`
- `if (got != need) return Err(std::format("cgltf: accessor short read ({} of {} floats)", got, need));`
  (`unpack_floats` returns 0 on a buffer-view/sparse error — `cgltf.h:2397`, `:2429`.)
- return `out`.

This one helper backs both sampler outputs and (Phase 3) morph deltas, returning `Result` per
CONVENTIONS (`std::expected`, no exceptions). Do not keep any `cgltf_accessor_read_float` call on a path
that can be sparse. The per-vertex `position`/`normal`/`texcoord` reads in `appendPrimitive`
(`:815`,`:823`,`:833`) stay on `read_float` only because base-mesh attribute accessors are never sparse
in practice — but the sampler input/output reads, and (Phase 3) the morph-target attribute reads, go
through `readAccessorDense`. (`cgltf_accessor_read_index` at `:854` is unchanged; indices are not floats.)

### 2. Split the gate: node forest + animation decode run unconditionally

Restructure `geometry.cppm:944`–`1107` into three independent regions instead of one big `if`:

- **Node forest (always).** Move `:947`–`990` out of the `if`, to run right after `appendPrimitive`
  finishes, for every model. It fills `model.nodes` from `data->nodes` (name fallback `"Node {n}"`,
  parent index, TRS via `node.matrix` decompose or T/R/S, glTF→glm quat order). Keep the `glm::decompose`
  and quat-order comments. After this, `model.nodes.size() == data->nodes_count` for any glTF.
- **Skin payload (gated, unchanged condition).** Keep `:991`–`1019` under
  `data->skins_count > 0 && sawSkinnedPrimitive && !sawUnskinnedPrimitive`; still sets `model.skinDesc`,
  `model.skin`, `model.hasSkin = true`. Drop the now-redundant `model.nodes.reserve` (the forest region
  already filled nodes). The mixed-primitive warning at `:1108` stays.
- **Animation decode (always).** Move `:1024`–`1106` out of the `if`, to run for every model with
  `data->animations_count > 0`. Per-channel routing is rewritten in step 3.

The old "is this node a joint" lookup naturally returns `joint == -1` for an unskinned model (its
`model.skinDesc.joints` is empty), so a TRS channel routes to `Target::Node` with no special-casing.

### 3. Delete the three skips; route per channel into the generalized track

Replace the per-channel body (`:1039`–`1100`). For each `cgltf_animation_channel channel` with a valid
`target_node` and `sampler` (keep the `:1039` null guard):

- **Weights channel** (`channel.target_path == cgltf_animation_path_type_weights`): build a
  `Target::Node`, `Path::Weights` track.
  - Resolve the target mesh `const cgltf_mesh* m = channel.target_node->mesh;`. If `m == nullptr` or
    `m->primitives_count == 0` or `m->primitives[0].targets_count == 0`, this is a **weights channel on a
    zero-target mesh** → `return Err(...)` (reject, do not warn-skip).
  - `const u32 morphCount = static_cast<u32>(m->primitives[0].targets_count);` Set `track.morphCount =
    morphCount` (Phase 1 added `morphCount` to `AnimTrack`).
  - Decode sampler input (times) and output (weights) via `readAccessorDense`. The output is SCALAR-typed
    but logically N-wide per key; copy it **verbatim** into `track.values`.
  - **Validate the layout against interpolation** (what Phase 1's sampler relies on): with `N =
    times.count`, `M = morphCount`, require `output.count == N*M` for LINEAR/STEP, `output.count ==
    3*N*M` for CUBICSPLINE; else `return Err(std::format("cgltf: '{}' clip '{}' weights output {} != N*M
    ({}) or 3*N*M", path, clip.name, output.count, N*M))`. Set `track.interp` from the sampler.
  - `track.target = Target::Node; track.targetName = nodeName; track.joint = -1;
    track.path = Path::Weights;` `targetName` is the node carrying the morph mesh (the durable binding key
    Phase 4 resolves to a Uuid).
- **T/R/S channel** (`rotation`/`translation`/`scale`): node index
  `static_cast<i32>(channel.target_node - data->nodes)`, looked up in `model.skinDesc.joints`.
  - **On a skin joint** (`joint >= 0`): `track.target = Target::Bone; track.joint = joint;
    track.targetName = model.nodes[nodeIndex].name;` (keep the durable name).
  - **On a non-joint node** (`joint < 0`): `track.target = Target::Node; track.joint = -1;
    track.targetName = model.nodes[nodeIndex].name;` This is the deleted "non-joint" skip — now a real
    `Target::Node` TRS track (BoxAnimated's case). An unskinned model has an empty `joints` list, so every
    TRS channel routes here.
  - `track.path = toTrackPath(channel.target_path); track.interp = toTrackInterp(sampler.interpolation);`
  - Component count: 4 for rotation (quat), else 3. Decode `sampler.input` → `track.times` and
    `sampler.output` → `track.values` via `readAccessorDense`. For CUBICSPLINE copy verbatim (the Phase 1
    sampler understands the 3×-per-key layout); do not de-tangent here.
  - Validate `output.count == components * N` (LINEAR/STEP) or `3 * components * N` (CUBICSPLINE); `Err`
    on mismatch (same defensive shape as the weights check).

Delete the morph-weights `logWarn` skip (`:1045`), the non-joint `logWarn` skip (`:1061`), and the
sparse guard + `logWarn` skip (`:1066`–`1071`) **entirely** — sparse now decodes through
`readAccessorDense`. Keep the per-clip `clip.duration = max(track.times.back())` accumulation (`:1096`)
and the `clip.tracks.empty()` filter (`:1102`). Note the new strictness: a malformed channel now
`return Err`s and aborts the whole import — it does not silently drop one channel. A broken file is
rejected loudly, not half-imported (NO LEGACY).

### 4. Add the `Path::Weights` case to `toTrackPath`

In `toTrackPath` (`geometry.cppm:515`):

```cpp
if (path == cgltf_animation_path_type_weights)
{
    return AnimTrack::Path::Weights;
}
```

`Path::Weights` is the enum value Phase 1 added. This is the only control-surface touch this phase; no
new control command.

### 5. Update importer/asset consumers that gated nodes/animations on skin

Lift every assumption that `model.nodes` / `model.animations` are populated only for a rig:

- `importedNodesToJson` (`assets.cppm:2929`, called unconditionally from `assets.cppm:3251`): already
  writes `meta.nodes` always; `meta.skin` only when `graph.hasSkin` (`assets.cppm:3252`) — correct.
  Verify the BoxAnimated import round-trips `meta.nodes` for an unskinned model.
- Animation sub-asset emit: clips are emitted near the model-bake region (`assets.cppm:~3135`+). Remove
  any `hasSkin`/`rigged` guard so a non-rigged model with `model.animations` still emits its `.sanim`
  sub-chunks; route every clip (bone-only, node-only, or weights) through `saveAnimationToBuffer`.
- `assets.cppm:2236` (`!rigged->hasSkin || rigged->animations.empty()`): the rig-editor eligibility check
  stays keyed on `hasSkin` — an unskinned-but-animated model is not a rig and does not open in the rig
  editor. Confirm BoxAnimated's clips still reach the model's own clip list.
- The `rigged` catalog flag (`assets.cppm:3050`, `!meta.skin.is_null()`) stays keyed on skin — correct.
- `spawnModel` (`assets.cppm:4972`): the unskinned-with-nodes forest spawn is Phase 0's; Phase 2 only
  guarantees `result.nodes` is populated for unskinned models. Confirm Phase 0 consumes `result.nodes`.

Search broadly (`grep -rn "hasSkin\|\.nodes\b\|\.animations\b\|nodes.empty()\|rigged"
engine/source/saffron/assets engine/source/saffron/geometry`) and fix every site that branched on the
skin existing (or on `nodes.empty()` as a skin proxy — now wrong). Each fix lands in this same change.

### 6. `.sanim` v2 already carries `Target` + `morphCount` (Phase 1)

No format change here. Confirm `saveAnimationToBuffer`/`loadAnimationFromBytes` (`geometry.cppm:1619`,
`:1657`) round-trip the new fields Phase 1 added (`SANimTrackRecord` carries the `Target` kind and
`morphCount`, `AnimFormatVersion == 2`). The decode in this phase writes those fields; the regression
test asserts a skinned rig's bone tracks still round-trip byte-identically at v2.

## Backend changes (summary)

- `geometry.cppm`: add `readAccessorDense`; split the `:944` gate into forest(always) / skin(gated) /
  anim(always); rewrite per-channel routing into `Target::Bone` / `Target::Node` / `Path::Weights`;
  delete the three skips; add the `Path::Weights` case to `toTrackPath`; replace every sampler/morph
  float read with `readAccessorDense`.
- `assets.cppm`: remove the rig/skin gating on animation sub-asset emit; verify node-forest serialize for
  unskinned models; no `rigged`-flag change.

All fallible decode returns `Result<T>` (`std::expected`); a malformed weights/sampler layout or a
zero-target weights channel is an `Err` that aborts the import. No new module edges (Phase 2 stays within
`Saffron.Geometry` + `Saffron.Assets`, both already in the DAG).

## Frontend changes

None this phase. The Timeline/Clips/Inspector surfacing of node-TRS and morph tracks is Phases 7–8
(behind the control commands). Phase 2 is importer-only and produces no new wire state to display.

## Performance

Import-time only, no per-frame cost.
- `cgltf_accessor_unpack_floats` decodes a sparse accessor **once** into a dense vector; the per-key copy
  loop then slices that dense vector. Strictly cheaper than the old per-element `read_float` loop (a
  function call per key) and the only correct way to resolve sparse data.
- Memory: one transient dense `std::vector<f32>` per accessor, freed when the track is built. Bounded by
  the largest sampler output (typically small). No retained allocation.

## Control commands

None added this phase. `toTrackPath` gains the `weights` case (a pure decode-mapping change, not a wire
command). No DTO change, so the control-schema contract test and `bun run check` are unaffected — still
run them in the milestone gate to confirm nothing regressed.

## Docs

Defer the morph and node-TRS *concept* pages to their runtime phases (Phase 4 node-TRS runtime, Phase 5
morph deform, Phase 9 docs+e2e). This phase changes only the importer's internal decode, not a
user-facing engine concept, so no `docs/` page lands now. (If the `.sanim` v2 format page
`docs/content/explanations/geometry-and-assets/sanim-format.md` was not already updated when Phase 1
shipped the format bump, it must be updated in that phase — not deferred here.)

## Tests

Add e2e tests under `tests/e2e/` driving a headless engine over the control plane (`tests/e2e/harness.ts`,
fixtures in `tests/e2e/fixtures/`). Generate glTF fixtures programmatically where feasible (mirror
`tests/e2e/fixtures/gen_leg.py`) or vendor the Khronos sample assets (BoxAnimated, AnimatedMorphCube,
MorphStressTest, MorphPrimitivesTest) into `tests/e2e/fixtures/`. Drive import via `import-model` (as
`model_flow.test.ts:42`) and inspect via the model-query commands.

New `tests/e2e/animation-import.test.ts` (or extend `model_flow.test.ts`):

1. **BoxAnimated node-TRS import.** Import BoxAnimated; assert `nodes > 0`, `animations == 1`, the clip
   has `Target::Node` T/R/S tracks bound by node name (assert a track `targetName` matches a known
   animated node), no `Target::Bone` tracks, and `hasSkin == false`.
2. **AnimatedMorphCube weights import.** Assert exactly one `Weights` track, `morphCount == 2`, interp
   `CubicSpline`, `values.size() == 3 * 2 * keyCount` (3×N×M). Add a LINEAR weights fixture and assert
   `values.size() == 2 * keyCount` (N×M).
3. **Sparse decode is non-zero.** Import MorphStressTest and MorphPrimitivesTest; assert the morph deltas
   / sparse sampler outputs decode to non-zero counts and at least one non-zero value (proves the sparse
   overlay applied — the old `read_float` path would be all-zero).
4. **Rejection cases.** A glTF with multiple primitives of *differing* `targets_count` (minimal crafted
   fixture) imports with `Err`. A weights channel targeting a *zero-target* mesh imports with `Err`.
   Assert `import-model` returns the error and no partial asset is created.
5. **Regression: skinned rig unchanged.** Import an existing skinned fixture (`skinned-strip.gltf` or
   `leg.gltf`); assert its bone tracks decode identically (same track count, `Target::Bone`, values) —
   byte-compare the `.sanim` sub-chunk against a golden, or assert values within epsilon. Pins that the
   gate lift did not perturb the skinned path.
6. **No double-transform.** Import a static multi-mesh OBJ and confirm vertices still world-bake into one
   mesh (OBJ path unchanged). Import BoxAnimated and confirm its mesh vertices are *local*, not
   world-baked (assert the mesh AABB matches local primitive bounds, not the transformed-into-world
   bounds). Pins the `keepNodeForest` no-bake invariant.

Re-run the existing animation/skinning e2e suites (`animation.test.ts`, `skinning.test.ts`,
`skinned-motion.test.ts`, `skinned-rt.test.ts`, `model_flow.test.ts`) and confirm green — the gate lift
must not regress any rigged flow.

## Acceptance criteria

- The skin-only animation gate (`geometry.cppm:944`) is gone; node-forest import and animation decode run
  unconditionally, only the skin payload stays gated. One decode path produces bone + node + weights
  tracks.
- Sparse accessors decode via `cgltf_accessor_unpack_floats` through the single `readAccessorDense`
  helper; sparse morph deltas and sparse samplers read correctly (non-zero), never all-zero.
- BoxAnimated imports `Target::Node` T/R/S tracks bound by node name; `nodes > 0`, `animations == 1`,
  `hasSkin == false`.
- AnimatedMorphCube imports one `Weights` track with `morphCount == 2`, CUBICSPLINE, `values.size() ==
  3*2*keyCount`; a LINEAR weights fixture yields `2*keyCount`.
- A multi-primitive `targets_count` mismatch and a zero-target weights channel are each rejected with
  `Err` (no partial asset).
- An existing skinned rig's bone tracks decode identically (regression golden passes).
- No double-transform: static OBJ still world-bakes into one mesh; BoxAnimated does not bake (local
  verts, the `keepNodeForest` no-bake invariant holds).
- The three skips (morph-weights, non-joint, sparse) are deleted from the source, not left as fallbacks;
  no `hasSkin`/`rigged` guard remains on node-forest or animation handling in the importer/asset path.
- `make engine` builds clean; `make prepare-for-commit` (format + lint) is clean with zero new warnings;
  the e2e suite above is green.
