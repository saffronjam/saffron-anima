# Phase 13 — Diffing reimport preserving overrides

**Status:** COMPLETED
**Depends on:** 12

> Implementation note: `reimportModel` skips when `hashFileFnv(source) == stored` + importer version
> matches; else `translateModel` (deterministic, same sub-ids) → `bakeModel` reusing the modelId →
> diff old vs new sub-ids (`updated`/`added`/`removedFromSource`, the last kept + reported, never
> dropped) → preserve the remap for surviving sub-ids (extracted edits survive) via a META rewrite →
> refresh catalog rows + drop sub-id GPU caches so live instances re-resolve with no re-instantiation.
> `reimport-model` command idles the GPU first. Verified by the GPU-free `runReimportSelfTest`
> (skip-unchanged → re-bake-on-drift → re-skip) and the 131-check contract test. The full edit-source +
> render-diff e2e lands in phase 18.

## Goal

Implement `reimportModel`: compare the container's stored `sourceHash` + `importerVersion` against the
current source (skip if unchanged); otherwise re-bake with the **stored `ImportOptions`** (never current
defaults), diff sub-assets by stable sub-id, **preserve remapped externals** (never clobber an extracted
edit), keep the `remap` + `options` in the MetadataChunk, and update `sourceHash`. Live entities pick up
changes by `(modelId, subId)` with **no re-instantiation**. Add a `reimport-model` command + e2e. Defers:
the reference graph (14), cleanup (15).

## Why

This is UE's deterministic replay + Unity's "extracted materials survive reimport." It's what makes the
asset durable across source edits: tweak the glTF in Blender, reimport, and the scene updates — while any
material you extracted and hand-edited is untouched. Determinism (phase 04) is what makes the skip-if-
unchanged fast path correct.

## `reimportModel`

```cpp
struct ReimportDelta { std::vector<Uuid> updated, added, removedFromSource; bool skipped = false; };

std::expected<ReimportDelta, std::string>
reimportModel(AssetServer& assets, Renderer& renderer, Uuid modelId);
```

Recipe:
1. `readContainerMetadata` → `import.sourcePath`, `import.sourceHash`, `import.importerVersion`,
   `import.options`, and the existing `remap`.
2. Hash the current source bytes. If `hash == sourceHash` **and** `importerVersion` matches → set
   `skipped=true`, return (content-addressed skip).
3. Else `translateModel(source, ImportOptions::fromJson(import.options))` (phase 04 — stored options, not
   defaults) → a fresh graph with the **same stable sub-ids** for matching source elements.
4. **Diff by sub-id:** matched → update the chunk bytes; new (added in source) → add a chunk + sub-asset
   row; missing-from-source → flag in `removedFromSource` (do **not** silently drop a still-referenced one;
   keep it and report — phase 14/15 decides its fate).
5. **For any sub-id present in `remap`:** keep the external override; discard the freshly-baked chunk
   content for it (never overwrite the user's extracted edit). The chunk may still be rewritten as a dormant
   fallback, but the remap stays authoritative.
6. Rewrite the container preserving `remap` + `options`; update `sourceHash`.
7. Patch GPU caches by sub-id (phase 06) after `waitGpuIdle`; **live entities need no re-instantiation** —
   they resolve by `(modelId, subId)` and pick up the new bytes.
8. Surface a dirty indicator when a source hash drifts (the editor can show "source changed" — phase 16).

## Files to touch

- `engine/source/saffron/assets/assets.cppm` — `reimportModel`, `ReimportDelta`; the sub-id diff; the
  remap-preservation rule; GPU-cache refresh.
- `engine/source/saffron/control/control_commands_asset.cpp` + `control_dto.cppm` + `gen.ts` —
  `reimport-model` command + DTO + regen.

## Steps

1. Add source hashing + the skip-if-unchanged fast path.
2. Implement the sub-id diff against a re-translated graph (reusing phase 04's deterministic ids).
3. Implement the remap-preservation rule (extracted sub-assets survive).
4. Rewrite the container; refresh GPU caches under `waitGpuIdle`; confirm live instances update.
5. `reimport-model` command + DTO + `gen.ts` + fixtures.
6. e2e: import → instantiate → extract a material (phase 12) → edit the source file → `reimport-model` →
   assert the mesh/other materials update, the **extracted material is unchanged**, the instance reflects
   the new geometry **without re-instantiation**, and a no-op reimport (unchanged source) is `skipped`.

## Gate / done

- `make engine` clean; the reimport e2e proves skip-if-unchanged, source-edit propagation to live
  instances, and extracted-edit survival; `make e2e` + contract test pass; `make prepare-for-commit` clean.

## Risks

- **Non-determinism breaks the skip + diff:** if `translateModel` isn't reproducible (cgltf ordering, float
  drift — phase 04 risk), `sourceHash` churns and the diff mismatches sub-ids. The determinism self-test
  (phase 04) is the guard; reuse it here.
- **Clobbering an extracted edit:** the classic data-loss bug. The remap check (step 5) must run before any
  chunk write for that sub-id; the e2e explicitly asserts survival.
- **Removed-from-source while referenced:** a sub-asset deleted in the source but still used by a scene must
  not vanish silently; report it (`removedFromSource`) and let cleanup (15) handle it deliberately.
- **Whole-container rewrite cost:** like extraction (12), updating the META/chunks rewrites the file;
  acceptable for v1, but note it for large containers.
