# 02 — Math and geometry: glam, the byte formats, and the importers

`saffron-geometry` is the second leaf after `saffron-core`. It is one C++ module today —
`engine-old/source/saffron/geometry/geometry.cppm`, 2269 lines, one namespace — and it carries five
distinct responsibilities that the rest of the engine builds on:

1. the **math vocabulary** (vectors, quaternion, matrices) that every other crate uses;
2. the CPU **mesh / submesh / vertex / skin** types and the picking primitives (ray, AABB);
3. the **animation track / clip** types and the glTF-faithful import of them;
4. the **byte-exact disk formats** — `.smesh` (`SMSH`), `.sanim` (`SANM`), `.smodel` (`SMDL`
   container) — that are a triple contract (disk image == in-memory payload == GPU buffer);
5. the **importers / decoders** — glTF via the `gltf` crate, OBJ via `tobj`, raster images via the
   `image` crate (or an stb binding for bit-parity), plus the deterministic glue around them.

The crate depends on **`saffron-core` only** (the foundations contract). It has no GPU, no `ash`, no
scene, no Vulkan — it is pure CPU + math + file I/O, which is exactly why it ports early and is heavily
unit-testable. It is consumed by `saffron-scene` (component math), `saffron-animation` (`.sanim` +
clip types), `saffron-assets` (importers + `.smodel` container + the GPU-bound byte layouts), and
`saffron-physics` (math + collider auto-fit reads the mesh AABB).

`#![deny(unsafe_code)]` holds for this crate. The byte formats are reinterpreted with `bytemuck`'s
*safe* `cast_slice` / `from_bytes` over `#[repr(C)]` Pod structs, never `std::mem::transmute` or raw
`memcpy`. The C++ side used `std::memcpy` into `#[repr(C)]`-equivalent structs; `bytemuck` is the safe
Rust expression of exactly that, and it is the seam that lets us keep the formats byte-identical
without `unsafe`.

The companion idiom rules are in [`../00-foundations/conventions.md`](../00-foundations/conventions.md);
this README only adds the geometry-specific decisions.

---

## 1. Math: glam, decided once

`glm` 1.0.1 (`GLM_FORCE_DEPTH_ZERO_TO_ONE`, `GLM_ENABLE_EXPERIMENTAL`, set globally in
`engine-old/CMakeLists.txt:163`) becomes **`glam`**. The decisions that bite, locked here for every
downstream crate (feasibility §5, GLM row):

- **Quaternion order is `xyzw`.** glam's `Quat` is `[x, y, z, w]`, which is *exactly* the glTF storage
  order. The C++ importer has to swizzle on every node — `glm::quat(w, x, y, z)` from glTF's
  `(x, y, z, w)` (`importGltfModel`, geometry.cppm:980-982). In Rust that swizzle is **deleted**:
  `Quat::from_xyzw(r[0], r[1], r[2], r[3])` reads the four glTF floats in declaration order. This is a
  per-area win the whole animation/physics chain inherits — do not re-introduce a wxyz convention
  anywhere.
- **No global `DEPTH_ZERO_TO_ONE`.** glam has no compile-flag equivalent; the `[0,1]` Vulkan clip
  depth is a *per-projection* choice. Wherever the C++ relied on the global flag making
  `glm::perspective` emit `[0,1]`, the Rust call is the explicit `Mat4::perspective_rh` (right-handed,
  `0..1` depth) variant. **No projection lives in `saffron-geometry`** (the camera/projection math is
  scene/rendering), so this rule is *recorded here* for the downstream areas and not exercised by this
  crate; geometry only needs `Mat4` multiply, `inverse`, `transpose`, `Mat3` from `Mat4`, and
  `decompose` (see §4).
- **`Vec3` is 12 bytes, never `Vec3A`.** glam ships two 3-vectors: `Vec3` (3×f32 = 12 B, no padding)
  and `Vec3A` (16-byte SIMD-aligned). The on-disk `Vertex` is `position: Vec3, normal: Vec3, uv0:
  Vec2` and **must be 32 bytes** (`static_assert(sizeof(Vertex) == 32)`, geometry.cppm:379). Using
  `Vec3A` would make it 48+ and silently break the `.smesh` stride and the GPU vertex buffer. **All
  format-bearing structs use `Vec3`/`Vec4`/`Vec2`/`Mat4`, never the `A` variants.** This is a hard,
  asserted invariant (§3).
