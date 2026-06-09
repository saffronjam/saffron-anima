# renderer profiler plan

This plan adds an **on-demand renderer profiler** on top of the live perf-telemetry HUD: a
record/stop **capture** that measures GPU *and* CPU time for every render-graph pass, correlates the
two onto one timeline, nests sub-pass scopes into a tree, and serializes the result to standard trace
formats — visualized in the editor as a **flame chart / timeline / sortable table** under a dedicated
**Profiler** dock tab, and downloadable as **Chrome Trace Event JSON** or a **Perfetto protobuf**
trace that opens unmodified in Perfetto / speedscope / chrome://tracing.

The live HUD (`plans/perf-telemetry/`, COMPLETED) answers *"is it slow, right now?"*. The profiler
answers *"which pass, exactly, this frame — and how much of it is CPU vs GPU?"*. They are different
surfaces and stay separate: the HUD streams at ~1 Hz; capture is explicit, request-scoped, and gated
behind a button because it has overhead.

## Why

The engine already owns the load-bearing half of a profiler — and it is the *only* half built:

- **Per-pass GPU timestamps exist.** `executeRenderGraph` brackets every pass body with
  `writeTimestamp2(eTopOfPipe …)` / `writeTimestamp2(eBottomOfPipe …)` (`render_graph.cppm:309,406`)
  into a per-frame-in-flight `vk::QueryPool` (`RgTimestamps`, `render_graph.cppm:128-133`).
- **Non-blocking read-back exists.** `readbackGpuTimings` (`renderer.cppm:787-840`) reads the slot
  with `e64 | eWithAvailability` (never `WAIT`), masks by `timestampValidBits`, scales by
  `timestampPeriod`, and reports a **span-based** total (`spanBegin..spanEnd`, not a sum).
- **The mode gate exists.** `ProfilerMode {Off, Timestamps, PipelineStats}` (`renderer_types.cppm:557`)
  with `setProfilerMode` capability fall-back (`renderer.cppm:863`), pools armed in `beginFrame`
  (`renderer.cppm:1436`) and read in `endFrame`'s lead-in (`renderer.cppm:1337`).

What is missing is everything that turns *per-pass numbers* into *a profile you can read*:

- **No depth.** Timings are a flat `std::vector<PassTiming{name; gpuMs}>` (`renderer_types.cppm:567`).
  No nesting (depth-prepass vs opaque vs transparent inside the scene pass), no CPU lane at all
  (only two EMA scalars, `cpuFrameMs`/`cpuWaitMs`), and no CPU↔GPU correlation onto one axis.
- **No capture path.** `pass-timings` is a live poll of *last frame*; there is no way to record a
  bounded window, snapshot one frame, or hand back a downloadable trace.
- **No profiler UI.** Per-pass bars live inside the monolithic `RenderStatsPanel` under the **Stats**
  tab; there is no flame chart, no timeline, and no capture controls.

## What "done" looks like

- Every render-graph pass carries a **`VK_EXT_debug_utils` label** (free RenderDoc/Nsight readability)
  and, while capturing, a **nested GPU scope** (`{name, parentIndex, depth}` tree) plus a **CPU scope**
  for both the frame-lifecycle phases and each pass body.
- GPU spans are mapped onto the CPU `steady_clock` via **`VK_EXT_calibrated_timestamps`**, so a capture
  is **one merged CPU+GPU timeline**, not two disconnected axes.
- A **bounded capture** (`single` / `frames:N` / `rolling`, **default single frame**) armed by
  `profiler.capture-start` and returned by `profiler.capture-stop` — small captures inline, large ones
  as a written file with `{path, pending}`, mirroring the `ScreenshotResult` vs `ThumbnailResult`
  precedent (`control_dto.cppm:589-608`).
- An editor **Profiler dock tab** (peer to Stats, not a rewrite of it): capture controls (a Start/Stop
  toggle + window-length selector + Download), landing on a **sortable per-pass table**, with a
  **time-ordered flame chart / two-lane timeline** and an **aggregate icicle** as sibling views, plus
  **Open in Perfetto**.
- Export to **both** Chrome Trace Event JSON and **Perfetto protobuf**; optional **pipeline statistics**
  (overdraw / culling / vertex-reuse / compute invocations) decoded into per-pass columns.
- Scriptable from `se` and covered by an e2e capture-contract test.

## The caveat that shapes the whole plan: software GPU

Same as perf-telemetry: the toolbox usually runs **Mesa llvmpipe / lavapipe**, where "the GPU" is the
CPU. GPU timestamps there are **CPU rasterization time, not representative hardware timing**. The
existing `stats.softwareGpu` flag must propagate into the capture's metadata and the exported trace's
`args`, the Profiler panel must show the same banner the HUD does (`RenderStatsPanel.tsx:327`), and the
e2e capture test must stay magnitude-tolerant when `softwareGpu` is set. In-engine queries answer
*what/which pass*; the micro-architectural *why* (occupancy, cache/DRAM-bound) still needs a vendor
profiler — which the Phase 1 debug-utils labels make immediately usable.

## Design decisions (locked)

- **UI placement: a dedicated `profiler` dock tab**, peer to `inspector | environment | stats`. Stats
  stays intact as the always-on HUD; the profiler is a separate, self-contained panel. (We deliberately
  did *not* fold the HUD into a unified "Performance" hub — the two are kept side by side.)
- **Depth: full.** v1 ships per-pass + nested sub-scopes + CPU/GPU calibrated timeline + flame
  chart/table/icicle + Chrome-Trace **and** Perfetto-protobuf export + pipeline statistics.
