# Phase 6 — OBJ import (tobj, BTreeMap dedup), image/HDR decode, and `subIdFor`

**Status:** COMPLETED

**Depends on:** phase-2 (`generate_normals`), phase-5 (`ImportedModel`/`translate_model` exist; this
phase completes the `.obj` dispatch arm), phase-1 (`DecodedImage`/`DecodedImageFloat`, `Uuid` from
core).

## Goal

Port the remaining importer/decoder surface: `importObjModel` (the OBJ → `ImportedModel` path with the
deterministic vertex dedup and first-seen material slots), the four raster decoders
(`decode_image`/`decode_image_from_memory` for 8-bit RGBA, `decode_image_hdr`/`decode_image_from_memory_hdr`
for float RGBA), and the `sub_id_for` FNV-1a stable sub-asset id. After this phase `translate_model`
handles every supported format.

## Why this shape (NO LEGACY)

**OBJ dedup determinism is the load-bearing concern.** The C++ dedups `(vertex, normal, texcoord)` index
triples into unique vertices via `std::map<std::array<int,3>, u32>` (geometry.cppm:1146-1182). `std::map`
is an *ordered* tree, so a given OBJ always emits vertices in the same order regardless of input
traversal accidents — and the byte output of the subsequent `.smesh` bake is therefore stable. The Rust
equivalent that preserves both the dedup result **and the emitted vertex order** is **`BTreeMap<[i32; 3],
u32>`**, not `HashMap` — a `HashMap` would dedup correctly but emit vertices in a nondeterministic order,
silently changing every baked `.smesh`'s bytes (and any hash over them). This is the single decision this
phase exists to lock; it is asserted by a re-import-determinism test.

The material-slot grouping (faces grouped by material into first-seen slots via `std::map<int, u32>`,
empty slots skipped, geometry.cppm:1188-1239) and the **OBJ V-flip** (`uv0.y = 1.0 - texcoord.v`,
because OBJ's texture origin is bottom-left and Vulkan samples top-left, geometry.cppm:1174-1176) are
reproduced. The out-of-range vertex-index guard (geometry.cppm:1155-1160) becomes an `Err`.

**Image decode** maps stb to the `image` crate (feasibility §5): `stbi_load*` → `load_from_memory(..).to_rgba8()`,
`stbi_loadf*` → `.to_rgba32f()`. The return shape is the contract — always 4 channels (`STBI_rgb_alpha`),
tightly packed `w*h*4`. **Bit-parity caveat:** pure-Rust decode can differ from stb at the bit level,
which matters only if the asset catalog must hash decoded bytes stably against stb. Decision: ship the
`image` crate now; if PP-7/assets proves a hash needs stb-bit-parity, swap *that one* decode path to an
stb binding behind the unchanged `DecodedImage` return — recorded as an open question, not built here.

**`sub_id_for` is reproduced bit-exactly.** It is FNV-1a over `(modelKey, kind, sourceName)` with a
per-field extra mix round (the comment: "keeps 'ab|c' != 'a|bc'", geometry.cppm:1305), then a 4-byte
little-endian `dupIndex` mix, then the `< 1024 → +1024` fold into the non-reserved id range
(geometry.cppm:1292-1320). The asset catalog resolves baked sub-assets by this id across reimports, so a
different hash silently orphans every sub-asset — the constants (offset `1469598103934665603`, prime
`1099511628211`) and the mix sequence are copied verbatim.

## Grounding (real files/symbols)

- `engine-old/source/saffron/geometry/geometry.cppm`:
  - `importObjModel` (1127-1277): `tinyobj::LoadObj`, the `resolveVertex` closure + `std::map<array<int,3>,u32>`
    dedup (1146-1182), the V-flip (1175-1176), the `slotFor` material grouping (1188-1204), the
    per-shape face loop (1205-1221), the submesh emit skipping empty slots (1226-1239), the material
    extraction (1252-1275, diffuse/metallic/roughness/emission + the diffuse texture file read).
  - `decodeImage` (1322-1338), `decodeImageFromMemory` (1340-1357), `decodeImageHdr` (1359-1375),
    `decodeImageFromMemoryHdr` (1377-1394).
  - `subIdFor` (1292-1320) — the FNV-1a hash + fold.
  - `translateModel` (1279-1290) — wire the `.obj` arm.
  - `readBinaryFile` (496-512), `directoryOf`/`extensionOf` (463-481), `anyNormalsPresent` (451-461).
- `engine-old/cmake/Dependencies.cmake`: tinyobjloader v1.0.6, stb v1.16 (the libraries replaced).
- `engine-old/source/saffron/core/core.cppm`: `Uuid`, the `<1024` reservation.
- Fixture: `engine-old/assets/models/cube.obj`.

## Plan

1. Add `tobj` and `image` to the workspace deps (PP-2 pins). `import_obj_model(path) -> Result<ImportedModel>`:
   load with `tobj` (resolve `.mtl` + textures from the OBJ's directory), then the `resolve_vertex`
   closure over a `BTreeMap<[i32; 3], u32>` building `mesh.vertices`, the V-flip on uv0, the
   `BTreeMap<i32, u32>`-style first-seen slot grouping (or a `Vec` + index map preserving first-seen),
   the per-shape per-face fill, the submesh emit skipping empty slots, and the material extraction.
   Recompute normals when the source has none.
2. `sub_id_for(model_key: &str, kind: &str, source_name: &str, dup_index: u32) -> Uuid` — the verbatim
   FNV-1a with the per-field extra-mix round, the 4-byte `dup_index` mix, and the `< 1024 → +1024`
   fold. Constants copied exactly.
3. The four decoders returning `DecodedImage`/`DecodedImageFloat` via `image::load_from_memory` /
   `load` → `to_rgba8()` / `to_rgba32f()`, packed tightly, with the width/height set.
4. `translate_model` gains the `.obj` arm; the unsupported-format `Err` text is preserved.

## Acceptance gate

- `cargo build -p saffron-geometry` + workspace compile.
- OBJ `#[test]` (from `runGeometrySelfTest`): `cube.obj` imports with the expected counts; importing it
  twice yields the **same emitted vertex order** (the `BTreeMap` determinism — assert the full vertex
  vector is identical across two imports, not just counts).
- `sub_id_for` `#[test]` reproducing `runTranslateDeterminismSelfTest` (geometry.cppm:2005-2011):
  `("town","material","stone",0)` is stable across two calls; differs from `dup_index=1`, from
  `kind="mesh"`, and from `source_name="marble"`; and is `>= 1024`. Optionally pin the exact u64 of one
  known tuple as a golden value so a hash drift is caught.
- Image decode `#[test]`: decode a tiny embedded PNG (and an HDR) → correct width/height and a
  `rgba.len() == w*h*4` packed buffer.
- `cargo clippy` clean; no `unsafe`.
