+++
title = '.smesh format'
weight = 3
+++

# .smesh format

Importing a glTF or OBJ means parsing JSON or text, de-duplicating vertices, and maybe
regenerating normals. `.smesh` is the baked result: a 64-byte header followed by three raw
arrays, loaded by reading bytes straight into the vectors. A model is baked once on import,
and every later load reads the `.smesh`.

## Layout

A fixed 64-byte header, then vertices, indices, and submeshes contiguously at the offsets
the header records:

```cpp
struct SMeshHeader
{
    char magic[4];        // 'S','M','S','H'
    u32 version;
    u32 flags;            // reserved (0)
    u32 vertexStride;     // == sizeof(Vertex)
    u32 vertexCount;
    u32 indexCount;
    u32 indexWidth;       // bytes per index (4)
    u32 submeshCount;
    u64 verticesOffset;
    u64 indicesOffset;
    u64 submeshesOffset;
    u32 reserved[2];
};
static_assert(sizeof(SMeshHeader) == 64, "SMeshHeader must be exactly 64 bytes");
```

The arrays are written as raw byte blobs, no per-element serialization. That works because
`Vertex` and `Submesh` have [asserted fixed sizes](../mesh-and-vertex-layout/) (32 and 16
bytes), so the in-memory layout is the on-disk layout. The header records `vertexStride`
and `indexWidth` so the loader can reject a file written by an incompatible build.

## Why a custom binary format

The trade is import cost for load cost. The runtime path never re-parses glTF/OBJ; it does
three contiguous reads into pre-sized vectors. The format is versioned
(`MeshFormatVersion`, currently 1) so a layout change is detected rather than silently
misread, and self-describing enough to validate without trusting the producer, which
matters because a `.smesh` is read back as raw memory.

## Loading defensively

`loadMesh` does not trust the header. Before resizing any vector it recomputes the expected
layout from the counts and requires the header's offsets to match and the file to be that
large:

```cpp
const u64 verticesEnd = sizeof(SMeshHeader) + u64(header.vertexCount) * sizeof(Vertex);
const u64 indicesEnd  = verticesEnd + u64(header.indexCount) * sizeof(u32);
const u64 submeshesEnd = indicesEnd + u64(header.submeshCount) * sizeof(Submesh);
if (header.verticesOffset != sizeof(SMeshHeader) ||
    header.indicesOffset  != verticesEnd ||
    header.submeshesOffset != indicesEnd ||
    u64(fileSize) < submeshesEnd) { return Err(...); }
```

The checks run in order: file at least header-sized, magic `SMSH`, version match, stride
and index-width match, then the layout-consistency block. Without it a malformed huge
`vertexCount` would reach `resize()` and abort on a giant allocation. Rejecting the file as
inconsistent or truncated first keeps a corrupt file from crashing the process.

## Self-test

`runGeometrySelfTest` is a headless sanity check: it imports `cube.obj` and `cube.gltf`,
bakes the glTF result, reads it back, and confirms the counts and the first vertex position
round-trip. It logs the outcome rather than asserting, so it is a diagnostic, not a gate.

## In the code

| What | File | Symbols |
|---|---|---|
| Header layout | `geometry.cppm` | `SMeshHeader` |
| Version constant | `geometry.cppm` | `MeshFormatVersion` |
| Write path | `geometry.cppm` | `saveMesh` |
| Defensive load | `geometry.cppm` | `loadMesh` |
| Round-trip check | `geometry.cppm` | `runGeometrySelfTest` |

> [!WARNING]
> The on-disk vertex/submesh stride is the in-memory `sizeof`. Adding a field to `Vertex`
> or `Submesh` without bumping `MeshFormatVersion` would make every existing `.smesh`
> misread silently. The size asserts and the version check are the only guards.

## Related

- [Vertex layout](../mesh-and-vertex-layout/) — the structs whose size the format pins
- [Import pipeline](../import-pipeline/) — where the bake happens
- [Mesh upload](../gpu-mesh-upload/) — what consumes a loaded `Mesh`