- **ZYX euler stability is hand-ported, not free.** The C++ scene stores rotation as a quaternion and
  converts to/from euler with a ZYX convention plus a gimbal-stability branch (feasibility §3 scene:
  "euler ZYX stability glam doesn't give for free"). That conversion lives in **`saffron-scene`**, not
  geometry — geometry only stores/imports the quaternion (`ImportedNode.rotation`,
  geometry.cppm:120). Recorded here because glam's `Quat::to_euler(EulerRot::ZYX)` does not reproduce
  the C++ branch verbatim and the scene area owns that fidelity.

`glam` types do **not** derive the traits we need for the byte formats by default
(`bytemuck::Pod`). We do not wrap glam types; instead the format structs are `#[repr(C)]` and hold
glam fields, and we enable glam's `bytemuck` feature so `Vec3`/`Vec4`/etc. are `Pod` — then the
`#[repr(C)]` aggregate is `Pod`/`Zeroable` by derive. (`glam = { version = "...", features =
["bytemuck"] }` in the workspace dependency table; the pin is PP-2's, this area only states the
feature requirement.)

---

## 2. The CPU types — a near-mechanical port

| C++ type | geometry.cppm | Rust shape |
|---|---|---|
| `Vertex` (pos/normal/uv0, 32 B) | 36-41, 379 | `#[repr(C)] struct Vertex { position: Vec3, normal: Vec3, uv0: Vec2 }` + `Pod`/`Zeroable` + `const_assert!(size_of == 32)` |
| `Submesh` (firstIndex/indexCount/vertexOffset:i32/materialSlot, 16 B) | 45-51, 380 | `#[repr(C)] struct Submesh { first_index: u32, index_count: u32, vertex_offset: i32, material_slot: u32 }` |
| `VertexSkin` (u16vec4 joints + vec4 weights, 24 B) | 63-67, 381 | `#[repr(C)] struct VertexSkin { joints: [u16; 4], weights: Vec4 }`, asserted 24 B |
| `Mesh` (vertices/indices/submeshes) | 54-59 | plain `struct Mesh { vertices: Vec<Vertex>, indices: Vec<u32>, submeshes: Vec<Submesh> }` — **not** `#[repr(C)]`, it is the in-memory aggregate the formats serialize *from* |
| `Ray` (origin/dir, dir unit) | 70-74 | `struct Ray { origin: Vec3, dir: Vec3 }` with a doc note that `dir` is caller-normalized |
| `AnimTrack` + `Path`/`Interp` enums | 79-101 | `struct AnimTrack`; `Path`/`Interp` as `#[repr(u8)]` data-less enums (the on-disk byte values are pinned: `Translation=0,Rotation=1,Scale=2`; `Step=0,Linear=1,CubicSpline=2`) |
| `AnimClip` (name/duration/tracks) | 105-110 | plain struct |
| `ImportedNode` (name/parent:i32/TRS) | 115-122 | `struct ImportedNode { name: String, parent: i32, translation: Vec3, rotation: Quat, scale: Vec3 }` (parent `-1` == root) |
| `ImportedSkin` (joints/inverseBind/skeletonRoot/meshNode) | 127-133 | plain struct, `inverse_bind: Vec<Mat4>` |
| `ImportedMaterial` (PBR factors + 5 optional texture byte blobs) | 144-168 | one struct; each optional texture is `Option<TextureSource { bytes: Vec<u8>, ext: String }>` rather than the C++ `has*` bool + parallel fields — Rust `Option` makes the "present" flag and the payload one field (NO LEGACY: the bool/blob pairs do not survive) |
| `ImportedModel` (mesh/materials/skin/nodes/skinDesc/animations) | 170-186 | one struct; `has_skin` collapses into `skin: Option<SkinPayload { stream, nodes, desc, animations }>` (the four skin-only fields are gated by one `Option`, replacing the `hasSkin` bool that gates three vectors) |
| `DecodedImage` (rgba u8, w, h) | 189-194 | `struct DecodedImage { rgba: Vec<u8>, width: u32, height: u32 }` |
| `DecodedImageFloat` (rgba f32, w, h) | 198-203 | `struct DecodedImageFloat { rgba: Vec<f32>, width: u32, height: u32 }` |
| `MaterialMapRole` enum | 208-216 | `#[repr(u8)] enum MaterialMapRole` (colorspace policy key; consumed by assets) |
| `MeshCounts` | 237-241 | plain struct |
| `ChunkKind` (fourcc-valued) | 286-294 | `#[repr(u32)] enum ChunkKind` whose discriminants are the packed fourcc values |
| `SModelHeader` (64 B) / `TocEntry` (32 B) | 298-323 | `#[repr(C)]` Pod, asserted sizes |
| `ContainerChunk` / `ContainerReader` | 326-345 | `ContainerChunk<'a>` borrowing the bytes; `ContainerReader` owns the path + header + toc and reads lazily |

