# Phase 1 — Crate scaffold, glam adoption, math + CPU mesh/skin/ray types

**Status:** COMPLETED

**Depends on:** 00-foundations (the `saffron-core` crate exists and the workspace compiles; this phase
adds `saffron-geometry` to the workspace member list and the `[workspace.dependencies]` `glam` /
`bytemuck` entries).

## Goal

Create the `saffron-geometry` crate, adopt `glam` as the engine math vocabulary with its three locked
decisions, and port the pure-value CPU types that everything else in the area (and the downstream
crates) build on: `Vertex`, `Submesh`, `Mesh`, `VertexSkin`, `Ray`, and the import-graph aggregates
(`ImportedNode`, `ImportedSkin`, `ImportedMaterial`, `ImportedModel`, `MaterialMapRole`,
`DecodedImage`, `DecodedImageFloat`, `MeshCounts`). No I/O, no parsing, no math functions yet — just the
types and the crate's `Error`/`Result`.

## Why this shape (NO LEGACY)

`saffron-geometry` is one crate (mirroring the one C++ `Saffron.Geometry` module), depending on
`saffron-core` only — the foundations contract fixes this edge. It is the second leaf, so it must exist
and compile before the formats and importers can.

The glam decisions are locked **here, once**, because they cascade through every downstream crate:

- **`Vec3` (12 B), never `Vec3A`.** The format structs are byte-stride contracts; `Vec3A`'s 16-byte
  alignment would silently bloat `Vertex` past 32 bytes. All format-bearing fields use `Vec3`/`Vec4`/
  `Vec2`/`Mat4`.
- **Quaternion `xyzw`.** glam's `Quat` order is the glTF storage order, so the C++ wxyz swizzle is
  deleted at the importer (phase 5). `ImportedNode.rotation` is a glam `Quat`.
- **No global depth flag.** glam has no `GLM_FORCE_DEPTH_ZERO_TO_ONE`; projection is per-call
  (`Mat4::perspective_rh`). No projection lives in this crate, so it is recorded for downstream and
  unused here.

The `has*`-bool + parallel-blob pairs in `ImportedMaterial` and the `hasSkin` bool gating three vectors
in `ImportedModel` are **collapsed to `Option`** in the same move — one field answers "is it present?"
*and* carries the payload, so a bool can never disagree with its blob. The old bool/blob shape does not
survive (NO LEGACY: one way to express optionality).

## Grounding (real files/symbols)

- `engine-old/source/saffron/geometry/geometry.cppm`:
  - `Vertex` (36-41), `Submesh` (45-51), `Mesh` (54-59), `VertexSkin` (63-67), `Ray` (70-74).
  - size asserts `sizeof(Vertex)==32` / `Submesh==16` / `VertexSkin==24` (379-381).
  - `ImportedNode` (115-122, `parent=-1` is root, `rotation` a quaternion), `ImportedSkin` (127-133),
    `ImportedMaterial` (144-168, the `has*` bools + parallel byte/ext blobs to collapse),
    `ImportedModel` (170-186, `hasSkin` gating `skin`/`nodes`/`skinDesc`/`animations`).
  - `MaterialMapRole` (208-216), `DecodedImage` (189-194), `DecodedImageFloat` (198-203),
    `MeshCounts` (237-241).
- `engine-old/source/saffron/core/core.cppm`: `Result`, `Err`, `Uuid`, fixed-width aliases
  `u8/u16/u32/i32/f32`.
- `engine-old/cmake/Dependencies.cmake`: glm 1.0.1 (the version being replaced).

## Plan

1. Add `engine/crates/geometry/` to the workspace members; `Cargo.toml` declares `saffron-core`,
   `glam = { workspace = true, features = ["bytemuck"] }`, `bytemuck` (derive feature), and
   `static_assertions` (or a `const _: () = assert!` size guard). `#![deny(unsafe_code)]` +
   `//!` crate doc at `lib.rs`.
2. Define the crate `Error` enum (`thiserror`) + `pub type Result<T>`; seed the variants this area will
   need (`Io(String)`, `BadMagic`, `UnsupportedVersion(u32)`, `Truncated`, `BadLayout`,
   `Decode(String)`, `Import(String)`) — variants are added as later phases need them, but the enum and
   alias exist now so signatures compile.
3. Port the value types in the §2 table of the area README. `Vertex`/`Submesh`/`VertexSkin` are
   `#[repr(C)]` + `#[derive(Clone, Copy, bytemuck::Pod, bytemuck::Zeroable)]`; `Mesh` is a plain
   `Vec`-aggregate. `AnimTrack`/`AnimClip`/`ImportedNode`/`ImportedSkin`/`ImportedMaterial`/
   `ImportedModel` may be declared here as plain structs (their bodies are minimal; the importers in
   5/6 populate them) OR deferred to phase 4/5 — but the `Vertex`/`Submesh`/`Mesh`/`VertexSkin`/`Ray`
   set is mandatory this phase.
4. Add the `const` size asserts for `Vertex` (32), `Submesh` (16), `VertexSkin` (24) so a future glam
   bump or a stray `Vec3A` fails the build immediately.
5. Re-export the public types from `lib.rs`.

## Acceptance gate

- `cargo build -p saffron-geometry` compiles; the whole workspace still compiles
  (`cargo build --workspace`).
- The size asserts hold: a `#[test]` asserts `size_of::<Vertex>() == 32`, `size_of::<Submesh>() == 16`,
  `size_of::<VertexSkin>() == 24`, and that `Vertex`/`Submesh`/`VertexSkin` are `Pod` (compiles only if
  the derives succeed).
- A `#[test]` round-trips a `Vertex`/`Submesh` through `bytemuck::bytes_of` → `from_bytes` and asserts
  equality (proves the `Pod` reinterpret is wired without `unsafe`).
- `cargo clippy -p saffron-geometry` is warning-clean; `#![deny(unsafe_code)]` holds (no `unsafe` in
  the crate).
