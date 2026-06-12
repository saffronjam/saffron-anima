# Phase 1 — `.srig` rig sidecar format

**Status:** NOT STARTED

## Goal

Make the rig **asset-persisted**. After import, the skeleton description — the node forest, the
joint list, inverse binds, skeleton root, mesh node — exists only in the transient `ImportResult`
and as spawned scene entities; nothing on disk can reconstruct it. Add a `.srig` sidecar file
(uuid-named, beside the `.smesh`, mirroring the `.sanim` precedent) that `importModel` writes for
every skinned import, and a loader that returns exactly the inputs `spawnSkinnedModel` consumes.
This is the foundation: the preview scene (phase 4) spawns from it, the skeleton tree (phase 8)
displays it.

## What exists to build on

- The data to persist already flows through import: `ImportedModel.nodes` (`ImportedNode`
  `geometry.cppm:103-110` — name, parent index, local TRS) and `ImportedModel.skinDesc`
  (`ImportedSkin` `geometry.cppm:115-121` — `joints[]`, `inverseBind[]`, `skeletonRoot`,
  `meshNode`), decoded from cgltf at `geometry.cppm:684-747`. They reach `ImportResult`
  (`assets.cppm:109-121`) and die at the end of the `import-model` handler
  (`control_commands_asset.cpp:484-507`).
- The format precedent: `.sanim` — `SANimHeader` (`geometry.cppm:263-272`: magic, version, counts,
  reserved spare), `saveAnimation` (`geometry.cppm:1326-1362`), `loadAnimation`
  (`geometry.cppm:1364-1444`, hard version gate). `importModel` bakes clips to
  `"models/<uuid>.sanim"` at `assets.cppm:1893-1908`.
- The consumer contract: `spawnSkinnedModel(Scene&, std::string, const ImportResult&)`
  (`assets.cppm:2095-2169`) consumes `nodes` + `skinDesc` (+ `mesh`, `animations`) — the `.srig`
  payload is precisely `nodes` + `skinDesc`.
- The self-test seam: `runGeometrySelfTest` (invoked from the host under `SAFFRON_SELFTEST`,
  `host.cppm:717`) already round-trips mesh/clip formats.

## Work

### 1. The format (Saffron.Geometry)

A binary sidecar with its own magic + version, shaped like `.sanim`:

- `SRigHeader { u32 magic /*"SRIG"*/, u32 version = 1, u32 nodeCount, u32 jointCount,
  u32 materialCount, i32 skeletonRoot, i32 meshNode, u32 reserved[2] }`.
- Per node: name (u32 length + bytes), parent index (i32), local TRS (3+4+3 f32 — translation,
  rotation quaternion, scale), matching `ImportedNode`.
- Then the joint list: `jointCount × i32` node indices, followed by `jointCount × 16` f32 inverse
  binds, matching `ImportedSkin`.
- Then the material table: `materialCount` records of a geometry-local `RigMaterial` mirroring the
  **full** `MaterialSlot` field set (`scene.cppm:209-229`) — baseColor, metallic, roughness,
  emissive + strength, unlit, **and every texture slot** (albedo, metallicRoughness, normal,
  occlusion, emissive, height) as catalog uuids (`u64`) plus uvTiling/uvOffset and
  alphaClip/alphaCutoff. The import loop already builds full `MaterialSlot`s
  (`assets.cppm:1916-1992`); a lossy subset would make the preview lose normal/occlusion/emissive
  maps and UV transforms versus the same rig in-scene. (`MaterialSlot` itself lives in
  `Saffron.Scene`, which Geometry cannot import — Assets converts `MaterialSlot ↔ RigMaterial` at
  the call boundary.) Without this the preview (phase 4) spawns flat white: `spawnSkinnedModel`
  applies `result.materials` via `applyImportedMaterials` (`assets.cppm:2155`, `:2059-2086`), and
  nothing else persists the imported material set.

