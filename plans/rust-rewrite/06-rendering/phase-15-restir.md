# Phase 15 — ReSTIR DI: reservoir spatiotemporal importance resampling

**Status:** COMPLETED

**Depends on:** 06-rendering:phase-13-ray-tracing, 06-rendering:phase-9-screen-space-gi

## Goal

Port ReSTIR DI — stochastic many-light direct lighting via reservoir resampling. Three compute passes:
initial candidate sampling (K candidates per pixel from the froxel candidate lists), temporal + spatial
reuse, and resolve (one shadow ray per pixel via the TLAS, then shade). Per-pixel reservoir SSBOs +
a resolved-radiance image, per-view (so two views never read each other's reservoirs). Gated on
`rt_supported` (needs ray-query) + the froxel cull + the G-buffer + the TLAS. Diffuse direct only in v1.

## Why this shape (NO LEGACY)

- **Device-shared scaffolding in `Restir`, per-view state in `RestirView`** — the same split as the
  screen-space effects (README §2). `Restir` (`renderer_types.cppm:1671`) holds the sampler + the four
  descriptor-set LAYOUTS + the candidate count K; `RestirView` (`:1685`) holds the per-pixel reservoir
  SSBOs (initial / combined / previous), the resolved-radiance image, the descriptor SETS binding them,
  and the per-view temporal state (`frame_index`, `history_reset`). Sized to the view's pixel count,
  recreated with the offscreen.
- **Three passes declaring their accesses; the resolve consumes the TLAS** — `restir-initial`,
  `restir-reuse`, `restir-resolve` (`renderer.cppm:~1900`–`:1990`). The resolve binds set 6 (the TLAS,
  phase 13) for the single visibility ray; the initial pass reads the froxel candidate lists (the cluster
  SSBO, phase 7); temporal reuse reads the previous combined reservoir + motion (phase 10). The
  dependency wiring (`do_restir` requires `rt_supported && tlas_ready && has_gbuffer && do_cull`) is the
  C++ gate exactly (`renderer.cppm:781`).
- **The reservoir record is `#[repr(C)]` + bytemuck, size-asserted** — it is an SSBO element the shaders
  read by std430 layout; the byte layout is pinned like the other GPU structs (README §3).
- **The mesh fragment samples the resolved direct radiance via set 7 when ReSTIR ran**
  (`FrameGraphState.has_restir`, the `restirRadiance` handle, `renderer_types.cppm:1713`). One ReSTIR
  path, one `use_restir` toggle; when off, direct lighting takes the clustered-forward path (phase 7).
- **Temporal reset on enable/resize via `history_reset`** (per-view), the same discipline as TAA/DDGI.

## Grounding (real files/symbols)

- `engine-old/source/saffron/rendering/renderer.cppm` — the `restir-initial`/`restir-reuse`/
  `restir-resolve` passes in `beginFrameGraph` (`:~1900`–`:1990`), the `do_restir` gate (`:781`),
  `setRestir` (`:2954`), `restirEnabled` (`:2964`), `resetViewTemporal` (`:2970`).
- `engine-old/source/saffron/rendering/renderer_types.cppm` — `Restir` (`:1671`), `RestirView` (`:1685`),
  `FrameGraphState.hasRestir`/`restirRadiance` (`:1713`,`:1719`), the per-view `restirViews[ViewCount]`
  array on `Renderer` (`:1755`).
- Shaders: `restir_initial`, `restir_reuse`, `restir_resolve`.
- README §2 (device-shared vs per-view), §6.

## Acceptance gate

- `cargo build -p saffron-rendering` and the workspace build are green.
- `cargo test -p saffron-rendering` passes named tests:
  - ReSTIR is a no-op when any gate is unmet (no RT, no cull, no G-buffer, no TLAS) and when
    `use_restir` is off; direct lighting falls back to clustered forward.
  - the reservoir record size/offset asserts; the per-view reservoir buffers size to the view pixel count.
  - switching the active view uses that view's reservoirs (no cross-view aliasing); `history_reset` on
    enable/resize.
- On an RT-capable device — including the toolbox's software lavapipe, which advertises the ray-query
  extension and traces this path in software (correct but slow): a **golden-image / metric** test where a
  many-light scene resolved by ReSTIR converges to within a noise tolerance of a reference brute-force
  many-light render after K frames. Validation log clean across the three-pass chain.
