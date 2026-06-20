# Phase 3 — Morph delta storage + .smesh flags collapse + spawn seeding

**Status:** NOT STARTED

**Depends on:** Phase 2 (the import gate is lifted; `weights` animation channels and sparse accessors
already decode. This phase consumes the decoded morph-target *geometry* accessors and persists them.)

## Why

Phase 2 made `AnimatedMorphCube.gltf` import its `weights` *animation channel*. This phase imports and
persists the morph *geometry* — the per-target vertex deltas — and gives every morph mesh a durable
per-entity weight vector. Deltas are stored sparsely in a flags-collapsed `.smesh`; two scene components
carry the weights: a durable `MorphComponent` (authored/seeded) and a runtime-only
`MorphWeightOverrideComponent` (the Phase-4 evaluator's write target, mirroring `PoseOverrideComponent`).

Everything downstream consumes this phase: Phase 4 writes the runtime override each tick, Phase 5 uploads
the delta bank and runs the GPU morph stage, Phase 7 adds the `set/get-morph-weights` command. No GPU work
and no evaluator work lands here — this is the storage + spawn-seeding layer only.

This is the **NO-LEGACY format cutover**: the two mesh-version constants collapse into one version + a
flags word in the same change, every caller and self-test moves with it, and v1/v2 files are rejected
with `Err`. There is no migration of old `.smesh`/`.smodel` files (clean-slate — reimport the source).

## Grounding (the exact code to change)

- `Vertex` (geometry.cppm:36-41) — `position`/`normal`/`uv0`, no tangent stream, `static_assert
  sizeof(Vertex)==32`. This is *why* `MorphDelta` is NORMAL-only: a serialized `dTangent` would be dead
  bytes nothing reads.
- `Mesh` (geometry.cppm:54-59) — gains a `morphTargets` member; `VertexSkin` (geometry.cppm:63-67) is the
  parallel-stream precedent (morph deltas are a *third* parallel section, never interleaved into Vertex).
- `Submesh.vertexOffset` (geometry.cppm:49) — morph delta `vertexIndex` is offset by this so a delta
  indexes the merged `Mesh.vertices` array, not a per-submesh primitive range.
- `MeshFormatVersion = 1` / `MeshFormatVersionSkinned = 2` (geometry.cppm:136-139) — collapse to ONE
  `MeshFormatVersion`.
- `SMeshHeader` (geometry.cppm:386-401) — 64 bytes; has `flags` ("reserved (0)") and `reserved[2]`. Spend
  `flags` for skin/morph bits and one reserved u32 for `morphTargetCount`; re-assert `sizeof == 64`.
- `static_assert(sizeof(VertexSkin) == 24, ...)` and `static_assert(sizeof(Submesh) == 16, ...)`
  (geometry.cppm:380-381) — the same baked-stride discipline applies to `MorphDelta` (28 B).
- `encodeMeshImage` (geometry.cppm:1401-1443) — picks the version by skin presence today; rewrite to set
  flags + append the morph section.
- `loadMeshFromBytes` (geometry.cppm:1477-1519) — accepts version 1 OR 2 (the `!= MeshFormatVersion &&
  != MeshFormatVersionSkinned` test at :1489); replace with flags-based branching, reject any version !=
  `MeshFormatVersion`.
- `loadMeshSkinFromBytes` (geometry.cppm:1577-1602) — branches on `version != MeshFormatVersionSkinned`
  with a v1 "empty skin" fallthrough (:1589-1592); replace with the skin-bit test, and shift the skin
  offset past the new morph section.
- `saveMeshToBuffer` / `saveMeshSkinnedToBuffer` (geometry.cppm:1461-1475) — the public encode entry
  points; both must carry morph targets.
- `meshCountsFromBytes` (geometry.cppm:1552-1565) / `meshFileCounts` (geometry.cppm:1536-1550) — only
  read counts; the header reshape must not break the `memcpy(&header, ...)` (keep the field order such
  that `magic`/`version`/counts stay where these readers expect, or update both readers in lockstep).
- `.smodel` container: `SModelHeader` (geometry.cppm:296-312) embeds the `.smesh` chunk **verbatim** by
  fourcc TOC; `runContainerSelfTest` (geometry.cppm:2024-2095). **No container bump** — the chunk payload
  is opaque to the container.
- `runGeometrySelfTest` (geometry.cppm:2186+) — the `.smesh` round-trip block at :2207-2228, the
  animated-strip block at :2230-2263. Extend with a morph round-trip + a wrong-version reject.
- Scene templates: `SkinnedMeshComponent` (scene.cppm:79-86, import-managed) and `PoseOverrideComponent`
  (scene.cppm:123-128, runtime-only). `SceneVersion = 3` (scene.cppm:1200), `sceneFromJson` migration
  (scene.cppm:1411-1428), serde forward decls (scene.cppm:1176-1177),
  `runSceneSerializationSelfTest` (scene.cppm:1501+).
- Component registration: `registerBuiltinComponents` in `scene_edit_components.cpp` — the `SkinnedMesh`
  row at :108-109 is the import-managed (`removable=true` but never user-added) template; the
  non-removable `Relationship` row at :102-104 shows the `false` flag form.
- Serde body generator: `emitSceneSerde()` in `tools/gen-control-dto/gen.ts` (the SkinnedMesh / vector
  bodies are the template); it writes `engine/source/saffron/scene/scene_component_serde.generated.cpp`
  — **edit gen.ts, never the file.**
- Spawn: `spawnSkinnedModel` (assets.cppm:4818-4970), `spawnModel` (assets.cppm:4972-4982),
  `ModelSpawnInput` (assets.cppm:139-151), `instantiateModel` (assets.cppm:4989+, rebuilds
  `ModelSpawnInput` from the container `MetadataChunk`).
- `GpuMesh` (renderer_types.cppm:228+) + `uploadMesh` (renderer_drawlist.cpp:76-214) — the morph
  delta-bank upload is **declared/noted only** here (no SSBO yet; Phase 5 wires it).

## Decisions (locked — consistent with the canonical design)

1. **`MorphDelta` is 28 B, NORMAL-only.**
   ```
   struct MorphDelta  { u32 vertexIndex; glm::vec3 dPosition; glm::vec3 dNormal; };  // 28 B
   struct MorphTarget { std::string name; std::vector<MorphDelta> deltas; };
   ```
   `static_assert(sizeof(MorphDelta) == 28, "MorphDelta must stay 28 bytes (the .smesh morph stride)")`.
   No `dTangent` — the engine `Vertex` has no tangent stream; the tangent is re-derived by Gram-Schmidt
   against the morphed normal at deform time (Phase 5). A 40 B record is rejected as dead storage.

2. **Sparse only, per target.** `std::vector<MorphTarget> morphTargets;` on `Mesh` (surfaced through
   `ImportedModel.mesh`). Only moved vertices get a record — cost scales with affected vertices, not
   vertices × targets. The importer drops a vertex whose `dPosition` and `dNormal` are both within a
   small epsilon of zero (the import-time compaction threshold UE/ACL use). No dense fallback anywhere.

3. **`vertexIndex` is offset by the submesh `vertexOffset` at import** so a delta indexes the merged
   `Mesh.vertices` array (the same array the GPU deform reads), applied once in the importer.

4. **Validation at import returns `Err`, never silently drops:**
   - each delta `vertexIndex` < `Mesh.vertices.size()` (post-offset, bounds-checked);
   - per target, the glTF `POSITION` delta accessor `count == base POSITION count` (a short/over-long
     delta accessor is `Err`);
   - per-node primitive agreement: every primitive on a mesh node has the same target count and target
     names (a node cannot have mixed morph layouts), else `Err`. Mirrors the Phase-2 `weights`-channel
     rejection for a node whose mesh has zero targets.

5. **ONE `MeshFormatVersion` + a flags word; the dual-version branch is DELETED.** Replace
   `MeshFormatVersion(1)` + `MeshFormatVersionSkinned(2)` with a single `inline constexpr u32
   MeshFormatVersion` set to a **fresh value (e.g. 3)** so a stale v1/v2 file is unambiguously rejected.
   Add:
   ```
   inline constexpr u32 MeshFlagSkin  = 1u << 0;
   inline constexpr u32 MeshFlagMorph = 1u << 1;
   ```
   `SMeshHeader.flags` carries the bits; spend one reserved u32 for `u32 morphTargetCount;` (leave one
   reserved). Section order: header, vertices, indices, submeshes, **morph section**, skin section. The
   morph section is `morphTargetCount` × `{ u32 nameLen; u32 deltaCount; }` then, per target, `name` bytes
   then `deltaCount` × `MorphDelta` (28 B stride). The morph offset is **derived** from counts (no stored
   offset field — re-assert `sizeof(SMeshHeader) == 64`). The skin offset shifts to after the morph
   section; `loadMeshSkinFromBytes` recomputes it by parsing the morph TOC lengths (read the lengths, skip
   the bytes) even though it only wants the skin stream.

6. **Wrong version is `Err`.** `loadMeshFromBytes` / `loadMeshSkinFromBytes` reject any `header.version !=
   MeshFormatVersion` (no "accept 1 or 2", no "v1 → empty skin" fallthrough). A missing morph bit means
   zero morph targets; a missing skin bit means an empty skin stream — both via the flags, not a version
   number.

7. **`.smodel` container is structurally unchanged.** It embeds the `.smesh` chunk verbatim by fourcc;
   the morph section rides inside that opaque payload. No `SModelHeader` bump, no TOC change. Verify with
   `runContainerSelfTest`.

8. **Two scene components, mirroring the skin/pose split:**
   - `MorphComponent { std::vector<f32> weights; };` — durable, serialized, on the mesh entity beside
     `MeshComponent`/`SkinnedMeshComponent`. Import-managed identity → registered **NON_ADDABLE** (like
     `SkinnedMesh`: present after import, never user-added from the inspector add-component menu). Applies
     to static meshes too (morph needs no rig).
   - `MorphWeightOverrideComponent { std::vector<f32> weights; };` — runtime-only, never serialized,
     copied, or registered (mirrors `PoseOverrideComponent` exactly). The Phase-4 evaluator writes it
     each tick; removed on stop, so Edit preview reverts by construction. Declared in `scene.cppm` in the
     runtime-only block beside `PoseOverrideComponent`, with a matching doc comment.

9. **Seeded weights = `node.weights ?? mesh.weights ?? zeros(targetCount)`** (the glTF precedence). The
   morph *count* is `Mesh.morphTargets.size()` (geometry); the default *values* ride
   `ModelSpawnInput.morphWeights`, populated by Phase 2's importer from `node.weights ?? mesh.weights ??
   zeros`. A mesh with zero morph targets gets **no** `MorphComponent` (an empty one is noise).

## Where the seed weights come from (the data path)

The durable weight *values* are NOT in the `.smesh` (geometry only) — they live in the spawned
`MorphComponent`. At spawn the seed comes from the import graph:

- target *count* = `Mesh.morphTargets.size()` (read back from the loaded mesh);
- default *values* = `ModelSpawnInput.morphWeights` (a new `std::vector<f32>` field Phase 2's importer
  fills from `node.weights ?? mesh.weights ?? zeros(count)`; for a multi-node model it rides the mesh
  node's entry);
- `instantiateModel` reconstructs `morphWeights` from the container `MetadataChunk` the same way it
  rebuilds nodes/skin. If the container metadata does not carry per-node weights yet, seed
  `zeros(count)` from the loaded mesh's morph count (a morph mesh with no authored default weights
  resolves to zeros, which is correct).

## Steps (ordered)

1. **geometry.cppm types.** Add `MorphDelta` (28-B `static_assert`) and `MorphTarget`; add
   `std::vector<MorphTarget> morphTargets;` to `Mesh`. Add the flags constants + the single
   `MeshFormatVersion`; delete `MeshFormatVersionSkinned`. Reshape `SMeshHeader` (flags bits +
   `morphTargetCount`, keep 64 B, re-assert), keeping `magic`/`version` first so `meshCountsFromBytes` /
   `meshFileCounts` still read the count fields correctly (or update both in lockstep).

2. **geometry.cppm importer.** In the Phase-2 primitive-read path, for each primitive with
   `targets_count > 0`: read each target's `POSITION`/`NORMAL` via the Phase-2
   `cgltf_accessor_unpack_floats`-based dense helper (these accessors are commonly sparse), build sparse
   `MorphDelta`s (drop a vertex with near-zero `dPosition` *and* `dNormal`), offset `vertexIndex` by the
   submesh `vertexOffset`, append to `Mesh.morphTargets`. Enforce the four validations (Decision 4) →
   `Err`. Populate `ModelSpawnInput.morphWeights` from `node.weights ?? mesh.weights ?? zeros`. Target
   names come from glTF `mesh.target_names` (extras) when present, else `"morph<i>"`.

3. **geometry.cppm encode.** Rewrite `encodeMeshImage` to set `flags` from `!skin.empty()` /
   `!mesh.morphTargets.empty()`, write `morphTargetCount`, and append the morph section then the skin
   section in the fixed order (keep the `put(offset, src, count)` helper). Update `saveMeshToBuffer`
   (morph, no skin) and `saveMeshSkinnedToBuffer` (morph + skin).

4. **geometry.cppm load.** Rewrite `loadMeshFromBytes`: reject `version != MeshFormatVersion`; parse the
   morph section iff `flags & MeshFlagMorph`, rebuilding `Mesh.morphTargets`; recompute every offset from
   counts and bounds-check the span (the existing "inconsistent or truncated" `Err` pattern). Rewrite
   `loadMeshSkinFromBytes`: reject wrong version; no skin bit → empty stream; else skip the morph section
   by parsing its TOC lengths, then read the skin stream at the derived offset.

5. **geometry.cppm self-tests.** Extend `runGeometrySelfTest`: build a tiny 2-target morph mesh in
   memory (a few verts, sparse deltas on 2 of them; one skinned variant + one unskinned variant),
   round-trip through `saveMeshToBuffer`/`saveMeshSkinnedToBuffer` → `loadMeshFromBytes` +
   `loadMeshSkinFromBytes`; assert target count, names, per-target delta counts, and a sample delta value
   survive. Add a wrong-version case: hand-stamp a buffer with an old version number and assert
   `loadMeshFromBytes` returns `Err`. Keep the existing cube + animated-strip blocks.

6. **scene.cppm components.** Add `MorphComponent` (durable, doc comment like SkinnedMesh's) and
   `MorphWeightOverrideComponent` (runtime-only, doc comment mirroring PoseOverride). Add
   `morphComponentToJson`/`morphComponentFromJson` forward decls beside the SkinnedMesh ones. Bump
   `SceneVersion` to `4`; add a migration branch + comment in `sceneFromJson` (a pre-v4 document has no
   `Morph` component — nothing to migrate, but the `version > SceneVersion` guard and the version-history
   comment move with the bump). Add `registerComponent<MorphComponent>` to the *self-test* registration
   in `scene.cppm`, and extend `runSceneSerializationSelfTest` to round-trip a `MorphComponent` with a
   couple of weights (assert the vector survives).

7. **gen.ts serde body.** In `emitSceneSerde()` add `morphComponentToJson`/`morphComponentFromJson`
   bodies modeled on the vector-of-scalars pattern (a JSON array of floats; defensive `FromJson` that
   tolerates a missing/short array). Run `bun run tools/gen-control-dto/gen.ts` to regenerate
   `scene_component_serde.generated.cpp` and the protocol types; **commit the regenerated file.**

8. **scene_edit_components.cpp registration.** Register `MorphComponent` once in
   `registerBuiltinComponents`, NON_ADDABLE (import-managed like `SkinnedMesh`), wiring
   `morphComponentToJson`/`morphComponentFromJson`. Do **not** register `MorphWeightOverrideComponent`
   (runtime-only). Missing this step means the component silently never serializes — the easiest
   catastrophic miss in the scene-component workflow.

9. **assets.cppm spawn seeding.** Add `std::vector<f32> morphWeights;` to `ModelSpawnInput`. In
   `spawnModel` (unskinned) and `spawnSkinnedModel` (skinned, on the mesh entity): if the loaded/imported
   mesh has morph targets, add `MorphComponent` with `weights` = `result.morphWeights` when sized to the
   target count, else `zeros(targetCount)`. Update `instantiateModel` to reconstruct `morphWeights` from
   the container metadata (or zeros from the loaded morph count).

10. **uploadMesh hook (decl/note only).** Add a one-line `// TODO(phase-5): upload the shared morph
    delta bank as a read-only SSBO on GpuMesh, keyed by mesh uuid` at the `uploadMesh` /
    `meshRefByUuid` seam. Do NOT allocate any GPU resource this phase — keep the footprint to a comment +
    (if cleaner) an unused `Ref` slot on `GpuMesh` left null. No SSBO, no descriptor.

11. **Gate.** `make engine`, fix every warning the change raises, then `make prepare-for-commit` (format
    + lint). Run the geometry + scene self-tests via the present-only smoke
    (`SAFFRON_EXIT_AFTER_FRAMES=1 ./build/debug/bin/SaffronEngine`) and confirm the new self-test log
    lines pass. Leave everything unstaged.

## Frontend (Timeline / Clips / Inspector)

No bespoke UI this phase. `MorphComponent` reaches the Inspector automatically through the regenerated
`@saffron/protocol` types and the generic `fieldRenderer` (a read-only weights array). The dedicated
0..1 sliders land in Phase 8, inside the existing Inspector `AnimationChannels` section — do **not** add a
parallel morph panel here. Because `MorphComponent` is import-managed, add it to `NON_ADDABLE` in
`InspectorPanel.tsx` (so a bare cube never offers an empty Morph section in add-component) — this naturally
follows from the regenerated protocol metadata; if the protocol does not carry the add-able flag, the
`InspectorPanel.tsx` `NON_ADDABLE` list edit is a Phase-8 item, noted here so it is not lost.
`MorphWeightOverrideComponent` is runtime-only and never reaches the wire.

## Performance

- **Sparse + per-target name** keeps delta memory bounded: a 100-shape face rig with a few hundred moved
  verts per shape stores hundreds of records per target, not `verts × targets`.
- The delta bank is **shared read-only across instances** (Phase 5 uploads it once per mesh asset, keyed
  by mesh uuid in `meshRefByUuid`). This phase only sizes the CPU-side `Mesh.morphTargets`; the
  zero-delta cull in the importer keeps the bank free of no-op records.
- No dense fallback — a dense per-vertex-per-target store would blow up VRAM for facial rigs.

## Control commands

**None this phase.** The drivable `set/get-morph-weights` commands land in Phase 7. The only generator run
here is the `gen.ts` scene-serde regeneration for `MorphComponent` (step 7), which also refreshes the
protocol types. The control-schema contract test (`tools/check-control-schema`) and `bun run check` must
still pass after the regeneration even though no command changed.

## Docs

**Defer the morph concept page to Phase 9.** Do not write a new docs page here. The format page
`docs/content/explanations/geometry-and-assets/smesh-format.md` documents the v1/v2 versioning and goes
stale the moment the dual-version constant is deleted. Phase 9 owns the rewrite to the single version +
flags + morph section. To avoid a misleading interim doc, add a one-line `> [!WARNING]` callout at the top
of `smesh-format.md` pointing at this phase's flags collapse (a pointer, not a rewrite). `sanim-format.md`
is untouched (no `.sanim` change this phase).

## Tests

- **geometry self-test (in-engine):** the 2-target morph round-trip (skinned + unskinned) + the
  wrong-version `Err` case (Step 5). Runs under the present-only smoke.
- **scene serde self-test (in-engine):** `MorphComponent` round-trips through `sceneToJson`/`sceneFromJson`
  (Step 6), part of `runSceneSerializationSelfTest`.
- **e2e import (`tests/e2e`):** add a fixture `AnimatedMorphCube.gltf` under `tests/e2e/fixtures/` (a small
  embedded-buffer glTF with 2 morph targets and per-mesh default weights) and a test
  `morph-import.test.ts` (modeled on `model_asset.test.ts` / `model_inspect.test.ts`, using
  `tests/e2e/harness.ts` and the typed `@saffron/protocol` client) that: imports the model over the
  control plane, asserts the loaded mesh reports 2 morph targets with non-zero sparse delta counts, spawns
  it, and asserts the mesh entity has a `Morph` component with a 2-wide weights array.
- **CLI save/load round-trip (`tests/e2e`):** after spawning the morph model, `save-project` then
  `load-project`, then re-read the mesh entity's `Morph` component and assert the weights vector survived
  (the `MorphComponent` round-trips through `project.json` — the end-to-end form of the scene serde
  self-test).

## Acceptance criteria

- `MorphDelta` is exactly 28 B (`static_assert`), NORMAL-only (no `dTangent`).
- Morph targets are carried on `Mesh.morphTargets` and serialized into `.smesh`.
- `.smesh` uses ONE `MeshFormatVersion` + a flags word (skin/morph bits) + `morphTargetCount`; the
  dual-version branch in `loadMeshFromBytes`/`loadMeshSkinFromBytes` is **deleted**; wrong version →
  `Err`.
- `SMeshHeader` re-asserts exactly 64 bytes; `.smodel` container is structurally unchanged (chunk embedded
  verbatim, `runContainerSelfTest` passes).
- The `.smesh` round-trip self-test passes for both skinned and unskinned 2-target morph meshes.
- `MorphComponent` is registered (NON_ADDABLE), serialized, and seeded at spawn (both branches) with
  `node.weights ?? mesh.weights ?? zeros`; `MorphWeightOverrideComponent` exists, runtime-only,
  unregistered.
- `SceneVersion` bumped to 4 with the migration branch; `runSceneSerializationSelfTest` passes with the
  `MorphComponent` round-trip.
- `AnimatedMorphCube` imports with 2 morph targets, sparse delta counts > 0, and a seeded
  `MorphComponent`; the weights survive a `project.json` save/load.
- `make engine` is clean, `make prepare-for-commit` (format + lint) is clean, and the regenerated
  `scene_component_serde.generated.cpp` + protocol types are committed. Everything left unstaged for the
  user to review.
