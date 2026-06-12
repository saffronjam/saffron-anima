# Phase 12 — Sub-asset extraction + remap table

**Status:** COMPLETED
**Depends on:** 11

> Implementation note: `extractSubAsset` slices the chunk → standalone file (keeping the sub-id) →
> standalone catalog row → `remap[subId]={external}` written via `rewriteContainerMeta` (the simplest
> correct v1: re-emit META + every payload verbatim). `clearExtraction` reverts (drop remap, **delete**
> the external file to prevent its uuid name aliasing the embedded chunk on a later scan, revert the
> row). The id-aliasing footgun is closed by making `catalogRowsForModel` honor the remap (emit the
> standalone form for remapped sub-assets), so a scan agrees with the resolver. `extract-subasset` +
> `clear-extraction` commands take a model selector + sub-asset WireUuid. Verified by the GPU-free
> `runExtractSelfTest` (extract → remap + standalone + resolver reads external; clear → reverts) and the
> 130-check contract test. The command-level full-flow e2e (with sub-asset discovery via `model-info`)
> lands in phase 18.

## Goal

Implement `extractSubAsset(model, subId, dest)`: slice the chunk's byte range out of the `.smodel` and
write it as a standalone file (`.smesh` / `.smat` / `<ext>` image) **keeping the same sub-id**, add a
standalone catalog row, and write a **remap record** into the container's MetadataChunk so resolution
prefers the external file. A missing remap target falls back to the embedded chunk **with a warning**.
Clearing the remap reverts to embedded. Add an `extract-subasset` control command + DTO + e2e. Defers:
diffing reimport that preserves these overrides (13).

## Why

This is the Unity "Extract Materials / Extract Textures" workflow and the answer to the user's "extract if
I want to." Because every embedded sub-asset already has a stable sub-id (phase 04) and the TOC is
offset-addressed (phase 01), extraction is a **byte-range copy + a remap entry**, no rehydration — and
because the id is preserved, every existing scene reference keeps resolving through the indirection.

## `extractSubAsset`

```cpp
// Slice subId's chunk to a standalone file (keeping subId), register it, and remap the container to it.
std::expected<AssetRef, std::string>
extractSubAsset(AssetServer& assets, Uuid modelId, Uuid subId, std::filesystem::path dest /* "" ⇒ default */);

// Revert: drop the external file's authority; resolution falls back to the embedded chunk.
std::expected<void, std::string>
clearExtraction(AssetServer& assets, Uuid modelId, Uuid subId);
```

Steps it performs:
1. `loadModelAsset` → find the `subId` TOC entry → `readChunk` its bytes.
2. Write the bytes to `dest` (default: `materials/<subId>.smat`, `models/<subId>.smesh`, or
   `textures/<subId>.<ext>` per type). The chunk is already a valid standalone file image (phase 05 reused
   the standalone encoders), so no transform is needed.
3. Add/update a standalone `AssetEntry` (`container=0`, `path=<dest>`); the **id stays `subId`** so `byId`
   now points the id at the standalone file.
4. Write `remap[subId] = { external: <relative dest> }` into the container's MetadataChunk and re-emit just
   the META chunk (rewrite the container, or update-in-place if the META size is unchanged — see Risks).
5. The resolver (phase 06) already prefers `remap` over the embedded chunk; warns + falls back if the
   external file is missing.

## Files to touch

- `engine/source/saffron/assets/assets.cppm` — `extractSubAsset`, `clearExtraction`; META rewrite helper;
  catalog-row update keyed by the preserved sub-id.
- `engine/source/saffron/control/control_dto.cppm` + `control_commands_asset.cpp` + `tools/gen-control-dto/gen.ts`
  — `extract-subasset` (+ optional `clear-extraction`) command + DTO + regen.

## Steps

1. Implement the slice → standalone-file write, choosing the default dest by sub-asset type.
2. Update the catalog: turn the sub-asset row into a standalone row under the same id; reconcile `byId`.
3. Write the `remap` entry and re-emit the META chunk; confirm `readContainerMetadata` reflects it.
4. Verify the resolver prefers the external file; add the missing-target warning path.
5. `extract-subasset` command + DTO + `gen.ts` + fixtures.
6. e2e: bake a model, `extract-subasset` a material → assert a standalone `.smat` exists with the same id,
   the container's `remap` references it, an instantiated entity now resolves the external material, and
   editing the external `.smat` changes the render; `clear-extraction` reverts to embedded.

## Gate / done

- `make engine` clean; the extract/clear e2e passes (same-id standalone file + remap + resolver preference
  + revert); `make e2e` + contract test pass; `make prepare-for-commit` clean.

## Risks

- **META rewrite invalidates payload offsets:** if writing the remap grows the META chunk, every payload
  offset shifts — you must rewrite the whole container (or reserve META slack / append a remap side-chunk).
  Simplest correct v1: rewrite the container via `writeContainer` from the loaded chunks + updated META.
- **Id aliasing:** after extraction the same `subId` maps to a standalone file *and* still exists as an
  embedded chunk; `byId` must point at the standalone row, and the resolver must use `remap` to choose —
  keep one authority (the remap) to avoid divergence.
- **Dest collisions:** a default dest of `<subId>.<ext>` is collision-free by construction; a user-chosen
  dest could clash — validate and refuse to overwrite an unrelated asset.
- **Extraction must survive reimport:** the remap entry is what phase 13 reads to avoid clobbering; ensure
  it's stored in the MetadataChunk (not only in-memory) so it persists.
