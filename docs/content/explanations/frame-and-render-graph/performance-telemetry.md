+++
title = 'Performance telemetry'
weight = 7
+++

# Performance telemetry

A single frame-time number cannot say *why* a frame was slow. The CPU records frame N+1 while the
GPU still executes frame N, so the two run on independent timelines: a slow frame can be CPU-bound
(the GPU sat idle waiting for work) or GPU-bound (the CPU blocked on a fence). And even a correct
total hides *which pass* regressed. The profiler answers both — it measures CPU and GPU time
separately, times every render-graph pass on the GPU, counts the per-frame throughput, and reads the
VMA memory budget.

All of it is off by default. The present-only host pays nothing until a client turns the profiler on
over the control plane, so the baseline cost of running the engine is unchanged.

## The CPU/GPU split

Each frame the renderer brackets its work between two `steady_clock` stamps taken in `beginFrame` and
`endFrame`, and separately accumulates the time spent *blocked* — the fence waits at the top of
`beginFrame` (and the per-image wait on the present path). Subtracting the waits from the window
gives two numbers:

- **`cpuFrameMs`** — render-thread busy time, the work the CPU actually did this frame.
- **`cpuWaitMs`** — time the CPU sat blocked on the GPU. High wait time means GPU-bound; near-zero
  wait with `cpuFrameMs` close to the frame budget means CPU-bound.

Both are EMA-smoothed for display, the same way the existing frame-to-frame `frameMs` is. The
window covers the renderer's own work (command recording, graph build, graph execute, submit); the
application's `onUpdate` runs before `beginFrame` and is outside it.

## Per-pass GPU timing

The render graph is the natural place to time passes: `executeRenderGraph` already walks the passes
in order and brackets each one's body. When the profiler is armed it passes an `RgTimestamps` handle
into the graph, and the graph writes a timestamp before each pass's barriers and another after its
body closes — the same "no pass writes it by hand" principle as barrier derivation. It records the
pass name alongside each slot.

The mechanics are the standard Vulkan timestamp-query rules:

- One `vk::QueryPool` of type `eTimestamp` **per frame-in-flight**, sized `2 × MaxProfiledPasses`.
- The pool is reset (`cmd.resetQueryPool`) at the start of recording — queries are uninitialized
  until reset, and reading an unreset pool risks device loss.
- Read-back targets the **previous** use of this slot's pool. When `beginFrame` waits on the frame
  fence, that slot's GPU work (from `MaxFramesInFlight` frames ago) is complete, so
  `getQueryPoolResults` with `eWithAvailability` never blocks. The loop never uses `eWaitBit`.
- Raw ticks convert to nanoseconds with `timestampPeriod`, after masking each value to the graphics
  queue's `timestampValidBits`.

> [!NOTE]
> Per-pass numbers are *relative*. Adjacent or async passes overlap on the GPU, so the parts do not
> cleanly sum to the whole — the reported total is the wall-clock span from the first pass's begin to
> the last pass's end, not a sum. An underclocked or idle GPU also inflates timings; cross-check
> against the CPU wall-clock.

## Throughput counters and the VRAM budget

Alongside the existing draw-call / batch / instance counts, the profiler accumulates per frame:
triangles submitted, descriptor-set binds in the scene pass, command buffers, queue submits, and
**pipelines compiled this frame**. That last one is the signature of a PSO-compile hitch — non-zero
on a steady-state frame means a shader was built mid-frame. The descriptor-bind count staying flat as
batches grow is the live proof that the bindless + submesh-major instancing path keeps binds
constant, not `O(draws)`.

The VRAM figure comes from `VK_EXT_memory_budget`: `vmaGetHeapBudgets` (cheap, unlike
`vmaCalculateStatistics`) reports per-heap usage and budget each frame, summed across the
device-local heaps. Usage approaching budget predicts eviction thrashing.

## Frame history & quality metrics

A single number describes one frame; smoothness lives in the *distribution* of many. The editor
polls at ~6 Hz but the engine renders at the display rate, and percentiles and stutter live in the
per-frame data — so the **engine** owns a fixed-size ring of the last `FrameHistoryCapacity` frames
(≈8–17 s at 60–120 Hz), pushed once per frame at `endFrame`. It records the *raw* per-frame numbers,
not the EMA-smoothed display values: smoothing belongs on the display path, never on the recorded
series, or the distribution lies. The frame time it tracks is `cpuMs + cpuWaitMs` — the render-thread
wall clock, work plus the fence wait that absorbs GPU-bound stalls. The ring is always recorded; it
does not need the profiler enabled.

The summary is computed on demand (not every frame) over the ring:

- **Percentiles / lows.** p50 (the median — outlier-resistant, the primary number), p95, p99, p99.9.
  A high *percentile frame time* is a *low* frame rate: the p99 frame time is the 1%-low FPS. Average
  FPS is deliberately absent — it oversamples fast frames and hides exactly the hitches that hurt.
- **Consistency.** The p50→p99 gap (small = smooth), plus stddev and the max frame time.
- **Stutter.** A frame is flagged a stutter when its time exceeds **both** `2 × the previous-3-frame
  average` **and** an absolute floor of `2 × budget`. The relative rule catches hitches at any frame
  rate; the floor rejects noise at trivially fast rates. A per-session count and the last-stutter
  timestamp are kept.

