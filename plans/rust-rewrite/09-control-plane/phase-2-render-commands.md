# Phase 2 — Render command domain

**Status:** COMPLETED

**Depends on:** 09-control-plane:phase-1-socket-server-and-dispatch, 06-rendering (the renderer crate + its query/toggle surface)

## Goal

Register the 29 render-domain commands (`register_render_commands`) onto the registry: render stats,
the profiler/capture group (`profiler.*`), perf config, frame history, alarms, the anti-aliasing /
view-mode / clustering / IBL / SSAO / shadows / GI / skinning / depth-prepass toggles, the native
viewport info + size, exposure, and probe management. Every handler in this domain reaches only
`ctx.renderer` (53 hits, no other subsystem), making it the cleanest domain to port — a thin
DTO ↔ renderer-query/setter bridge.

## Why this shape (NO LEGACY)

- **One reach: `renderer`.** The grep is unambiguous — `register_render_commands` touches
  `ctx.renderer` and nothing else. So this phase depends only on `06-rendering`, not on scene/assets,
  and lands green the moment the renderer's query/toggle functions exist. The handlers are pure
  translation: read a DTO field → call a renderer setter → read back the renderer state → build the
  result DTO (e.g. `set-aa` maps `AaModeDto` to `{samples, fxaa, taa}` via `applyAaMode` and returns
  the applied state).
- **Toggles share `ToggleParams` / per-command result DTOs.** `set-clustered`/`set-ibl`/`set-ssao`/
  `set-contact-shadows`/`set-ssgi`/`set-rt-shadows`/`set-restir`/`set-shadows`/`set-skinning`/
  `set-depth-prepass` all take `ToggleParams` and return a distinct `Set*Result` — the distinct result
  types are kept (they carry the applied boolean back), not collapsed to a shared result, because the
  wire contract names them per-command (the editor and OpenRPC schema reference each).
- **`AaModeDto`/`ViewModeDto`/`GiModeDto`/`ProfilerModeDto` are kebab-case enums.** The wire spelling
  (`"fxaa"`, `"taa"`, `"msaa2"`, `"wireframe"`, `"timestamps"`, `"pipeline-stats"`) is frozen; the
  `aaModeDto` string→enum helper (`control_commands_render.cpp:23`) becomes the enum's `FromStr` /
  serde derive in `saffron-protocol`, the single translation place.
- **Probe + exposure commands belong to this phase** because they are registered in the render file in
  the C++ tree (renderer-side state), even though the manifest interleaves them — the README §3 split
  is by registration file. `set-probes`/`recapture-probes`/`list-probes`/`set-exposure`.
- **The profiler/capture group keeps its names dotted** (`profiler.set-mode`,
  `profiler.capture-start/-stop/-status`) — the dot is part of the frozen `cmd` string, not a Rust
  module path.
- **`software-gpu` / `*-supported` flags are reported, not assumed.** `profiler.set-mode` reports
  `timestampsSupported`/`pipelineStatsSupported`/`softwareGpu` from the renderer (`:510`) so the editor
  can grey out unsupported modes — kept verbatim (the llvmpipe path depends on it).

## Grounding (real files/symbols)

- `engine-old/source/saffron/control/control_commands_render.cpp`
  - `registerRenderCommands` (the 29-command block) + `aaModeDto`/`applyAaMode` (`:23`/`:47`).
  - Representative handlers: `render-stats` → `renderStatsDto(ctx.renderer)` (`:499`);
    `profiler.set-mode` → `setProfilerMode`/`profilerMode`/`*Supported`/`softwareGpu` (`:504`);
    `pass-timings` → `passTimingsDto` (`:516`).
  - `help` is registered in this file too but belongs to phase 1 (`:488`).
- DTOs: `RenderStatsDto`, `RenderPassTimingsDto`/`RenderPassTimingDto`, `ProfilerSetModeParams`/
  `ProfilerModeResult`, `CaptureStartParams`/`CaptureStartResult`/`CaptureStopResult`/
  `CaptureStatusResult`, `ProfileSpanDto`/`ProfileCaptureDto`/`ProfileCaptureMetadataDto`/
  `PipelineStatsDto`, `FrameHistoryParams`/`FrameHistoryDto`/`FrameSampleDto`, `PerfConfigDto`/
  `SetPerfConfigParams`, `AlarmEventDto`/`DrainAlarmsParams`/`DrainAlarmsResult`/`ActiveAlarmDto`/
  `ActiveAlarmsDto`, `SetAaParams`/`SetAaResult`, `SetViewModeParams`/`SetViewModeResult`,
  `ToggleParams` + the per-toggle `Set*Result`, `SetGiParams`/`SetGiResult`, `ViewportNativeInfoResult`,
  `SetViewportSizeParams`/`SetViewportSizeResult`, `SetExposureParams`/`SetExposureResult`,
  `SetProbesParams`/`SetProbesResult`/`RecaptureProbesResult`/`ProbeRef`/`ListProbesResult` — all in
  `control_dto.cppm`.
- Enums: `AaModeDto`, `GiModeDto`, `ViewModeDto`, `ProfilerModeDto`, `ProfileLaneDto`, `CaptureModeDto`,
  `CaptureStateDto`, `AlarmSeverityDto`, `AlarmStateDto` (`control_dto.cppm:96`+).
- `09-control-plane/catalog.md` — the render-domain table (29 rows) + fixtures.

## Acceptance gate

- `cargo build -p saffron-control` green with the render handlers registered; `cargo clippy` /
  `cargo fmt --check` clean.
- `cargo test -p saffron-control` passes render-domain unit tests over a stub/headless renderer:
  - `set-aa` with `{"mode":"msaa4"}` returns the applied `{samples:4,...}`; `"fxaa"`/`"taa"`/`"off"`
    each map correctly; an unknown mode is a typed error.
  - each `Toggle*` command echoes the applied boolean; `set-gi` with `"off"`/`"ddgi"` maps the enum.
  - `set-view-mode` with `"wireframe"` round-trips the kebab-case enum.
- The wire-contract test (`13-testing-and-verification` / `check-control-schema`) validates every
  render-domain command's live `result` against its generated OpenRPC schema and its `help` line
  against the manifest, using the catalog fixtures (`empty`, `toggle-on`, `aa`, `view-mode-wireframe`,
  `gi-off`, `profiler-timestamps`, `capture-single`, `frame-history-samples`, `perf-config-30`,
  `alarms-since-0`, `viewport-size`, `exposure-zero`).
- All ids in render results that carry them remain decimal strings (no regression to JSON numbers).

## Substrate added after completion (for phases 3/4)

The `ControlRenderer` seam this phase defines was extended with the asset/scene-domain renderer
surface (see `09-control-plane/README.md` §4): **view-select** (`set_active_view` +
`view_desired_size`/`set_view_desired_size`), **screenshot** (`capture_viewport`), **wait-gpu-idle**,
and the **GPU-upload** access point (`with_gpu_uploader`). The old active-only `set_viewport_size` was
**removed** (NO LEGACY) and `set-viewport-size` now targets the named `ViewId` via the per-view
`set_view_desired_size` (the prior handler validated the `view` param but sized the active view — a
latent bug, now fixed). The concrete `impl ControlRenderer for Renderer` moved out of `saffron-control`
into the host's `HostControlRenderer` (it bundles the host-owned `Uploader`); the control crate keeps
the trait + the test stub only.
