# Phase 13 — RT acceleration structures: per-frame TLAS + ray-query shadows

**Status:** COMPLETED

**Depends on:** 06-rendering:phase-12-skinning-prepass

## Goal

Port hardware ray tracing: per-mesh BLAS (built at upload when RT is supported), a per-frame TLAS over
the scene's mesh instances, the per-skinned-instance refit BLAS (built once, refit in place each frame
from the deformed buffer), and inline ray-query shadows in the mesh fragment. All of it is feature-gated
on `rt_supported` (KHR acceleration-structure + ray-query present + enabled) and routed through the
resolved `RtDispatch` PFN table — there is no static linkage of RT entry points.

## Why this shape (NO LEGACY)

- **RT entry points are resolved function pointers on `Device`, called through `RtDispatch`** — exactly
  the C++ pattern (`renderer_types.cppm:1019`,`:427`). The `AccelerationStructure` wrapper (phase 3)
  holds the resolved `destroy_accel` PFN for its Drop. ash exposes these as extension structs loaded per
  device, the same explicit pattern.
- **The TLAS is per-frame-in-flight (ping-ponged), with grow-only instance + scratch buffers** (`Rt`,
  `renderer_types.cppm:1631`). `set_rt_scene` captures this frame's static instance transforms + meshes
  (`frameModels`/`frameMeshes`); the `tlas-build` compute-kind pass builds it (`renderer.cppm:2876`).
  Cleared each frame in `begin_frame`.
- **The skinned refit BLAS is per-frame-in-flight then keyed by entity uuid; BUILD on first sight, then
  MODE_UPDATE (refit in place)** (`Rt.skinnedBlas`, `renderer_types.cppm:1657`). It is per-slot because an
  in-place UPDATE rewrites the AS while frame N's GPU work may still trace the same slot's prior contents;
  the per-slot fence wait in `begin_frame` serializes each slot. The deformed vertices are already in
  world space (phase 12), so the TLAS transform for a skinned instance is identity. The refit reads the
  deformed buffer via `AccelStructBuildRead` (the access stubbed in phase 12 becomes live).
- **Ray-query shadows are inline in the mesh fragment (set 6 = the TLAS), one visibility ray per pixel**,
  gated by `use_rt_shadows` (only meaningful when `rt_supported`). The mesh PSO already binds set 6 (the
  übershader handles the gate at runtime); no separate RT pipeline / shader binding table — this is
  ray-query, not ray-tracing-pipeline shadows. ReSTIR (phase 15) reuses the same TLAS for its visibility
  ray.
- **One TLAS, one refit-BLAS map — no parallel "RT v2".** The toggle is `set_rt_shadows`; when off, the
  build pass is skipped and the fragment takes the shadow-map path (phase 7).

## Grounding (real files/symbols)

- `engine-old/source/saffron/rendering/renderer.cppm` — `buildTlas` (`:2876`), the `tlas-build` pass in
  `beginFrameGraph` (`:~1872`), `setRtScene` (`:2865`), `setRtShadows` (`:2850`), `rtSupported` (`:2845`),
  `rtBlasCount` (`:2860`).
- `engine-old/source/saffron/rendering/renderer_types.cppm` — `Rt` (`:1631`), `SkinnedBlas` (`:1621`),
  `SkinnedRtInstance` (`:633`), `AccelerationStructure` (`:423`), `RtDispatch` (`:1019`), `GpuMesh.blas`
  (`:248`), the per-slot fence-wait note (`:1651`).
- `engine-old/source/saffron/rendering/renderer.cppm` — BLAS build at `uploadMesh` time (in
  `renderer_drawlist.cpp`), `begin_frame` per-slot fence wait + the RT frame-vector clear.
- Shader: `mesh.slang` (the ray-query shadow branch). README §6.

## Acceptance gate

- `cargo build -p saffron-rendering` and the workspace build are green.
- `cargo test -p saffron-rendering` passes named tests:
  - all RT paths are no-ops when `rt_supported == false` (a device lacking the extensions); the engine
    still renders via the shadow-map path.
  - `set_rt_scene` + `tlas-build` produces a TLAS whose instance count matches the static draw count;
    `rt_blas_count` reflects the built BLAS.
  - a skinned instance gets a BUILD on its first frame and an UPDATE thereafter (the per-entity map grows
    once, refits after).
- On an RT-capable device — including the toolbox's software lavapipe, which advertises the
  acceleration-structure + ray-query extensions and traces these paths in software (correct but slow): a
  **golden-image** test where RT shadows match the shadow-map golden within tolerance for a simple
  occluder. Validation log clean (TLAS build + refit barriers, incl. the `AccelStructBuildRead` on the
  deformed buffer).