`auto saveRig(const std::vector<ImportedNode>& nodes, const ImportedSkin& skin, const
std::vector<RigMaterial>& materials, const std::string& path) -> Result<void>` and
`auto loadRig(const std::string& path) -> Result<ImportedRig>` where
`ImportedRig { std::vector<ImportedNode> nodes; ImportedSkin skin;
std::vector<RigMaterial> materials; }`. Hard version gate on load (the `.sanim` pattern,
`geometry.cppm:1382-1384`); validate counts against the file size the way `loadAnimation` does.

### 2. Write it at import (Saffron.Assets)

In `importModel`, write `"models/<meshUuid>.srig"` from the **assembled `ImportResult`**, after
both the skin move and the material loop have populated it — **not** inside the early
`if (model->hasSkin)` block. Two ordering traps to respect:
- `result.nodes`/`result.skinDesc` are populated by `std::move(model->nodes/skinDesc)` at the top
  of the hasSkin block (`assets.cppm:1891-1892`) — `model->` is empty after.
- `result.materials` is populated **later**, by the material loop at `assets.cppm:1911-1993`
  (after the hasSkin block closes at `:1910`).

So `saveRig(result.nodes, result.skinDesc, result.materials, ...)` must run **after `:1993`**, once
all three are filled (build `RigMaterial`s from `result.materials` — the registered `MaterialSlot`s
with catalog uuids — not from `model->materials`, which are raw `ImportedMaterial` bytes with no
uuids). Saving earlier persists a zero-node or zero-material rig that only surfaces at phase 2's
bone-count / phase 5's textured assertions. The path is derivable from the mesh uuid — no catalog
field needed for the rig file itself (phase 2 adds the clip links). A failed rig write logs and
continues (the import still spawns; the asset just isn't editor-openable until re-import/migration),
matching how a failed clip bake is handled (`assets.cppm:1897-1902`).

### 3. Load helper beside the other asset loaders

`auto loadRigAsset(AssetServer& assets, Uuid meshId) -> Result<ImportedRig>` in `assets.cppm` next
to `loadMeshAsset` (`assets.cppm:2004-2046`): resolve the catalog mesh entry, derive the `.srig`
path from the uuid, `loadRig`. This is the one entry point phases 2/4/8 call.

## Validation (done criteria)

- `make engine` green; `make prepare-for-commit` clean.
- `runGeometrySelfTest` extended: build a small `ImportedRig` in memory (3 nodes, 2 joints,
  non-identity TRS + inverse binds), save → load → field-exact compare; a corrupted-header load
  errors rather than crashing.
- `make e2e`: importing `tests/e2e/fixtures/leg.gltf` produces a `.srig` beside the `.smesh` under
  the project assets (the `animation.test.ts` `.sanim` sidecar assertion is the template,
  `tests/e2e/animation.test.ts`).
- `docs/`: extend the asset/import explanation with the `.srig` sidecar (what it stores, why a
  sidecar and not a `.smesh` section).

## Notes / gotchas

- **Do not bump `.smesh`.** A trailing flags-gated section in the v2 file would also be
  backward-compatible (readers tolerate trailing bytes, `geometry.cppm:1309-1314`), but the sidecar
  carries zero risk to the mesh reader, mirrors `.sanim`, and keeps the formats independently
  versioned. A `MeshFormatVersion 3` bump is the one option that breaks every shipped build
  (`loadMesh` hard-fails on unknown versions, `geometry.cppm:1190-1193`) — never that.
- Write the quaternion in a fixed component order and document it in the header comment — the
  glTF→glm (w,x,y,z) trap already bit the node import once (`geometry.cppm:710-712`).
- `ImportedNode.name` is the durable clip-binding key (`AnimTrack.jointName`,
  `geometry.cppm:67-89`) — persist names byte-exact, no normalization.
- The geometry module uses classic `#include`, no `import std` (root AGENTS.md module rules).
