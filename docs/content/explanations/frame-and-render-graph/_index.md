+++
title = 'Frame & render graph'
weight = 4
bookCollapseSection = true
+++

# Frame & render graph

A render graph describes one frame of rendering as a set of passes and the resources they read and
write. Each pass declares its usage of a resource; the graph derives the barriers and layout
transitions and records the pass body. No pass writes a pipeline barrier by hand. Application
layers add their own passes to the cull → scene → UI frame.

## Pages

| Page | Covers | Code |
|---|---|---|
| [Render graph](render-graph-overview/) | why declared usage, the build-execute-per-frame model | `render_graph.cppm` |
| [Passes](passes-and-attachments/) | `RgPass`, MRT `colors`, depth, load/store/clear, the execute closure | `render_graph.cppm` |
| [Barrier derivation](usage-and-barrier-derivation/) | `RgUsage`, `usageInfo`, `applyAccess`, hazard + layout logic | `render_graph.cppm` |
| [Cross-frame layouts](cross-frame-layouts/) | `externalLayout` write-back, imported images, seeded source scope | `render_graph.cppm` |
| [Adding passes](who-can-add-passes/) | engine passes in `beginFrameGraph` vs. layer `onRenderGraph` | `renderer.cppm` |
| [Limits](limits-and-seams/) | single queue, no transient aliasing, no async compute, the seams left | `render_graph.cppm` |
| [Performance telemetry](performance-telemetry/) | CPU/GPU split, per-pass GPU timestamps, throughput counters, VRAM budget, the profiler mode gate | `renderer.cppm`, `render_graph.cppm` |
| [Performance alarms](performance-alarms/) | EMA + hysteresis + debounce, MAD-spike / burn-rate detectors, severity, the non-blocking `drain-alarms` seq cursor | `renderer.cppm`, `renderer_types.cppm` |
| [Renderer profiling](renderer-profiling/) | the capture model (merged CPU+GPU spans, nesting, calibration), timestamp caveats, capture modes, Chrome-Trace + Perfetto export, pipeline statistics, software-GPU honesty | `render_graph.cppm`, `renderer.cppm` |