### The shared budget/threshold config

One `PerfConfig` is the single source of truth for green/amber/red, surfaced over the wire so the
engine, the editor HUD, and the e2e tests read identical numbers — thresholds are never hardcoded in
the editor. `budget = 1000 / targetFps`.

| Knob | Default | Meaning |
|---|---|---|
| `targetFps` | 60 | budget = `1000 / targetFps` (16.67 ms); 30/60/90/120 + custom |
| green | `< 0.8 × budget` **and** `< 1.5 × median` | within budget with headroom, consistent |
| amber | `≥ 0.8 × budget`, or `1.5–2 × median` | near budget / occasional spike |
| red | `> budget`, or `> 2 × median`, or frozen | over budget / true stutter |
| frozen | `> 250 ms` | a hard hitch → always red |
| VRAM warn / crit | 80% / 95% of budget | from the Phase-1 `usage / budget` |

`get-perf-config` / `set-perf-config` read and update it; a VR target swaps the budget to 11.11 ms
(90 Hz) by setting `targetFps` and tightening the amber multiplier (presence breaks before smoothness
does). `frame-history` returns the percentile summary plus, on request, the recent raw ring so the
editor can draw the live graph without keeping its own high-rate buffer.

## Modes and capability

`profiler.set-mode {off | timestamps | pipeline-stats}` selects the depth. `off` is the default and
the baseline cost. `timestamps` enables per-pass GPU timing, the throughput counters, and the VMA
budget read. `pipeline-stats` is reserved for the deepest per-pass statistics capture (overdraw,
clipping, shader invocations) and is gated on the `pipelineStatisticsQuery` device feature.

A requested mode the device cannot support degrades gracefully: no timestamp support falls back to
`off`; no pipeline-statistics feature falls back to `timestamps`. The set-mode result reports
`timestampsSupported`, `pipelineStatsSupported`, and `softwareGpu` so the editor can disable controls
the device cannot drive.

> [!WARNING]
> On a software rasterizer (Mesa **llvmpipe / lavapipe**, common in headless/CI runs) "the GPU" is
> the CPU. GPU timestamps, occupancy, and saturation are CPU rasterization time and are **not
> representative of real hardware**. The `softwareGpu` flag is set on every payload so downstream
> annotates or suppresses GPU-timing magnitudes. In-engine queries say *what* is slow; the
> micro-architectural *why* still needs a vendor profiler (Radeon GPU Profiler / Nsight).

## Driving it

The data is on the control plane, so it is scriptable from the `sa` CLI:

```sh
se profiler.set-mode timestamps   # mode=timestamps  timestamps=yes  pipeline-stats=no
sa render-stats                   # cpu=…ms  gpu=…ms  wait=…ms  fps=…  draws=…  tris=…  binds=…
se pass-timings                   # per-pass GPU ms + the total span
se frame-history                  # p50/p95/p99/p99.9 + max, stddev, stutters, budget
sa get-perf-config                # targetFps + derived budget + green/amber/red thresholds
sa set-perf-config --targetFps 90 # retarget the budget (VR-style)
se profiler.set-mode off          # back to baseline
```

`render-stats` carries the headline numbers and counters on the hot path; the heavier per-pass array
and the frame-time ring live behind the separate `pass-timings` and `frame-history` commands so the
common poll stays cheap. `frame-history` works whether or not the profiler is on — the history ring
is always recorded.

## In the code

| What | File | Symbols |
|---|---|---|
| Profiler state + extended stats | `renderer_types.cppm` | `GpuProfiler`, `RenderStats`, `PassTiming`, `ProfilerMode` |
| Per-pass timestamp writes | `render_graph.cppm` | `RgTimestamps`, `executeRenderGraph` |
| CPU timing, pool lifecycle, read-back, VMA budget | `renderer.cppm` | `beginFrame`, `endFrame`, `readbackGpuTimings`, `readVramBudget`, `setProfilerMode` |
| Throughput counters | `renderer_drawlist.cpp`, `renderer_pipelines.cpp` | `submitDrawList`, `recordSceneDrawList`, `requestMeshPipeline` |
| Device capabilities + software-GPU tag | `renderer.cppm` | `newRenderer` (timestamp limits, `VK_EXT_memory_budget`) |
| Frame-time ring + percentiles + stutter | `renderer_types.cppm`, `renderer.cppm` | `FrameSample`, `PerfConfig`, `endFrame`, `frameHistoryStats` |
| Wire surface | `control_dto.cppm`, `control_commands_render.cpp` | `RenderStatsDto`, `RenderPassTimingsDto`, `FrameHistoryDto`, `PerfConfigDto`, `profiler.set-mode`, `pass-timings`, `frame-history`, `get/set-perf-config` |

## Related

- [Render graph](../render-graph-overview/) — the build-execute model the timestamps ride on
- [Adding passes](../who-can-add-passes/) — where the passes the profiler times come from
- [Control plane](../../tooling-and-control/control-plane-architecture/) — the JSON-over-socket wire the telemetry travels on
