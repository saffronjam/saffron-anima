# Phase 3 — container metadata and `.smodel` model open

**Status:** COMPLETED

**Depends on:** 07-assets-and-materials:phase-1-crate-skeleton-and-asset-server, 02-math-and-geometry:phase-7-smodel-container (the `.smodel` ContainerReader/writer codec)

## Goal

Port `ContainerMetadata` (the META-chunk record of a `.smodel`), its deterministic encode/decode
(`encode_container_metadata` / `read_container_metadata`), the `ModelAsset` open path (`load_model_asset`,
negative-cached in `model_by_uuid`), and the chunk-source resolution (`chunk_source_for` + the remap
table) + the `ByteSource` (file-or-chunk-slice) reader. These sit on top of geometry's container codec
(`ContainerReader`, `find(kind, subId)`, `read_chunk`).

## Why this shape (NO LEGACY)

The META chunk's object-key serialization is **stable (sorted) order** so the bytes are deterministic for
source hashing and the contract test — that ordering is preserved exactly. `ModelAsset` (`{meta,
reader}`) is held behind `Arc` in the model cache and is negative-cached just like meshes/textures: a
container that fails to open inserts `None` so it is not re-read every frame. `chunk_source_for`
resolves a sub-id with the remap-wins / embedded-chunk-fallback order (an extracted sub-asset points at
an external file; a missing external file warns once and falls back to the embedded chunk). `ByteSource`
is a small value type (`{path, offset, length}`) whose `read()` returns the bytes — a file when
`offset==0&&length==0`, else a slice; it stays a plain value, not an `Arc`.

## Grounding (real files/symbols)

- `engine-old/source/saffron/assets/assets.cppm`: `ContainerMetadata` (+ nested `Import`/`SubAsset`,
  `schema`, `modelId`, `name`, `sourceFormat`, `subAssets`, `materials`, `nodes`, `skin`, `remap`),
  `encodeContainerMetadata` ("object keys serialize in a stable (sorted) order"), `readContainerMetadata`,
  `MetadataSchemaVersion`, `ModelAsset` (`{meta, reader}`), `loadModelAsset` (negative-cached
  `modelRefByUuid`), `chunkSourceFor` (remap-wins resolution), `ByteSource`, `meshCountsForAsset`.
- Upstream geometry: `ContainerReader`, `find`, `read_chunk`, `ChunkKind` (the `.smodel` codec ported by
  02-math-and-geometry:phase-7).

## Acceptance gate

- `cargo build -p saffron-assets` + workspace green; clippy + fmt clean.
- A META round-trip `#[test]`: a hand-built `ContainerMetadata` → `encode_container_metadata` →
  `read_container_metadata` reproduces every field incl. `subAssets`, `materials`, `nodes`, `skin`,
  `remap`; a golden-bytes test pinning the sorted-key META encoding (deterministic for source hashing).
- `load_model_asset` `#[test]`s: a valid `.smodel` fixture opens once and is cached (second call does not
  re-read disk); a missing/corrupt container negative-caches `None` and is not retried; a non-Model
  catalog entry negative-caches.
- `chunk_source_for` `#[test]`s: an embedded sub-asset returns `{path, offset, length}` for the right
  TOC entry; a remapped sub-asset returns the external path; a remap whose external file is gone falls
  back to the embedded chunk with a warn; an unknown sub-id returns an empty `ByteSource`.
