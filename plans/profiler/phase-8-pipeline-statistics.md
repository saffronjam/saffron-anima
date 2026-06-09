# Phase 8 — pipeline statistics

**Status:** COMPLETED

Finish the lane that is already declared but never decoded. `ProfilerMode::PipelineStats`
(`renderer_types.cppm:561`) exists, `GpuProfiler.pipelineStatsSupported` is set from the
`pipelineStatisticsQuery` device feature (`renderer.cppm:257`), and `setProfilerMode` already accepts the
mode and falls back to `Timestamps` when unsupported (`renderer.cppm:871-873`). What is missing is the
actual `VK_QUERY_TYPE_PIPELINE_STATISTICS` query and its decode — the answer to *why* a pass is slow, not
just *how long* it takes. This is the heaviest capture mode, strictly opt-in, and lands after the UI is
functional so it enriches existing views rather than blocking them.

## The queries

Per the perf-telemetry Phase 1 note, this is the deferred decode:

- Allocate a `vk::QueryPool` of type `ePipelineStatistics` **per frame-in-flight** (parallel to the
  timestamp pools in `allocateProfilerPools`, `renderer.cppm:742-764`), with a `pipelineStatisticsFlags`
  mask covering the useful counters.
- Bracket each instrumented pass body with `vkCmdBeginQuery`/`vkCmdEndQuery`. **Critical difference from
  timestamps:** a statistics query **cannot straddle** the `beginRendering`/`endRendering` boundary the
  way `writeTimestamp2` can — so the begin/end must sit *inside* the dynamic-rendering scope, not around
  it. This means the instrumentation seam in `executeRenderGraph` is slightly different from the timestamp
  seam; account for it in the pass-body wrapper.
- Reset the stats pool alongside the timestamp pool reset in `beginFrame` (`renderer.cppm:1436`), and read
  it back non-blocking in the same `readbackGpuTimings` slot discipline (`renderer.cppm:787-840`) — never
  with `eWaitBit`.

## The decode (useful ratios, not raw counts)

Decode into the ratios that actually guide optimization, attached to each `ProfileSpan.pipelineStats`:

- `FRAGMENT_SHADER_INVOCATIONS` vs rendered pixels → **overdraw**.
- `CLIPPING_PRIMITIVES` vs `CLIPPING_INVOCATIONS` → **culling efficiency** (is back/frustum cull working).
- `INPUT_ASSEMBLY_VERTICES` vs `VERTEX_SHADER_INVOCATIONS` → **vertex reuse** (post-transform cache).
- `COMPUTE_SHADER_INVOCATIONS` → workload size for the GI/lighting/cluster compute passes.

## Wire + UI

- Extend `ProfileSpanDto` (Phase 5) with an optional `pipelineStats` block; it is populated only when the
  capture's `pipelineStats` include-flag is set and `pipelineStatsSupported`. Regenerate the protocol
  (`bun run tools/gen-control-dto/gen.ts`).
- In the Profiler panel (Phase 7), add the stats as **extra columns** on the per-pass table (overdraw,
  cull %, vertex-reuse) shown only when present, and surface them in the flame chart's hover tooltip and
  the exported trace `args` (Phase 9). No new view — this enriches the existing ones.
- Keep it strictly opt-in (default off), gated on `pipelineStatsSupported` with graceful fall-back: when
  unsupported, the include-flag is disabled in the UI and the engine silently drops to timestamps-only.

## Files touched

| What | File | Symbols |
|---|---|---|
| Stats pool alloc + reset | `engine/source/saffron/rendering/renderer.cppm` | `allocateProfilerPools`, `beginFrame` |
| Begin/end inside render scope | `engine/source/saffron/rendering/render_graph.cppm` | `executeRenderGraph` pass-body wrapper |
| Decode → `ProfileSpan.pipelineStats` | `engine/source/saffron/rendering/renderer.cppm` | `readbackGpuTimings` |
| DTO + codegen | `engine/source/saffron/control/control_dto.cppm`, `tools/gen-control-dto/gen.ts` | `ProfileSpanDto.pipelineStats` |
| Table columns + tooltip | `editor/src/components/CaptureTable.tsx`, `CaptureFlame.tsx` | conditional stats columns |

## Validation

- `make engine` + `make prepare-for-commit` clean; `bun run check` clean after regen.
- Headless with `profiler.set-mode pipeline-stats` and the `pipelineStats` include-flag: a capture's scene
  pass span carries non-zero fragment-invocation and vertex counts; the overdraw/cull/reuse ratios compute
  to sane values. Under a device without `pipelineStatisticsQuery`, the mode falls back to timestamps and
  the include-flag is disabled — no crash, no validation warning.
- The validation note: under llvmpipe the *counts* are meaningful (they are pipeline-stage invocation
  counts, not timing), so this mode is actually useful even on software GPU — call that out in the docs.
