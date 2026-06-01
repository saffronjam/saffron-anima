# Phase 3 — Adopt 1.4-core features (optional)

**Status:** NOT STARTED
**Depends on:** phase 2

Optional and open-ended. After the floor is at 1.4, several previously-optional extensions are
guaranteed core, and minimum limits are higher. None of this is required — pick an item only
when it actually simplifies or unblocks something. Each item is an independent small change with
its own verify + doc note; this phase can sit `NOT STARTED` forever.

## Candidates (with where they'd land)

- **Push descriptors core** (`VK_KHR_push_descriptor` is 1.4 core). Could replace some per-frame
  descriptor-set writes with `vkCmdPushDescriptorSet` for the small, frequently-rewritten sets
  (the per-frame lighting/screen-space binds in `renderer.cppm` / `renderer_pipelines.cpp`).
  Win: fewer descriptor-set allocations + writes per frame.
- **Raised minimum limits.** 1.4 guarantees higher `maxPerStageDescriptor*` / update-after-bind
  counts. Lets the bindless albedo array (`MaxBindlessTextures`, set 0 in `mesh.slang` /
  `renderer_textures.cpp`) and any per-device limit clamps rely on a guaranteed floor instead of
  querying. Win: drop a runtime check or two; safely grow the array.
- **`VK_KHR_dynamic_rendering_local_read`** (1.4 core). On-tile input-attachment reads within a
  dynamic-rendering pass. Would let a future G-buffer + lighting fold into one pass without a
  separate sampled-read round-trip (relevant to `frame-and-render-graph` / the screen-space MRT).
  Only worth it if a one-pass deferred path is on the table.
- **maintenance5 / maintenance6** (1.4 core). Quality-of-life: `vkCmdBindIndexBuffer2` (size +
  offset), better default-state queries. Minor; adopt incidentally if touching those call sites.

## Per-item recipe

1. Flip the relevant bit on the `features14` struct wired in phase 2 (or just use the core entry
   point — promoted functions need no extension enable at 1.4).
2. Refactor the one call site; keep the change small and isolated.
3. Verify: `-j1` build, validation-clean, pixel-identical frame (or the intended visual delta),
   `se render-stats` where it applies.
4. Update the matching `docs/` explanation page in the same change (the "Keep `docs/` current"
   rule), and add an `se` toggle if the item introduces user-visible state.

## Done when

- Per item: the feature is adopted, verified, and documented. There is no "all done" bar for this
  phase — close items individually, and delete this file once nothing here is worth pursuing.