The internal-only headers (`SMeshHeader` 64 B, `SANimHeader` 32 B, `SANimTrackRecord` 20 B,
geometry.cppm:386-428) are private `#[repr(C)]` Pod structs in the format module, with the same
asserted sizes.

**The `has*`-bool → `Option` collapse and the `hasSkin` → `Option<SkinPayload>` collapse are
load-bearing NO-LEGACY moves**, not cosmetic: there must be exactly one way to ask "is there an
albedo texture?" (the `Option` is `Some`), never a bool that can disagree with the blob. This is
called out in each affected phase.

---

## 3. The byte formats — `#[repr(C)]` + `bytemuck` + asserted sizes

The three formats are the single most fidelity-critical part of this area: a one-byte drift produces a
torn mesh or a corrupt clip, never a compile error. They are reproduced byte-for-byte. The discipline:

- Every header/record struct is `#[repr(C)]` and derives `bytemuck::Pod + bytemuck::Zeroable`. Field
  order, widths, and explicit pad fields match the C++ struct exactly (e.g. `SANimTrackRecord` keeps
  its `pad: u16`, geometry.cppm:421).
- Every size is pinned with a compile-time assert (a `const _: () = assert!(size_of::<T>() == N);` or
  the `static_assertions` crate). The pinned sizes, lifted verbatim from the C++ `static_assert`s:
  `Vertex` 32, `Submesh` 16, `VertexSkin` 24, `SMeshHeader` 64, `SANimHeader` 32, `SANimTrackRecord`
  20, `SModelHeader` 64, `TocEntry` 32.
- Reading reinterprets bytes with `bytemuck::from_bytes` / `cast_slice` (safe; checks alignment +
  length), never `unsafe`. Writing serializes with `bytemuck::bytes_of` / `cast_slice`. All
  multi-byte values are **little-endian** (the C++ writes raw host bytes and the engine targets LE;
  this is stated as a frozen assumption, asserted by the round-trip golden test on an LE host).
- The **layout-recompute-and-validate** discipline on read is preserved exactly: `load_mesh_from_bytes`
  recomputes the section offsets from the counts and requires the header's stored offsets to match and
  the span to be long enough (geometry.cppm:1497-1508). A `.smodel` MESH chunk slice must read
  identically to a standalone `.smesh` file — the offsets are self-relative, validated against the
  slice length, not a file size (geometry.cppm:257-261). This is what makes the container embedding
  safe and it is reproduced verbatim.
- The **bounded-cursor** anti-DoS read in `.sanim` (`take(count)` bounds-checks every field so a lying
  count cannot drive a giant allocation, geometry.cppm:1678-1688) becomes a small `Cursor` helper that
  returns `Result` on overrun — the Rust `?` makes it shorter than the C++ `overran` flag, same
  semantics.

The `.smesh` version handling is a NO-LEGACY point: today there are **two** version constants
(`MeshFormatVersion = 1` unskinned, `MeshFormatVersionSkinned = 2` skinned, geometry.cppm:136-139) and
`encodeMeshImage` picks the version by whether the skin is empty (geometry.cppm:1405-1409). The Rust
port keeps **exactly this two-version scheme** as it exists in the live source — it does *not* adopt
the (unbuilt) `full-animations` morph plan's flags-collapse, because that plan is `NOT STARTED` and the
authoritative source is `engine-old/geometry.cppm`. We port what is real, one version field, two
accepted values (1 and 2), unknown versions rejected with `Err`.