- **Interchange: Chrome Trace Event JSON *and* Perfetto protobuf.** Chrome JSON is emitted engine-side
  (cheap text; gives the `se` CLI and the file-download path parity with the screenshot precedent);
  the Perfetto protobuf and the "Open in Perfetto" deep-link are produced client-side from the
  structured capture DTO the editor already holds.
- **Capture default: a single frame** (the "what is this frame made of" snapshot), with
  **1 / 8 / 64 / 256-frame** presets; `rolling` reuses the existing `FrameHistory` ring. No unbounded
  multi-second capture in v1 (documented upgrade path only).
- **Scope is a *view/filter* over a full frame**, never partial graph execution (that would break
  barrier derivation). Pass names gain group prefixes (`scene.opaque`, `lighting.cluster`,
  `post.tonemap`) so a name-prefix filter doubles as a group selector.

## Reuse vs build (against perf-telemetry, all COMPLETED)

**Extend, never duplicate:** the GPU timestamp mechanism (`RgTimestamps`, `executeRenderGraph` writes,
`readbackGpuTimings`, the per-frame-in-flight pools, `timestampMask`/`timestampPeriod` conversion, the
span-based total), the `ProfilerMode` gate + `setProfilerMode` capability checks, `FrameSample` +
`FrameHistoryStats` percentiles (any windowed aggregation calls these), the `AlarmState`/`drain-alarms`
ring (for any profiler-emitted "pass over budget" events), `softwareGpu` tagging, and the editor's
`perfThresholds.ts` grading + `frameSeries.ts` ring + the alarm pipeline.

**Build new:** CPU phase/per-pass RAII scopes; `VK_EXT_calibrated_timestamps` correlation; the nested
scope stack (flat→tree); the bounded capture state machine + `capture-start`/`capture-stop` commands +
Chrome-Trace serializer; the editor capture store slice + the `profiler` tab + the flame-chart/table/
icicle views + the Perfetto export; the pipeline-statistics decode.

## Status convention

Each phase file carries a `**Status:**` line (`NOT STARTED` / `IN PROGRESS` / `COMPLETED`). Mark a phase
`COMPLETED` only when its work is validation-clean (`make engine` + `make prepare-for-commit`; editor
phases also `bun run check`; capture phases also `make e2e`); delete a phase file only *after* it is
`COMPLETED` and merged. Per the root `AGENTS.md` "keep current" rule, each phase ends by updating its
`docs/content/` concept page and its test in the same change — not deferred to the end.

## Phases

| Phase | What | Status |
|---|---|---|
| [1 — debug-utils pass labels](phase-1-debug-utils-labels.md) | `vkCmdBeginDebugUtilsLabelEXT` around every pass body, mode-independent; instant RenderDoc/Nsight readability | COMPLETED |
| [2 — CPU phase + per-pass scopes](phase-2-cpu-scopes.md) | integer-ID RAII `steady_clock` markers around the frame-lifecycle phases and each pass body; a per-frame CPU-span ring | COMPLETED |
| [3 — nested GPU scope stack](phase-3-nested-scope-stack.md) | flat per-pass list → a `{name, parentIndex, depth}` tree; bigger pool cap; sub-pass scopes inside the scene pass | COMPLETED |
| [4 — CPU↔GPU correlation](phase-4-calibrated-timestamps.md) | `VK_EXT_calibrated_timestamps`: map GPU spans onto the CPU clock; one merged timeline; graceful GPU-only fallback | COMPLETED |
| [5 — capture commands + Chrome-Trace](phase-5-capture-commands-and-trace.md) | the `ProfileSpan`/`ProfileCaptureDto` model, the bounded capture state machine, `capture-start`/`capture-stop`, the engine Chrome-Trace serializer, codegen + `se` dump | COMPLETED |
| [6 — editor store + Profiler tab](phase-6-editor-store-and-tab.md) | the `captureState`/`capture` Zustand slice, the typed client wrappers, the new `profiler` `BottomTab` + an empty panel shell | COMPLETED |
| [7 — Profiler panel & views](phase-7-profiler-panel-views.md) | capture controls (Start/Stop + window selector + Download), the sortable table (default), the flame-chart/timeline, the aggregate icicle, cross-highlight | COMPLETED |
| [8 — pipeline statistics](phase-8-pipeline-statistics.md) | decode the wired-but-unimplemented `PipelineStats` mode into per-pass overdraw / culling / vertex-reuse / compute-invocation columns | COMPLETED |
| [9 — Perfetto export + docs/e2e](phase-9-perfetto-export-and-docs.md) | client-side Perfetto-protobuf export + Open-in-Perfetto, the docs concept pages, and the e2e capture-contract test | COMPLETED |

## Sequencing

Strictly dependency-ordered, lowest-risk first. **1** is free and independent (markers, no timing
path). **2–4** deepen the *engine's* captured data with no wire/UI surface — each is internally
shippable and gate-clean on its own. **5** is the wire seam everything editor-side reads. **6** lands
the tab + store with no visuals; **7** is the visible profiler. **8** (pipeline stats) and **9**
(Perfetto export + docs + e2e) are the close-out enrichments. A reasonable first *visible* slice is
**1 + 5(single-frame only) + 6 + the table half of 7** — a working capture→table round-trip — before
building the nested/CPU/calibrated depth and the flame chart. The live HUD path
(`pass-timings`/`frame-history`/`drain-alarms`) stays untouched throughout.
