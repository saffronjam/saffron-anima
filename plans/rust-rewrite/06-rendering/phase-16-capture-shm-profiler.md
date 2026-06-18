# Phase 16 — capture, the shm publish interface, thumbnails, and the profiler

**Status:** COMPLETED

**Depends on:** 06-rendering:phase-11-tonemap-grid-overlay

## Goal

Port the renderer's read-back / publish surface and the instrumentation: the offscreen→shm BGRA8 publish
(the editor's frame transport, FROZEN byte layout), window/asset screenshot capture, the thumbnail render
paths (mesh / material / model / texture → PNG, on a worker thread), and the GPU/CPU profiler (per-pass
timestamps, pipeline-statistics, the bounded capture state machine, the frame-history ring + alarms).
This closes the rendering area: the editor can display frames, scrub thumbnails, and read live timings.

This phase owns the *renderer-side* shm record + the GPU blit/copy that fills it. The shm seqlock ABI
and the host/run-loop wiring are co-designed with `08-host-and-viewport` (PP-10), against the unchanged
`editor/src-tauri/src/wayland_viewport.rs` reader as the byte-exact oracle.

## Why this shape (NO LEGACY)

- **The shm publish reproduces the FROZEN byte layout exactly** — a 32-byte header `[magic, width,
  height, seq, ringSlots, slotCapacity, 0, 0]` followed by `ringSlots` fixed-capacity BGRA8 frames; frame
  `s` lands in ring slot `s % ringSlots`; `seq` bumps last under a `fence(Release)` so a reader seeing the
  new `seq` is guaranteed the matching w/h + pixels (`renderer_capture.cpp:129`,`:291`,`:301`). The Rust
  port uses `rustix`/`memfd` for `shm_open`/`ftruncate`/`mmap` and `std::sync::atomic::fence(Release)`
  for the seqlock — easier to get right than the C++, validated frame-by-frame against the reader oracle.
  The per-frame-in-flight `ShmPublishSlot` (a BGRA8 image the GPU blits into + a persistently-mapped
  staging buffer the CPU memcpys from once the slot's fence signals — no `waitIdle` on the path) is the
  same pipelined design. This is the `08-host` unsafe seam; the renderer side records the blit/copy.
- **Screenshot capture waits idle (the offscreen may still be sampled) — an out-of-band path, never on
  present** (`captureViewport`, `renderer_capture.cpp:47`). The pending swapchain screenshot is deferred
  to `end_frame` because the swapchain image is only safely owned in-frame (`captureNextSwapchainPath`,
  `renderer_types.cppm:1791`).
- **Thumbnails run on a worker thread with its own command pool** (`workerCommandPool`,
  `renderer_types.cppm:1795`) because Vulkan command pools are not thread-safe; the worker binds it once
  (`bindThumbnailWorkerThread`, `:1972`). The worker's queue submit takes `gpuQueueMutex`, its uploads
  take `bindlessMutex` (README §5) — the two `Arc<Mutex>` sites. `prewarmThumbnailResources` builds the
  lazy preview pipelines on the main thread first so the worker never races init. PNG encode uses the
  `image` crate (or an stb binding if hash-parity is needed); `PngTransfer::{Clamp, Tonemap}` selects the
  HDR mapping (`:1940`).
- **The profiler is fully optional and zero-cost when `Off`** (`ProfilerMode::Off` default,
  `renderer_types.cppm:666`). Timestamps/stats are written via the `RgTimestamps`/`GpuScope` hooks the
  render graph (phase 2) already carries as nullable parameters — armed here. The capture state machine
  (`CaptureRecorder`, `:846`), the frame-history ring (`FrameSample`/`FrameHistoryStats`, `:871`/`:881`),
  the perf config (`PerfConfig`, `:898`), and the alarm ring (`AlarmState`, `:972`) port 1:1. The
  read-back is non-blocking (read `MaxFramesInFlight` frames later, after the slot's fence). Calibrated
  timestamps (`GpuCalibration`, `:740`) correlate GPU spans onto the CPU clock when the extension exists,
  degrading gracefully when absent.

## Grounding (real files/symbols)

- `engine-old/source/saffron/rendering/renderer_capture.cpp` — `captureViewport` (`:47`),
  `publishShmPublishSlot` (`:270`), the header init / shm_open / mmap (`:143`–`:201`), the release fence
  (`:291`,`:301`), `recordShmPublishCopy` (in `renderer.cppm:2389`).
- `engine-old/source/saffron/rendering/renderer_types.cppm` — `ShmPublish`/`ShmPublishSlot` (`:1095`/
  `:1083`), `captureNextSwapchainPath` (`:1791`), `workerCommandPool` (`:1795`), `PngTransfer`/
  `ThumbnailPng` (`:1940`/`:1948`), the whole profiler stack (`ProfilerMode` `:666`, `PipelineStats`
  `:678`, `PassTiming` `:695`, `GpuProfiler` `:750`, `CpuProfiler` `:775`, `ProfileCapture` `:817`,
  `CaptureRecorder` `:846`, `FrameSample` `:871`, `PerfConfig` `:898`, `AlarmState` `:972`,
  `GpuCalibration` `:740`).
- `engine-old/source/saffron/rendering/renderer_thumbnail.cpp` — `bindThumbnailWorkerThread` (`:1125`),
  the thumbnail/PNG render+encode paths.
- `engine-old/source/saffron/rendering/renderer_profiler.cpp` — `readbackGpuTimings`, `tickCapture`,
  `tickAlarms`, `calibrateTimestamps`, `readVramBudget`.
- `engine-old/source/saffron/rendering/render_graph.cppm` — the `RgTimestamps`/`GpuScope`/`CpuScope`
  hooks (`:147`,`:245`,`:225`) phase 2 stubbed, armed here.
- `editor/src-tauri/src/wayland_viewport.rs` — the byte-exact shm reader oracle (PP-10 shares it).
- README §5 (the `Arc<Mutex>` sites), §6.

## Acceptance gate

- `cargo build -p saffron-rendering` and the workspace build are green.
- `cargo test -p saffron-rendering` passes named tests:
  - the shm header is byte-identical to the C++ layout (magic + field order + the `seq=0`-until-first-
    frame rule); a published frame's seqlock is read back consistently (no torn read).
  - a thumbnail render on a spawned worker thread (with its own pool) produces a PNG without racing the
    main-thread frame; the worker's bindless uploads + queue submits hold the two mutexes.
  - the profiler is exactly zero added work when `Off` (no query pools allocated); `Timestamps` mode
    produces per-pass spans; the capture state machine arms→records→ready→drains; the frame-history ring
    saturates at capacity and the percentiles compute.
- The frozen-wire **parity gate**: a Rust-published shm frame is displayed correctly by the *unchanged*
  `wayland_viewport.rs` reader (the PP-10 go/no-go oracle), proving byte-compatibility. Validation log
  clean.

## Substrate added for the control plane (post-completion)

The asset/scene command ports (09-control-plane #81 / #100) reach through the renderer for the
multi-view + screenshot operations the C++ commands call (`control_commands_asset.cpp`,
`control_commands_scene.cpp`). These were filled in `crates/rendering/src/renderer.rs` +
`view_target.rs`, matching engine-old's `renderer.cppm` / `renderer_capture.cpp`:

- **`ViewId` (`Scene = 0`, `AssetPreview = 1`) + `VIEW_COUNT`** — the typed editor-pane selector (the C++
  `ViewId`/`ViewCount`), with FROZEN wire tokens (`"scene"` / `"assetPreview"`) + dense slot indices that
  match the host's per-view shm segments and the presenter's reader ordering.
- **Both views are created at startup** (the init loop over `renderer.views`) — previously only the scene
  view existed; the asset-preview pane now has its own offscreen + screen-space + AA + ReSTIR targets so
  a `set-active-view assetPreview` and its shm segment render immediately.
- **`set_active_view(ViewId)`** (resets the newly-shown view's temporal accumulators) +
  `active_view_id()`; **`set_viewport_desired_size(ViewId, w, h)`** now targets a specific view and
  records its `desired_width`/`desired_height` (`ViewTarget`); **`view_desired_width/height(ViewId)`** read
  it for the seed-on-first-activate check (the C++ `previewView.desiredWidth == 0`).
- **`capture_viewport(path)`** — the screenshot path (the C++ `captureViewport`): idle, copy the active
  view's offscreen into a host buffer through a one-off submit, leave it `ShaderReadOnly`, and write the
  PNG (`PngTransfer::Clamp` via `write_png_file`).
- Tests: `view_id_wire_tokens_and_indices_are_frozen`,
  `per_view_targets_size_independently_and_capture_writes_a_png` (the per-view sizing + the offscreen →
  PNG capture pipeline, validation-clean on llvmpipe).

## Deferred seams closed (post-completion)

The substrate pass left four host seams returning graceful errors because `saffron-rendering` lacked the
backing GPU primitives. These are now LIVE, ported from `renderer_thumbnail.cpp` / `renderer_capture.cpp`
into `crates/rendering/src/thumbnail_render.rs` + `render_settings.rs` + `renderer.rs`, and wired through
the host's `ThumbnailGpu` / `ControlRenderer` seam (`crates/host/src/control_renderer.rs`):

- **Offscreen thumbnail render → PNG** (the C++ `renderMeshThumbnail` / `renderModelThumbnail` /
  `encodeAssetThumbnailPng` / `encodeModelThumbnailPng` / `encodeTextureThumbnailPng`). A `ThumbnailRenderer`
  sub-state on `Renderer` holds the lazy mesh-thumbnail + material-preview PSOs + the unit preview sphere
  (built from `thumbnail.spv` / `preview.spv`), allocates a `size`×`size` MSAA-resolved color + depth
  target, draws the framed geometry through a one-off submit, transitions the result to
  `SHADER_READ_ONLY`, and reads it back to a PNG via a chained 2× linear-blit downscale pyramid
  (matching the C++ undersampling fix). `get-thumbnail` / `view-asset` are now LIVE.
- **Material-preview render** (the C++ `renderMaterialPreview`, now taking the codegen-SPIR-V argument).
  `render_material_preview(material, size, shader_spv)` renders the studio-lit sphere with the bindless
  set + the 112-byte `PreviewPush`; `shader_spv: None` uses the cached default preview pipeline, a codegen
  material passes its compiled `_preview.spv` for a fresh per-call PSO. `preview-render` is now LIVE
  end-to-end: the assets-side `ThumbnailGpu::render_material_preview` carries the
  `shader_spv: Option<&Path>` argument, the host seam forwards it to the renderer, and the
  `preview-render` command compiles the `_preview.spv` for a non-foldable graph (the C++ `codegenSpv`
  block: load raw → non-empty graph that does not fold → `compile_material_preview_shader`) and passes it
  through. The off-thread worker tile keeps `None` (the cached studio preview), matching the C++ worker.
- **Window/composited-output capture** (the C++ `requestWindowCapture` + the swapchain-present capture
  branch). `request_window_capture(path)` arms `capture_next_window_path`; the next `render_frame` copies
  the just-presented swapchain image (the composited window output, distinct from `capture_viewport`'s
  offscreen) to a host buffer → PNG. Gated on the surface's `TRANSFER_SRC` capability.
  `screenshot {target:window}` is now LIVE.
- **Project `renderSettings` serde** (the C++ `renderSettingsToJson` / `applyRenderSettings`).
  `render_settings_to_json()` serializes the AA mode + exposure + the feature toggles; `apply_render_settings(&Value)`
  applies a saved block (missing field → keep current, RT toggles only on RT hardware). Wired into
  save-project / load-project through the host's `ControlRenderer`.
- Tests (`thumbnail_render.rs` + `render_settings.rs` + `tests/swapchain_present.rs`):
  `mesh_thumbnail_renders_nontrivial_pixels`, `material_preview_renders_nontrivial_pixels`,
  `prewarm_builds_then_renders_reuse_the_caches`, `texture_thumbnail_downscales_to_fit` (offscreen render
  → non-trivial readback bytes, validation-clean on lavapipe); `render_settings_block_has_the_frozen_keys`,
  `parse_then_serialize_round_trips_every_field`, `missing_and_malformed_fields_parse_to_none` (the
  serde round-trip, pure logic); `window_capture_writes_a_png_of_the_composited_output` (the window capture
  on the toolbox weston surface — needs a real present surface, so it skips off a display).
  The codegen-spv plumbing closes with `preview_codegen_spv_picks_the_pipeline_per_graph`
  (`crates/control/src/commands_asset.rs`): a graph-less and a foldable material take the default studio
  preview (`None`); a non-foldable procedural graph compiles + passes its on-disk `_preview.spv`
  (slangc-gated, degrades to the default off-slangc — never an error).