The `.smodel` container framing (`SMDL`, 64-byte header, 32-byte TOC stride, META front-loaded,
16-byte-aligned payloads, totalLength-vs-file-size validation, no-overlap check via sorted ranges,
lazy chunk reads) is reproduced 1:1 from `writeContainer` / `readContainer` / `ContainerReader`
(geometry.cppm:1756-1977). `fourcc` (geometry.cppm:279-283) becomes a `const fn fourcc(&[u8; 4]) ->
u32` packing little-endian (tag[0] in the low byte).

---

## 4. The importers — where the crates do half and we glue the rest

The decode/parse crates (feasibility §5) cover the parsing but **diverge** from the C++ in ways that
matter for determinism. Each divergence is a named, tested glue task:

- **glTF — `gltf` crate.** The crate exposes an *index-only* API (typed `Document`/`Node`/`Skin`
  views), whereas cgltf gives `cgltf_node_transform_world` (geometry.cppm:891) and pointer-identity
  joins. We hand-reconstruct: the **parent map** (the crate's `Node` has children, not a parent, so we
  build parent indices by a single walk, replacing `node.parent - data->nodes`,
  geometry.cppm:960-962), the **world-transform walk** (compose each node's local TRS up its parent
  chain — `cgltf_node_transform_world` is recursive parent-product; we reproduce it exactly), and the
  **node ordering** (the crate iterates nodes in document order, which is the cgltf array order the
  joint-index math depends on, geometry.cppm:994 `gltfSkin.joints[j] - data->nodes`). Accessor reads
  (`read_float`/`read_uint`/`read_index`) map to the crate's typed accessor iterators. The skin gate
  (import a skin only when the first skin covers *every* triangle primitive — mixed skinned/unskinned
  imports as plain geometry, geometry.cppm:940-944) and the channel-skip rules (morph-weights, non-skin
  target, sparse sampler all `logWarn` + skip, geometry.cppm:1043-1072) are reproduced. The matrix-node
  decompose (`glm::decompose` → TRS, geometry.cppm:968-970) maps to glam's
  `Affine3A::from_mat4(..).to_scale_rotation_translation()` (note: that path uses `Vec3A`/`Affine3A`
  internally for the *decompose math only* — the *stored* result is plain `Vec3`/`Quat`, so the §1
  Vec3-not-Vec3A rule on stored fields holds).
- **OBJ — `tobj` crate.** The C++ uses `std::map<std::array<int,3>, u32>` first-seen dedup of
  `(vertex, normal, texcoord)` index triples (geometry.cppm:1146-1182), which is **ordered** —
  insertion-order-independent, deterministic across runs. `std::map` is a red-black tree; the Rust
  equivalent that preserves *the same dedup result and the same emitted vertex order* is **`BTreeMap`**
  over the `[i32; 3]` key (a `HashMap` would still dedup correctly but the emitted vertex *order*, and
  thus the byte output, would be nondeterministic). The material-slot grouping (first-seen,
  `std::map<int,u32>`, geometry.cppm:1188-1204) and the V-flip (`1.0 - v` for OBJ's bottom-left origin,
  geometry.cppm:1175-1176) are reproduced. The slot bucket fill order and the "skip empty slots" pass
  (geometry.cppm:1226-1239) are preserved so the submesh layout matches.
- **Raster images — `image` crate, with an stb-binding fallback noted.** `stbi_load` /
  `stbi_load_from_memory` (8-bit RGBA, geometry.cppm:1322-1357) and `stbi_loadf` / `stbi_loadf_from_memory`
  (float RGBA for HDR, geometry.cppm:1359-1394) map to `image::load_from_memory` →
  `to_rgba8()` / `to_rgba32f()`. **Caveat (feasibility §5 stb row):** pure-Rust decode can differ at
  the bit level from stb, which matters only if existing texture *hashes* must match (the asset catalog
  hashes decoded bytes). The decision: **start on the `image` crate**; if PP-7/assets proves a hash
  must be bit-stable against stb-decoded bytes, swap that one decode path to an stb binding
  (`stb_image`-rs) behind the same `DecodedImage` return type. The return shape (`STBI_rgb_alpha`,
  always 4 channels, tightly packed `w*h*4`) is the contract, not the decoder.
