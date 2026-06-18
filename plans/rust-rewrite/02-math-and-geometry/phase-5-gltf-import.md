# Phase 5 — glTF import via the `gltf` crate

**Status:** COMPLETED

**Depends on:** phase-2 (`generate_normals` fallback), phase-3 (`Mesh`/`Vertex`/`VertexSkin`),
phase-4 (`AnimTrack`/`AnimClip` + the `Path`/`Interp` maps), phase-1 (`ImportedModel` aggregates).

## Goal

Port `importGltfModel` onto the `gltf` crate: decode `.gltf`/`.glb` into an `ImportedModel` — the merged
triangle mesh with first-seen material slots, the optional skin payload (joint list, inverse-bind
matrices, the source node forest, decoded skeletal clips), and the imported materials with their texture
byte blobs. Deterministic and byte-identical in its outputs to the cgltf path, because the asset bake
hashes/orders depend on it.

## Why this shape (NO LEGACY)

The `gltf` crate gives an **index-only** typed API (`Document`, `Node`, `Skin`, `Accessor` views),
whereas cgltf gave pointer-identity joins and `cgltf_node_transform_world`. The three deterministic glue
pieces the crate does *not* give for free, reconstructed by hand and pinned by tests:

1. **The parent map.** cgltf computes a node's parent by pointer subtraction (`node.parent -
   data->nodes`, geometry.cppm:962); the `gltf` crate's `Node` exposes `children`, not a parent. We
   build a `parent: Vec<i32>` (init `-1`) in one pass over every node's children, so `ImportedNode.parent`
   matches the cgltf result exactly.
2. **The world-transform walk.** For the *unskinned* path, cgltf bakes each mesh-node's world transform
   into its vertices (`cgltf_node_transform_world` + apply to position, and the inverse-transpose of the
   upper-3×3 to the normal, geometry.cppm:806-828). `cgltf_node_transform_world` is the parent-chain
   product of local TRS matrices; we reproduce it exactly (compose `Node::transform().matrix()` up the
   parent chain) so the baked vertices are bit-identical.
3. **Node ordering.** The joint-index math (`gltfSkin.joints[j] - data->nodes`, geometry.cppm:994) and
   the channel→joint resolution (`channel.target_node - data->nodes`, geometry.cppm:1049) depend on the
   node *array order* being document order. The `gltf` crate iterates `document.nodes()` in document
   order; we index by `node.index()` (the document index), which is that same order. Asserted by the
   re-import-determinism test.

The cgltf-specific control flow is reproduced exactly because it changes the output:

- **Skinned vs unskinned dispatch.** When the file has no skins, prefer iterating *mesh nodes* (baking
  the world transform) and fall back to raw `meshes` only if no node carries a mesh (geometry.cppm:879-909).
  When it has skins, iterate raw `meshes` without a node transform (geometry.cppm:911-921). Reproduced
  branch-for-branch.
- **The skin gate.** A skin is imported only when the **first** skin covers every triangle primitive —
  i.e. a skinned primitive was seen and no unskinned one was (geometry.cppm:940-944). A mixed model
  imports as plain geometry with a `logWarn` (geometry.cppm:1108-1111), because deforming unweighted
  vertices would collapse them to the origin.
- **Material first-seen slots.** Distinct source materials keyed by identity in first-seen order; a
  null material gets a default slot (geometry.cppm:744-801). The crate keys by `material.index()`
  (`Option<usize>`, `None` == the default material), preserving first-seen order in an index→slot map.
- **The quaternion swizzle is DELETED.** cgltf reads `(x,y,z,w)` and the C++ builds `glm::quat(w,x,y,z)`
  (geometry.cppm:980-982); glam's `Quat::from_xyzw(r[0],r[1],r[2],r[3])` takes the four glTF floats in
  order. The matrix-node case uses glam's `to_scale_rotation_translation` in place of `glm::decompose`.
- **Channel-skip rules.** Morph-`weights` channels, channels targeting a non-skin node, and sparse/empty
  samplers are each skipped with the same `logWarn` text (geometry.cppm:1043-1072). A clip with no
  surviving tracks is dropped (geometry.cppm:1102).
- **Material texture blobs.** `extractGltfMaterial` reads base-color/metallic-roughness/normal/occlusion/
  emissive texture bytes from embedded buffer views or external files, percent-decoding URIs, skipping
  `data:` URIs with a warning (`readGltfTextureBytes`, geometry.cppm:649-723). Each becomes an
  `Option<TextureSource>` (the phase-1 collapse): present == `Some(bytes+ext)`.

Normals are recomputed via `generate_normals` only when the source provides none
(`anyNormalsPresent`, geometry.cppm:451-461 / 1118-1120).

## Grounding (real files/symbols)

- `engine-old/source/saffron/geometry/geometry.cppm`:
  - `importGltfModel` (725-1125) — the whole importer: `appendPrimitive` closure (748-877), the
    skinned/unskinned dispatch (879-921), the skin payload + node forest (943-1019), the clip decode
    (1024-1106).
  - `readGltfTextureBytes` (649-688), `extractGltfMaterial` (690-723), `extensionFromMime` (483-494),
    `directoryOf`/`extensionOf` (463-481), `anyNormalsPresent` (451-461).
  - `toTrackPath` (515-526), `toTrackInterp` (529-540) (from phase 4).
  - `translateModel` (1279-1290) — the `.gltf`/`.glb`/`.obj` dispatch (the `.obj` arm lands in phase 6).
- `engine-old/cmake/Dependencies.cmake`: cgltf v1.15 (the library replaced by the `gltf` crate).
- Fixtures: `engine-old/assets/models/cube.gltf`, `animated-strip.gltf`;
  `tests/e2e/fixtures/leg.gltf`, `skinned-strip.gltf`, `two-materials.gltf`.

## Plan

1. Add the `gltf` crate to the workspace deps (PP-2 pins the version; the crate's `import` reads buffers
   + external images). Parse the file; on parse/buffer-load failure return `Err(Import(...))` with the
   same message shape.
2. Build the `parent: Vec<i32>` map and a `world_transform(node_index) -> Mat4` that composes local TRS
   up the parent chain (the `cgltf_node_transform_world` reproduction).
3. Port `append_primitive`: read positions/normals/uv0/joints0/weights0 accessors, apply the node
   transform (and the inverse-transpose normal transform) when baking, dedup nothing (glTF primitives
   are already indexed — push vertices, then indices offset by `vertex_offset`, then a `Submesh`).
   Track `saw_skinned`/`saw_unskinned` and the first-seen material slot map.
4. Implement the skinned/unskinned dispatch branches verbatim (mesh-node walk with transform; raw-mesh
   walk without).
5. After primitives: build the material table (`extract_gltf_material` for each first-seen material,
   default for the null slot). Then the skin gate: if `skins > 0 && saw_skinned && !saw_unskinned`,
   build the `SkinPayload` (the `ImportedNode` forest with TRS or decomposed-matrix, the joint index
   list, the inverse-bind matrices, `skeleton_root`, `mesh_node`, the moved skin stream) and decode the
   clips with the channel-skip rules.
6. `translate_model` gains the `.gltf`/`.glb` arm (the `.obj` arm is phase 6).

## Acceptance gate

- `cargo build -p saffron-geometry` + workspace compile.
- Import `#[test]`s over the real fixtures (from `runGeometrySelfTest` + `runTranslateDeterminismSelfTest`):
  - `cube.gltf` imports with the expected vertex/index/submesh counts and at least one material slot.
  - `animated-strip.gltf` imports `skin.is_some()` with at least one decoded clip; the clip's first
    track's `path`/`interp`/`joint`/`jointName` match the source.
  - `two-materials.gltf` yields two material slots in first-seen order.
  - **Determinism:** importing `cube.gltf` twice yields structurally-identical graphs (same counts, same
    first/last vertex positions, same node names in order) — the `runTranslateDeterminismSelfTest`
    `sameGraph` assertion.
- A skinned round-trip `#[test]` chaining phase 3/4: import `skinned-strip.gltf`, bake the mesh+skin via
  `save_mesh_skinned_to_buffer`, `load_mesh`/`load_mesh_skin` back, and save/load the first clip through
  `.sanim`, asserting equality.
- `cargo clippy` clean; no `unsafe`.
