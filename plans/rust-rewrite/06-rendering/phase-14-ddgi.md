# Phase 14 — DDGI: voxel-traced dynamic diffuse global illumination

**Status:** COMPLETED

**Depends on:** 06-rendering:phase-8-ibl-sky-probes

## Goal

Port Dynamic Diffuse Global Illumination: an irradiance probe volume updated each frame by a software
voxel-ray trace, sampled in the mesh fragment for multi-bounce indirect. The chain is five compute
passes — voxelize the scene proxy, trace probe rays, blend rays into the irradiance atlas, blend into the
moment (distance) atlas, copy the octahedral gutter borders. Octahedral atlases with 1-texel gutters; a
3D voxel proxy rebuilt every frame. Off by default (it adds several passes/frame).

## Why this shape (NO LEGACY)

- **The 3D voxel proxy is an `Image3D` (phase 3) imported via `import_image3d`** — the graph tracks its
  layout exactly like a 2D image for compute storage barriers (`render_graph.cppm:411`). The `Ddgi`
  sub-state owns it plus the two octahedral atlases (`irradiance` rgba16f, `distance` rg16f moments) and
  the per-frame ray image (`renderer_types.cppm:1583`).
- **Five passes, each declaring its storage usages so the graph derives the GENERAL barriers**:
  `ddgi-voxelize` (3D storage write + box SSBO read), `ddgi-trace` (voxel storage + irradiance sampler +
  ray storage write), `ddgi-blend-irr` (ray sampler → irradiance storage), `ddgi-blend-dist` (ray
  sampler → distance storage), `ddgi-border` (the octahedral gutter copy)
  (`renderer.cppm:~1730`–`:1850`). Each has its own descriptor-set layout in `Ddgi`.
- **`set_ddgi_scene` feeds the per-frame scene box SSBO (world AABBs + albedo) + the volume placement +
  sun/sky** (`renderer.cppm:3018`). The volume is fit to the scene AABB each frame; the box buffer is
  grow-only. The renderer carries the sun/sky as plain fields (no scene import), the same decoupling as
  IBL.
- **Temporal: `history_reset` is true on the first frame after enable/resize** (no temporal blend then);
  `frame_index` rotates the trace ray set (`renderer_types.cppm:1587`,`:1595`). These are owned by `Ddgi`,
  mutated through its methods.
- **The mesh fragment samples the DDGI irradiance + distance atlases via set 5** when DDGI ran this frame
  (`FrameGraphState.has_ddgi`, the `ddgiIrradiance`/`ddgiDistance` resource handles,
  `renderer_types.cppm:1711`). One DDGI path, one `use_ddgi` toggle.

## Grounding (real files/symbols)

- `engine-old/source/saffron/rendering/renderer.cppm` — the five `ddgi-*` passes in `beginFrameGraph`
  (`:~1685`–`:1852`), `setDdgi` (`:3004`), `setDdgiScene` (`:3018`), `ddgiEnabled` (`:3013`).
- `engine-old/source/saffron/rendering/renderer_types.cppm` — `Ddgi` (`:1583`, the atlases, the voxel
  `Image3D`, the box SSBO, the layouts/sets, the volume + sun/sky + temporal fields), `Image3D` (`:1515`),
  `FrameGraphState.hasDdgi`/`ddgiIrradiance`/`ddgiDistance` (`:1711`,`:1717`).
- `engine-old/source/saffron/rendering/render_graph.cppm` — `importImage3D` (`:411`).
- Shaders: `ddgi_voxelize`, `ddgi_trace`, `ddgi_blend_irradiance`, `ddgi_blend_distance`, `ddgi_border`.
- README §6.

## Acceptance gate

- `cargo build -p saffron-rendering` and the workspace build are green.
- `cargo test -p saffron-rendering` passes named tests:
  - the five DDGI passes appear only when `use_ddgi` is on and the resources are ready; absent otherwise.
  - the box SSBO grows to the scene box count; the volume placement matches the scene AABB fit.
  - `history_reset` is set on enable and after a resize, cleared on subsequent frames.
- **Golden-image** test: a closed box scene with one emissive wall develops a committed color bleed onto
  the opposite wall after K frames of DDGI accumulation (the multi-bounce indirect is correct).
  Validation log clean across the five-pass chain incl. the 3D-image storage barriers.
