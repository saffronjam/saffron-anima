# Phase 3 — Morph delta storage + .smesh format bump + spawn seeding

**Status:** NOT STARTED

**Depends on:** Phase 2 (the importer must decode `primitive.targets` and `mesh.weights`)

## Why

Morph deltas must live on disk (in `.smesh`) and reach the GPU as a shared read-only bank, and each
mesh instance needs a resolved weight array. This phase adds the sparse delta storage, replaces the
`.smesh` format to carry it, and seeds `MorphComponent` at spawn — everything Phase 5's GPU stage and
Phase 7's control command consume.

## Grounding

- `Vertex` 32 B locked (`geometry.cppm:36`, `static_assert sizeof(Vertex)==32`); `VertexSkin` 24 B
  parallel stream (`:63`). Morph deltas must be a **third parallel section**, not interleaved.
- `.smesh`: `SMeshHeader` 64 B (`:386-401`), `MeshFormatVersion=1` (unskinned), `MeshFormatVersionSkinned=2`
  (`:136-139`), `encodeMeshImage` (`:1401`, picks version on skin emptiness), `loadMeshFromBytes`
  (`:1479`), `loadMeshSkinFromBytes` (`:1579`).
- `primitive.targets` (cgltf): array of morph targets per primitive, each a dict POSITION/NORMAL/TANGENT
  → accessor of additive deltas (commonly sparse). `mesh.weights`/`node.weights` defaults.
- `spawnSkinnedModel` (`assets.cppm:4818`); `spawnModel` (`:4972`); `.smodel` metadata writer
  (`assets.cppm:3229+`, `meta.nodes` at `:3251`).

## Decisions (locked)

1. **Sparse delta model (UE/Unity/glTF-aligned).**
   ```
   struct MorphDelta  { u32 vertexIndex; glm::vec3 dPosition; glm::vec3 dNormal; glm::vec3 dTangent; };  // 40 B
   struct MorphTarget { std::string name; std::vector<MorphDelta> deltas; };
   ```
   Stored on `Mesh`/`ImportedMesh` as `std::vector<MorphTarget> morphTargets`. Only vertices the target
   moves get a record (sparse). NORMAL/TANGENT deltas decoded when present; zero when absent (the deform
   stage re-derives tangent from the morphed normal + base tangent, per spec). The importer reads each
   target accessor through `readAccessorDense` (Phase 2 helper), then compacts to non-trivial deltas
   (drop position deltas below a small epsilon — the import-time precision threshold UE/ACL use).
2. **`.smesh` format replaced (no v2 fallback for skinned).** A skinned-or-morphed mesh is one format.
   Bump `MeshFormatVersionSkinned = 3`; `SMeshHeader` gains `u32 morphTargetCount` and a
   `u64 morphOffset` (use the two `reserved[2]` slots — keeps the header 64 B; re-assert). The morph
   section (after the skin section) is, per target: `{u32 nameLen; u32 deltaCount}` + name + tightly
   packed `MorphDelta[deltaCount]`. `encodeMeshImage` writes v3 when `morphTargets` non-empty (skinned
   **or** unskinned-with-morphs both use v3). `loadMeshFromBytes`/`loadMeshSkinFromBytes` read v3 and
   return the morph targets; the v2 read path is **deleted** (NO LEGACY — a v2 file is
   `Err("unsupported .smesh version")`). The `.smesh` round-trip self-test extended with a 2-target
   morph mesh.
3. **`MorphComponent` (durable) + `MorphWeightOverrideComponent` (runtime-only).**
   - `MorphComponent { std::vector<f32> weights; }` on the mesh entity — serialized; the resolved
     authored weights (seeded from `node.weights` else `mesh.weights` else zeros). Editable in the
     Inspector (Phase 8), driven by the control command (Phase 7).
   - `MorphWeightOverrideComponent { std::vector<f32> weights; }` — runtime-only, mirrors
     `PoseOverrideComponent` (`scene.cppm:123-128`): the animated weights the evaluator writes each
     frame; never serialized/copied; removed when playback stops. The deform stage reads override if
     present, else `MorphComponent.weights`.
   Register `MorphComponent` once in `scene_edit_components.cpp` `registerBuiltinComponents` and add its
   serde body in `gen.ts emitSceneSerde` (per the scene AGENTS.md three-step). `MorphWeightOverride`
   stays unregistered/runtime-only like `PoseOverride`/`WorldTransform`.
4. **Spawn seeds `MorphComponent`.** Both spawn branches attach a `MorphComponent` sized to the mesh's
   morph-target count, seeded from the import's resolved weights (carried on `ModelSpawnInput`). The GPU
   mesh upload (Phase 5) uploads the shared delta bank; the component is per-instance weights only.
5. **`SceneVersion` bump + migration branch** (`scene.cppm` serde) for `MorphComponent`; pre-bump scenes
   default no morph component. Extend `runSceneSerializationSelfTest` with a `MorphComponent` round-trip.

## Edits

- `geometry.cppm`: add `MorphDelta`/`MorphTarget`, `Mesh.morphTargets`; `SMeshHeader` morph fields +
  re-assert; `MeshFormatVersionSkinned`→3; `encodeMeshImage`/`loadMesh*FromBytes` write/read the morph
  section, delete v2 read; importer (Phase 2 loop) decodes `primitive.targets` + compacts; carry
  resolved `mesh.weights`/`node.weights` onto `ImportedModel`.
- `scene.cppm`: `MorphComponent`, `MorphWeightOverrideComponent`; `SceneVersion` bump + migration;
  self-test.
- `scene_edit_components.cpp`: `registerComponent<MorphComponent>(...)`.
- `tools/gen-control-dto/gen.ts`: `emitSceneSerde` body for `MorphComponent` (vector<f32>); regenerate.
- `assets.cppm`: `spawnSkinnedModel`/`spawnModel` seed `MorphComponent`; `.smodel` mesh chunk carries
  morph targets (or they ride the embedded `.smesh` bytes — prefer the latter, one mesh format).
- GPU mesh upload (`renderer_drawlist.cpp uploadMesh`): upload the morph delta bank as a storage buffer
  on `GpuMesh` (Phase 5 binds it). (Decl only here; wiring in Phase 5.)

## Verification

- `make engine`; `make prepare-for-commit`; regenerated serde committed.
- `.smesh` round-trip self-test green with a morph mesh; v2 file → `Err`.
- Import `AnimatedMorphCube.gltf`: assert 2 morph targets, sparse delta counts > 0, `MorphComponent`
  seeded to `[0,0]` (its `mesh.weights` default).
- `MorphComponent` round-trips through `project.json` (scene serde self-test + an `se` save/load).

## Risks

- **Header budget:** `SMeshHeader` has exactly two `reserved` u32s; a `u64 morphOffset` needs 8 bytes.
  Resolution: store `morphOffset` as `verticesOffset`-relative or recompute it (skin section size is
  derivable), spending only one u32 `morphTargetCount` from reserved and keeping the header 64 B. Verify
  the `static_assert(sizeof(SMeshHeader)==64)` still holds.
- **Tangent w-handedness:** glTF morph TANGENT deltas are VEC3 (no w). Store dTangent as vec3; the
  deform stage re-derives handedness from the base tangent (the mesh has no tangent stream today —
  Phase 5 derives tangent at deform time per spec, or normal-only morph is acceptable for v1 if
  documented). Decision: store dPosition + dNormal always; dTangent optional (zero when absent).
- **Delta memory for big rigs:** sparse + per-target name keeps it bounded; the bank is shared
  read-only across instances (Phase 5). No dense fallback.
