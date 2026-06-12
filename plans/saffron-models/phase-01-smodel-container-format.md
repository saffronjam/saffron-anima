# Phase 01 — The `.smodel` container format in Geometry

**Status:** COMPLETED
**Depends on:** —

## Goal

Define the on-disk `.smodel` container — a fixed `SMDL` `FileHeader`, a `ChunkTOC`, and fourcc-typed,
length-prefixed payload chunks — plus the low-level reader/writer in `Saffron.Geometry`
(`writeContainer` / `readContainer` / `readContainerHeader`). Chunk payloads are **opaque byte ranges**
at this layer (no knowledge of what a `MESH`/`STEX`/`SMAT` chunk contains — that is phases 02/05). The
container reuses the discipline of `SMeshHeader` (magic, version, 64-bit offsets, hard version gate,
file-size validation). Defers: the metadata schema (02), catalog rows (03), baking real assets (05).

## Why

Every later phase reads and writes `.smodel` files; they all need one trustworthy byte layout first.
Building it as a standalone, payload-agnostic codec — with a round-trip self-test — means the riskier
phases (writer, chunk-slice loaders) build on proven bytes. `.smodel` is a **new magic/version**, so it
adds zero risk to existing `.smesh`/`.sanim` readers (the hard constraint: never bump `.smesh` to v3,
`geometry.cppm:1190`).

## The `SMDL` byte layout

Little-endian, mirroring `SMeshHeader` (`geometry.cppm:243`). All offsets are absolute file offsets;
payloads are 16-byte aligned.

```
[ FileHeader   (fixed, 64 B)                    ]
[ ChunkTOC     (tocCount × TocEntry, fixed)     ]
[ MetadataChunk (front-loaded; opaque here)     ]
[ Payload chunks (mesh/tex/mat/anim, aligned)   ]
[ optional ThumbnailChunk                        ]
```

```cpp
struct SModelHeader {            // 64 B
    char  magic[4];              // "SMDL"
    u32   containerVersion;      // bump only the container framing; starts at 1
    u32   schemaVersion;         // metadata-chunk schema version (phase 02 owns it)
    u32   flags;                 // reserved bits (hasSkin, hasThumbnail, ...)
    u64   tocOffset;             // byte offset of the ChunkTOC
    u32   tocCount;              // number of TocEntry records
    u64   metaOffset;            // byte offset of the MetadataChunk (also in the TOC; front-loaded)
    u64   metaLength;            // metadata chunk byte length
    u64   totalLength;           // total file size (validated on read)
    u32   reserved[2];           // pad to 64 B
};

enum class ChunkKind : u32 {     // fourcc, e.g. fourcc("MESH")
    Meta = /*'META'*/, Mesh = /*'MESH'*/, Texture = /*'STEX'*/,
    Material = /*'SMAT'*/, Animation = /*'SANM'*/, Thumbnail = /*'THMB'*/,
};

struct TocEntry {                // fixed-stride
    u32 fourcc;                  // ChunkKind
    u64 subId;                   // stable sub-asset Uuid (0 for META/THMB)
    u64 offset;                  // absolute byte offset of the payload
    u64 length;                  // payload byte length
    u32 flags;                   // per-chunk flags (e.g. STEX colorspace, MESH hasSkin)
};
```

## The codec API (payload-agnostic)

```cpp
// A chunk to write: caller owns the bytes; the writer only frames them.
struct ContainerChunk { ChunkKind kind; u64 subId; u32 flags; std::span<const std::byte> bytes; };

// Write all chunks into one file. The META chunk (if present) is placed first after the TOC and
// recorded in the header's metaOffset/metaLength. Returns the bytes written (or the error).
std::expected<void, std::string>
writeContainer(const std::filesystem::path& path, std::span<const ContainerChunk> chunks);

// Cheap: read + validate only the 64-B header (magic, containerVersion, totalLength vs file size).
std::expected<SModelHeader, std::string> readContainerHeader(const std::filesystem::path& path);

// Full: header + TOC, returning the TOC and a handle that can slice any chunk's bytes lazily.
struct ContainerReader {
    SModelHeader header;
    std::vector<TocEntry> toc;
    // reads [entry.offset, entry.offset+entry.length) from the file; validates against totalLength
    std::expected<std::vector<std::byte>, std::string> readChunk(const TocEntry& entry) const;
    const TocEntry* find(ChunkKind kind, u64 subId) const;
};
std::expected<ContainerReader, std::string> readContainer(const std::filesystem::path& path);
```

Validation mirrors `loadMesh` (`geometry.cppm:1170`): reject a wrong magic, an unknown
`containerVersion`, a `totalLength` that disagrees with the file size, a TOC entry whose
`offset+length` exceeds `totalLength`, or overlapping payloads.

## Files to touch

- `engine/source/saffron/geometry/geometry.cppm` — add `SModelHeader`, `ChunkKind`, `TocEntry`,
  `ContainerChunk`, `ContainerReader`, and `writeContainer` / `readContainer` / `readContainerHeader`,
  next to `SMeshHeader` / `saveMesh` / `loadMesh`. Add a `fourcc(const char[4])` helper.
- `engine/source/saffron/host/host.cppm` (or wherever `runGeometrySelfTest` lives) — add a container
  round-trip + corrupted-header case to the self-test hook.

## Steps

1. Define `SModelHeader` (64 B, `static_assert(sizeof == 64)`), `ChunkKind`, `TocEntry`, `ContainerChunk`.
2. Implement `writeContainer`: layout the header → TOC → META first → other payloads (16-B aligned),
   filling `tocOffset`/`tocCount`/`metaOffset`/`metaLength`/`totalLength`; write in one pass to a buffer
   then to disk (matches `saveMesh`).
3. Implement `readContainerHeader` (seek+read 64 B, validate) and `readContainer` (header + TOC), with
   `ContainerReader::readChunk` / `find`.
4. Add the self-test: build a container from 3 synthetic chunks (a fake META + two payloads), write,
   read back, assert TOC + chunk bytes are byte-identical; then corrupt the magic and the `totalLength`
   and assert both are rejected with an error (not a crash).

## Gate / done

- `make engine` clean; `runGeometrySelfTest` covers the multi-chunk round-trip and the two rejection
  cases; `make prepare-for-commit` clean.
- No change to `.smesh`/`.sanim` magic or version.

## Risks

- **16-byte alignment vs `saveMesh` reuse (phase 05):** chunk payloads must start aligned so a later
  phase can `mmap`/slice a `.smesh` image directly; bake the alignment rule in here or phase 06's
  chunk-slice loader will read a misaligned `SMeshHeader`.
- **Endianness / struct padding:** write fields explicitly (as `saveMesh` does), do not `memcpy` the
  struct, or padding bytes leak into the file and break the file-size check.
- **fourcc byte order:** pick one convention (`'MESH'` as a `u32`) and use it in both writer and reader;
  a mismatch silently fails `find`.