- **SVG — dropped, not ported (NO LEGACY).** The charter named "nanosvg → resvg", but the live source
  has **no SVG decode path in geometry at all**: the only SVG consumer is `uploadSvgIcon` in
  `renderer_textures.cpp`, which the icons doc (`engine-old/assets/icons/AGENTS.md`) marks vestigial —
  "nothing currently consumes them … there is no caller today." Porting a dead path violates NO
  LEGACY. So `saffron-geometry` ships **no SVG decode**; its image surface is raster (stb/`image`)
  only. If textured SVG icons are ever revived, that is a *new feature* added with `resvg`/`usvg`/
  `tiny-skia`, not a port obligation — recorded in the subtractions ledger (PP-3), not built here.

`translateModel` (the format dispatch on extension, geometry.cppm:1279-1290) becomes
`translate_model(path) -> Result<ImportedModel>` matching `.gltf`/`.glb` → glTF, `.obj` → OBJ, else
`Err`. The `subIdFor` FNV-1a stable sub-id hash (geometry.cppm:1292-1320) is reproduced *bit-exactly*
— the offset basis, prime, the per-field extra mix round (the comment "an extra mix round between
fields keeps 'ab|c' != 'a|bc'", geometry.cppm:1305), the 4-byte `dupIndex` mix, and the `< 1024 →
+1024` fold — because the asset catalog resolves sub-assets by this id across reimports and a different
hash silently orphans every baked sub-asset.

---

## 5. Ref / ownership sites in this area

This crate is almost entirely value-owning — `Mesh`, `AnimClip`, `ImportedModel`, `DecodedImage` are
plain owned aggregates moved out of the importers, no shared handles. The only `Ref`-adjacent type is
`ContainerReader`, described in C++ as "holds the path; drop it before device teardown like a Ref"
(geometry.cppm:335). In Rust it is a plain owned struct (`path: PathBuf, header, toc: Vec<TocEntry>`)
with no `Arc` — it does not share, it is moved to its single owner, and `readChunk` opens the file
on demand. There are **no `Arc`/`Arc<Mutex>`/`Rc<RefCell>` sites in `saffron-geometry`**; the shared
GPU-resource handles (`Arc<GpuMesh>`) live in `saffron-assets`/`saffron-rendering`, downstream.

---

## 6. Self-test removal

`geometry.cppm` carries four in-engine self-tests, all deleted and re-expressed as `#[cfg(test)]`
(idiom rule 8 / no-self-tests):

- `runPickMathSelfTest` (geometry.cppm:2137-2184) — ray-triangle (center hit, corner-gap miss,
  behind-origin miss, two-sided backface hit), the 45°-rotated world-AABB √2 growth, the slab hit/miss
  — becomes the unit oracle for the picking primitives (phase 2).
- `runTranslateDeterminismSelfTest` (geometry.cppm:1981-2022) — re-import determinism + `subIdFor`
  stability/distinctness/`>=1024` — becomes the importer + sub-id unit tests (phases 3, 5).
- `runContainerSelfTest` (geometry.cppm:2024-2133) — `.smodel` round-trip, META front-loading, 16-byte
  alignment, flags, bad-magic + lying-totalLength rejection — becomes the container unit + golden tests
  (phase 6).
- `runGeometrySelfTest` (geometry.cppm:2186-2268) — obj/gltf import counts, `.smesh` round-trip,
  rigged-fixture skin+clip + `.sanim` round-trip — its assertions are split across the importer (5),
  smesh (4), and sanim (6) phases' tests, driven by the real fixtures
  (`engine-old/assets/models/cube.obj|cube.gltf|animated-strip.gltf`, and the e2e fixtures
  `tests/e2e/fixtures/{leg,skinned-strip,two-materials}.gltf`).

There is no `runGeometrySelfTest` symbol in the Rust crate. The model fixtures are copied into the
crate's `tests/fixtures/` (or referenced from `engine-old/assets/models/` while it exists) so the
`#[test]`s have real inputs.

---

## 7. Phase breakdown

Each phase leaves the Cargo workspace compiling and its own tests green. They are ordered so the math +
CPU types come first (everything depends on them), then the formats (assets/animation consume them),
then the importers (which produce the in-memory types the formats serialize).

| Phase | Title | Depends on |
|---|---|---|
| 1 | Crate scaffold, glam adoption, math + CPU mesh/skin/ray types | 00-foundations (core, workspace) |
| 2 | Picking math: ray-triangle, ray-AABB slab, world-AABB, generate-normals | phase-1 |
| 3 | The `.smesh` byte format (v1 + v2 skin) — repr(C), bytemuck, size asserts, round-trip | phase-1 |
| 4 | The `.sanim` byte format + the anim track/clip types | phase-1, phase-3 |
| 5 | glTF import (gltf crate): world-transform walk, node ordering, skin gate, clip decode | phase-2, phase-3, phase-4 |
| 6 | OBJ import (tobj, BTreeMap dedup) + image/HDR decode + subIdFor | phase-2, phase-5 |
| 7 | The `.smodel` container (SMDL): writer, reader, lazy chunk slicing, META front-load | phase-3, phase-4 |

`subIdFor` lands in phase 6 with the importers that need it (it keys on a source name produced by
import); the format phases (3,4,7) do not need it. The importers (5,6) depend on the picking phase only
for `generate_normals` (an importer fallback when a source omits normals, geometry.cppm:1118-1120,
1245-1247).

---

## Grounding (real files/symbols)

| What | File | Symbols |
|---|---|---|
| The whole module (types, formats, importers, picking, self-tests) | `engine-old/source/saffron/geometry/geometry.cppm` | all of §1-§6 below |
| CPU mesh vocabulary | geometry.cppm | `Vertex`, `Submesh`, `Mesh`, `VertexSkin`, `Ray` |
| Size invariants | geometry.cppm | `static_assert(sizeof(Vertex)==32/Submesh==16/VertexSkin==24)` |
| Animation types | geometry.cppm | `AnimTrack`, `AnimTrack::Path`, `AnimTrack::Interp`, `AnimClip` |
| Import graph | geometry.cppm | `ImportedNode`, `ImportedSkin`, `ImportedMaterial`, `ImportedModel`, `MaterialMapRole` |
| `.smesh` format | geometry.cppm | `SMeshHeader`, `MeshFormatVersion`, `MeshFormatVersionSkinned`, `encodeMeshImage`, `loadMeshFromBytes`, `loadMeshSkinFromBytes`, `meshCountsFromBytes`, `saveMeshToBuffer`, `saveMeshSkinnedToBuffer` |
| `.sanim` format | geometry.cppm | `SANimHeader`, `SANimTrackRecord`, `AnimFormatVersion`, `saveAnimationToBuffer`, `loadAnimationFromBytes` |
| `.smodel` container | geometry.cppm | `fourcc`, `ChunkKind`, `SModelHeader`, `TocEntry`, `ContainerChunk`, `ContainerReader`, `writeContainer`, `readContainer`, `readContainerHeader`, `align16` |
| glTF import | geometry.cppm | `importGltfModel`, `extractGltfMaterial`, `readGltfTextureBytes`, `toTrackPath`, `toTrackInterp` (uses `cgltf_node_transform_world`, `glm::decompose`) |
| OBJ import | geometry.cppm | `importObjModel` (`std::map<std::array<int,3>,u32>` dedup, `std::map<int,u32>` slots) |
| Image decode | geometry.cppm | `decodeImage`, `decodeImageFromMemory`, `decodeImageHdr`, `decodeImageFromMemoryHdr` |
| Stable sub-id | geometry.cppm | `subIdFor` (FNV-1a, `<1024 → +1024` fold) |
| Picking math | geometry.cppm | `rayAabbSlab`, `rayTriangle`, `worldAabbFromCorners`, `generateNormals` |
| Self-tests (deleted) | geometry.cppm | `runGeometrySelfTest`, `runPickMathSelfTest`, `runTranslateDeterminismSelfTest`, `runContainerSelfTest` |
| Vendored dep versions | `engine-old/cmake/Dependencies.cmake` | glm 1.0.1, cgltf v1.15, tinyobjloader v1.0.6, stb v1.16 |
| SVG is vestigial (dropped) | `engine-old/assets/icons/AGENTS.md`, `engine-old/source/saffron/rendering/renderer_textures.cpp` | `uploadSvgIcon` (no caller) |
| Core deps used | `engine-old/source/saffron/core/core.cppm` | `Result`, `Err`, `Uuid`, `u8/u16/u32/i32/f32`, `logWarn`/`logInfo`/`logError` |
| Model fixtures | `engine-old/assets/models/`, `tests/e2e/fixtures/` | `cube.obj`, `cube.gltf`, `animated-strip.gltf`, `leg.gltf`, `skinned-strip.gltf`, `two-materials.gltf` |
