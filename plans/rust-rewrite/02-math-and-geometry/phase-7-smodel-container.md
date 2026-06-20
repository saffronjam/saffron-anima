# Phase 7 — The `.smodel` container (SMDL): writer, reader, lazy chunk slicing

**Status:** COMPLETED

**Depends on:** phase-3 (`.smesh` image as a chunk payload), phase-4 (`.sanim` image as a chunk
payload) — the container frames whatever bytes the caller hands it, so it only needs the byte-format
discipline and the fourcc/header types.

## Goal

Port the `.smodel` (`SMDL`) container: the 64-byte `SModelHeader`, the 32-byte `TocEntry` table, the
`fourcc` packer, the `ChunkKind` enum, `ContainerChunk`, the `ContainerReader`, and the three entry
points — `write_container`, `read_container_header`, `read_container` — with lazy per-chunk reads,
META front-loading, 16-byte-aligned payloads, and the full bounds/overlap validation. Byte-for-byte
identical so an existing `.smodel` reads back chunk-for-chunk.

## Why this shape (NO LEGACY)

The container is the on-disk packaging that bundles a model's `.smesh` (MESH), textures (STEX),
materials (SMAT), animations (SANM), thumbnail (THMB), and a front-loaded metadata chunk (META) into one
file (geometry.cppm:285-294). It is reproduced 1:1 because the asset catalog reads existing `.smodel`
files chunk-by-chunk by `(kind, subId)`.

Load-bearing details carried verbatim:

- **META front-loading.** META (if present) is placed *first* after the TOC and recorded in
  `metaOffset`/`metaLength`, so a prefix read reaches the metadata without scanning payloads
  (geometry.cppm:1758-1775, the two-pass ordering). Everything else keeps caller order.
- **16-byte payload alignment.** Each payload offset is `align16`'d; the TOC itself starts at
  `sizeof(SModelHeader)` and the first payload at `align16(tocOffset + tocBytes)`
  (geometry.cppm:1750-1801).
- **`totalLength` vs file size validation** on read (`readContainerHeader`, geometry.cppm:1864-1872) and
  the **chunk-table-in-bounds** + **no-overlap** checks (sort the payload ranges by offset, reject if any
  starts before the previous ends, geometry.cppm:1905-1934). These are the silent-corruption guards.
- **Lazy chunk reads.** `ContainerReader` holds the path + header + TOC and reads `[offset, offset+length)`
  on demand (`readChunk`, geometry.cppm:1943-1965), bounds-checked against `totalLength`. `find(kind,
  subId)` linear-scans the TOC (geometry.cppm:1967-1977).
- **`fourcc`** packs a 4-char tag little-endian, tag[0] in the low byte (geometry.cppm:279-283); the
  `ChunkKind` discriminants *are* these packed values, so the enum is `#[repr(u32)]` with `Meta =
  fourcc(b"META")` etc. — a `const fn fourcc` makes the discriminants compile-time.

`ContainerReader` is a plain owned struct (not an `Arc`) — the C++ note "drop it before device teardown
like a Ref" (geometry.cppm:335) is about *lifetime ordering* in the renderer, not shared ownership; in
Rust it is moved to its single owner and dropped at end of scope. No `unsafe`: header/TOC are
reinterpreted with `bytemuck::from_bytes`/`cast_slice` over the `#[repr(C)]` Pod structs.

## Grounding (real files/symbols)

- `engine-old/source/saffron/geometry/geometry.cppm`:
  - `fourcc` (279-283), `ChunkKind` (286-294, fourcc-valued), `ContainerFormatVersion`/`MetadataSchemaVersion`
    (274-276).
  - `SModelHeader` (298-312, 64 B asserted), `TocEntry` (315-323, 32 B asserted), `ContainerChunk`
    (326-332), `ContainerReader` (336-345).
  - `align16` (1750-1753), `writeContainer` (1756-1839, the two-pass META-first ordering, the cursor +
    alignment, the header fill, the payload writes).
  - `readContainerHeader` (1841-1874, the magic/version/totalLength validation), `readContainer`
    (1876-1941, TOC read + bounds + the sorted-range overlap check), `ContainerReader::readChunk`
    (1943-1965), `ContainerReader::find` (1967-1977).
  - `runContainerSelfTest` (2024-2133) — the test oracle (round-trip, META-first, 16-byte alignment,
    flags, bad-magic + lying-totalLength rejection).

## Plan

1. `const fn fourcc(tag: &[u8; 4]) -> u32` (LE pack). `#[repr(u32)] enum ChunkKind { Meta = fourcc(b"META"),
   Mesh = fourcc(b"MESH"), Texture = fourcc(b"STEX"), Material = fourcc(b"SMAT"), Animation = fourcc(b"SANM"),
   Thumbnail = fourcc(b"THMB") }`. `CONTAINER_FORMAT_VERSION`/`METADATA_SCHEMA_VERSION` consts.
2. `#[repr(C)]` Pod `SModelHeader` (64 B) and `TocEntry` (32 B) with `const` size asserts.
3. `ContainerChunk<'a> { kind, sub_id, flags, bytes: &'a [u8] }`.
4. `write_container(path, chunks: &[ContainerChunk]) -> Result<()>`: the META-first two-pass ordering,
   `align16` cursor, header fill (`metaOffset`/`metaLength`/`tocCount`/`totalLength`), TOC + payload
   writes into one buffer, then `std::fs::write`. Use `bytemuck` to lay the header/TOC bytes.
5. `read_container_header(path) -> Result<SModelHeader>` (size + magic + version + totalLength-vs-filesize).
6. `read_container(path) -> Result<ContainerReader>`: read+validate the TOC, the in-bounds check, the
   sorted-range no-overlap check. `ContainerReader { path: PathBuf, header, toc: Vec<TocEntry> }` with
   `read_chunk(&self, &TocEntry) -> Result<Vec<u8>>` (lazy, bounds-checked) and `find(&self, kind, sub_id)
   -> Option<&TocEntry>`.

## Acceptance gate

- `cargo build -p saffron-geometry` + workspace compile.
- A `#[test]` reproducing `runContainerSelfTest` (geometry.cppm:2024): write a container with MESH (first
  in caller order, so META-front-loading is actually exercised), META, and STEX chunks; read it back;
  assert `toc.len() == 3`, `metaLength`/`metaOffset` set, `find` locates all three, META's offset is
  before MESH's and STEX's, MESH and STEX offsets are 16-byte-aligned, STEX `flags == 1`, and each
  chunk's bytes round-trip via `read_chunk`.
- Rejection `#[test]`s: a corrupted magic byte and a lying `totalLength` (file size + 4096) both return
  `Err` from `read_container_header`; an out-of-bounds TOC and overlapping payloads return `Err` from
  `read_container`.
- A golden-bytes `#[test]`: a fixed chunk set writes to a known `total_length` with the header
  `magic == b"SMDL"`, `container_version == 1`, `toc_count == 3`, `toc_offset == 64`.
- `cargo clippy` clean; no `unsafe`.
