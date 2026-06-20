# Phase 3 — The `.smesh` byte format (v1 + v2 skin)

**Status:** COMPLETED

**Depends on:** phase-1 (the `Vertex`/`Submesh`/`Mesh`/`VertexSkin` Pod types + size asserts).

## Goal

Port the `.smesh` (`SMSH`) byte image: the 64-byte `SMeshHeader`, the three-section layout (vertices,
indices, submeshes) with the v2 skin section appended, and the buffer encode/decode functions —
`save_mesh_to_buffer`, `save_mesh_skinned_to_buffer`, `load_mesh_from_bytes`, `load_mesh_skin_from_bytes`,
`mesh_counts_from_bytes`, plus the file wrappers `save_mesh_skinned`, `load_mesh`, `load_mesh_skin`,
`mesh_file_counts`. Byte-for-byte identical to the C++ image so a `.smodel` MESH chunk and a standalone
file read the same.

## Why this shape (NO LEGACY)

The `.smesh` image is the canonical triple contract: disk bytes == in-memory payload == the GPU vertex
buffer. It is reproduced with **safe** `bytemuck` over `#[repr(C)]` Pod structs — `bytes_of`/`cast_slice`
to write, `from_bytes`/`cast_slice` to read — so `#![deny(unsafe_code)]` holds while the bytes stay
identical. The header's `verticesOffset == sizeof(SMeshHeader)`, `indicesOffset`, `submeshesOffset`
self-relative offsets and the **recompute-and-validate** read (recompute the section ends from the
counts, require the stored offsets to match and the span to be long enough) are reproduced verbatim
(geometry.cppm:1497-1508) — this is what makes a chunk slice read identically to a file and what guards
against a truncated/lying header.

**The two-version scheme is kept exactly as the live source has it** — `MeshFormatVersion = 1`
(unskinned), `MeshFormatVersionSkinned = 2` (skinned), and `encode` picks the version by whether the
skin is non-empty (geometry.cppm:1405-1409). We do **not** adopt the unbuilt `full-animations` morph
plan's single-version+flags collapse: that plan is `NOT STARTED` and the authoritative ground truth is
`engine-old/geometry.cppm`. Porting two real versions, not a hypothetical one, is the NO-LEGACY-correct
move (one code path that matches reality). `load` accepts versions 1 and 2 and rejects any other with
`Err(UnsupportedVersion)`; `load_mesh_skin_from_bytes` returns an empty stream (not an error) for a v1
image (geometry.cppm:1589-1592).

The skinned encoder enforces `skin.len() == vertices.len()` and errors otherwise
(geometry.cppm:1469-1473) — preserved as a `Result` on `save_mesh_skinned_to_buffer`.

## Grounding (real files/symbols)

- `engine-old/source/saffron/geometry/geometry.cppm`:
  - `SMeshHeader` (386-401, 64 B asserted): `magic[4]='SMSH'`, `version`, `flags`, `vertexStride`,
    `vertexCount`, `indexCount`, `indexWidth`, `submeshCount`, `verticesOffset`, `indicesOffset`,
    `submeshesOffset`, `reserved[2]`.
  - `MeshFormatVersion = 1` / `MeshFormatVersionSkinned = 2` (136-139).
  - `encodeMeshImage` (1401-1443): version-by-skin, the offset math, the `put(offset, src, count)`
    section writes, skin appended at `submeshesEnd`.
  - `loadMeshFromBytes` (1477-1519): magic check, version check, stride/width check
    (`vertexStride==sizeof(Vertex)`, `indexWidth==4`), recompute-and-validate, section `memcpy`s.
  - `loadMeshSkinFromBytes` (1577-1602): v1 → empty, v2 → read the skin section after `submeshesEnd`.
  - `meshCountsFromBytes` (1552-1565), `meshFileCounts` (1536-1550) — header-only counts.
  - `saveMeshToBuffer` (1461-1464), `saveMeshSkinnedToBuffer` (1466-1475, the parallel-length check),
    `saveMeshSkinned` (1567-1575), `loadMesh` (1521-1534), `loadMeshSkin` (1604-1617).
  - `readBinaryFile` (496-512), `writeBytesToFile` (1445-1458) — the file I/O wrappers.

## Plan

1. Private `#[repr(C)] struct SMeshHeader` (Pod/Zeroable) with the exact field order/widths; a
   `const _: () = assert!(size_of::<SMeshHeader>() == 64)`.
2. `MESH_FORMAT_VERSION: u32 = 1`, `MESH_FORMAT_VERSION_SKINNED: u32 = 2` consts.
3. `encode_mesh_image(mesh: &Mesh, skin: &[VertexSkin]) -> Vec<u8>` — build the header, allocate
   `total`, write header + the three sections + optional skin via `bytemuck::cast_slice` /
   `bytes_of`. Empty skin → v1; non-empty → v2.
4. `save_mesh_to_buffer(mesh) -> Vec<u8>`; `save_mesh_skinned_to_buffer(mesh, skin) -> Result<Vec<u8>>`
   (parallel-length check).
5. `load_mesh_from_bytes(bytes: &[u8]) -> Result<Mesh>` with the magic/version/stride checks and the
   recompute-and-validate. Use `bytemuck::from_bytes` for the header and `cast_slice` for the sections
   (length-validated up front so the cast cannot panic).
6. `load_mesh_skin_from_bytes`, `mesh_counts_from_bytes`, and the file wrappers (`save_mesh_skinned`,
   `load_mesh`, `load_mesh_skin`, `mesh_file_counts`) reading via `std::fs`.

## Acceptance gate

- `cargo build -p saffron-geometry` + workspace compile.
- A `#[test]` round-trip (from `runGeometrySelfTest`, geometry.cppm:2207-2228): build a `Mesh`,
  `save_mesh_to_buffer`, `load_mesh_from_bytes`, assert vertex/index/submesh counts and the first
  vertex position match. A skinned round-trip: `save_mesh_skinned_to_buffer` (v2), then
  `load_mesh_from_bytes` + `load_mesh_skin_from_bytes` return the mesh and the parallel skin.
- A **golden-bytes** `#[test]`: a fixed small mesh encodes to a known byte length and a header with
  `magic == b"SMSH"`, `version == 1`, `vertex_stride == 32`, `index_width == 4`, and the three offsets
  at `64 / 64+V*32 / +I*4`. (The byte image is the frozen contract; this is the regression tripwire.)
- Rejection `#[test]`s: bad magic → `Err(BadMagic)`; version 3 → `Err(UnsupportedVersion(3))`; a
  truncated buffer → `Err(Truncated)` / `Err(BadLayout)`; `load_mesh_skin_from_bytes` on a v1 image →
  `Ok(empty)`; `save_mesh_skinned_to_buffer` with a mismatched skin length → `Err`.
- `cargo clippy` clean; no `unsafe`.
