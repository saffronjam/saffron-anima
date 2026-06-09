+++
title = 'Renderer profiling'
weight = 9
+++

# Renderer profiling

The [performance HUD](performance-telemetry/) answers *is it slow, right now?* — a 1 Hz stream of
smoothed headline numbers. It cannot answer *which pass, this exact frame, and how much of it was
CPU versus GPU?* That is a different question with a different shape: precise, un-smoothed spans for a
single bounded window, correlated across the two clocks, nested into a tree, and handed back as a file
you can open in a trace viewer. That is what a capture is.

A capture is **request-scoped and opt-in**. The HUD streams continuously at low cost; a capture is
armed by an explicit command, runs for a bounded window, and is drained once. It carries overhead
(extra queries, the timestamps/pipeline-statistics mode), so it never rides the always-on lane.

## The capture model

A capture is a flat list of **spans**, each tagged with a lane, a name, a `[startNs, endNs)`
interval, and its nesting (`parentIndex` + `depth`). Two lanes share one timeline:

- **CPU spans** come from `CpuScope`, a RAII `steady_clock` marker around the frame-lifecycle phases
  (`build-frame-graph`, `execute-render-graph`, `submit-present`) and each pass body. Plain host
  memory, readable at end of frame.
- **GPU spans** come from `GpuScope`, which brackets each render-graph pass — and, opted in, its
  sub-passes — with a `VK_QUERY_TYPE_TIMESTAMP` pair. The tree is kept flat-and-tagged, not a literal
  nested structure, so the async-compute future collapses cleanly.

The flat-and-tagged form is deliberate: the consumer decodes the tree from `parentIndex`/`depth`,
which keeps one representation valid whether or not passes overlap.

## Why the per-pass numbers are *relative*

GPU passes overlap. The CPU records frame N+1 while the GPU still executes frame N, and adjacent GPU
passes can run concurrently, so the per-pass times do **not** cleanly sum to the frame total. The
frame's GPU total is the **span** from the earliest begin to the latest end, never a sum of children —
a parent pass brackets its sub-passes, so a naive last-record-end would be wrong. The UI labels the
per-pass share "% of span" for exactly this reason.

Timestamps are device ticks: a raw counter masked by `timestampValidBits` and scaled by
`timestampPeriod` (ns per tick). A queue with zero valid bits cannot time at all, and the read-back is
always non-blocking — a slot is read `MaxFramesInFlight` frames later, after its fence has signalled.

## One clock: CPU↔GPU correlation

GPU ticks and CPU `steady_clock` count from unrelated epochs. `VK_EXT_calibrated_timestamps` samples
both at one instant, giving an additive offset that projects a GPU tick onto the host axis
(`hostNs = tick · timestampPeriod + offset`). With it, the CPU and GPU lanes share a zero, so you can
see the GPU pass execute *after* the CPU submitted it, within the submit window. The offset is
re-sampled periodically to track drift.

When the extension or a matching host clock domain is absent, correlation is impossible. Rather than
fake it, the GPU lane stays on its own frame-relative zero and the capture's `correlated` flag is
false — the UI then draws two independent lanes, honest about the disconnect.

## Capture modes

- **`single`** — one frame, the default "what is this frame made of" snapshot.
- **`frames:N`** — a bounded window (hard-capped) for trend and consistency.
- **`rolling`** — a recent window; v1 records it forward like `frames`.

Arming forces `Timestamps` mode (or `PipelineStats` when stats are requested) plus sub-scopes for the
duration, then restores the prior mode on stop, so a capture never silently leaves the baseline host
instrumented.

## Pipeline statistics

The deepest mode adds a `VK_QUERY_TYPE_PIPELINE_STATISTICS` query per top-level pass — the answer to
*why* a pass is slow. The query lives **inside** the dynamic-rendering scope (a statistics query
cannot straddle `beginRendering`/`endRendering`, unlike a timestamp) and cannot nest, so it is
per-pass. The raw counts decode into the ratios that guide optimization: fragment invocations versus
rendered pixels (**overdraw**), clipping output versus input (**culling**), vertex-shader invocations
versus input vertices (**vertex reuse**), and compute invocations for the GI/lighting passes. The
counts are real pipeline-stage invocation counts — not timing — so they are meaningful even on a
software rasterizer.

## Interchange

A capture serializes to **Chrome Trace Event JSON** (engine-side, cheap text) — `X` complete events
with microsecond `ts`/`dur` on two named threads, which Perfetto, speedscope, and chrome://tracing all
ingest unmodified. The editor additionally emits a **Perfetto protobuf** (synthetic TrackEvent) client
side for the denser native format. Both carry `softwareGpu`, `correlated`, the device name, and the
pipeline-stat extras in their metadata, so a downloaded capture is self-documenting.

## The software-GPU caveat

The toolbox usually runs Mesa llvmpipe/lavapipe, where "the GPU" is the CPU. GPU timestamps there are
CPU rasterization time, not representative hardware timing — the `softwareGpu` flag propagates into
the capture and the export, and the UI shows a banner. In-engine queries answer *what* and *which
pass*; the micro-architectural *why* (occupancy, cache/DRAM-bound) still needs a vendor profiler,
which the always-on `VK_EXT_debug_utils` pass labels make immediately usable.

## Code

| What | File | Symbols |
|---|---|---|
| Per-pass GPU + CPU scopes, sub-passes | `render_graph.cppm` | `executeRenderGraph`, `GpuScope`, `CpuScope`, `ScopeRecord` |
| Read-back, calibration, capture state machine | `renderer.cppm` | `readbackGpuTimings`, `calibrateTimestamps`, `startProfileCapture`, `stopProfileCapture` |
| Capture commands | `control_commands_render.cpp` | `profiler.capture-start`, `profiler.capture-stop`, `profiler.capture-status`, `toChromeTrace` |
| Pass labels (always on) | `render_graph.cppm` | `beginPassLabel`, `RgDebugLabels` |
