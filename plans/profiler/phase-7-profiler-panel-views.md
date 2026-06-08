# Phase 7 — Profiler panel & views

**Status:** NOT STARTED

The visible profiler. Fill the `ProfilerPanel` shell from Phase 6 with the **capture controls** and the
**three views** — a sortable per-pass table (the default landing), a time-ordered flame chart / two-lane
timeline, and an aggregate icicle — wired to the capture store slice. This is the "click Start → inspect
a hot pass" surface, modeled on UE's GPU Visualizer, Unity's Profiler, and Godot's Visual Profiler, but
living in its own dock tab.

## Capture controls (panel header)

A header row mirroring where `MetricsRefreshControl` sits in the HUD (`RenderStatsPanel.tsx:412`):

- **Start/Stop toggle (one button, two states).** Idle: a filled red dot + **"Capture"**. Recording: a
  stop square + **"Stop (n/N)"** with a thin progress bar. Driven by the `captureState` machine from
  Phase 6 (`idle → arming → recording → ready`). One button, not two — matches UE's single
  `Trace.Start`/`Trace.Stop`.
- **Window-length selector** beside it: `1 / 8 / 64 / 256` frames, default **1** (the single-frame
  snapshot). Persisted via `captureWindowFrames`.
- **Download button**, disabled until a capture exists (`capture !== null`). Exports the current capture
  as Chrome Trace JSON (the engine already produced it in Phase 5; the editor saves the blob). The
  Perfetto-protobuf export and "Open in Perfetto" land in Phase 9.

**Arming forces the profiler on:** the arm path calls `setProfilerMode("timestamps")` via the same
plumbing `RenderStatsPanel.tsx:282-287` uses, and restores the prior mode on `idle`/`ready` so a capture
doesn't silently leave the baseline host instrumented. Surface this honestly ("Capturing enables the GPU
profiler"). The **software-GPU banner** (`RenderStatsPanel.tsx:327`) appears above the views whenever
`capture.metadata.softwareGpu`, and an **uncorrelated** banner whenever `!capture.metadata.correlated`
(from Phase 4).

## Visual feedback (capture state machine)

| State | Button | View |
|---|---|---|
| `idle` | red dot + "Capture" | last capture, or empty-state prompt |
| `arming` | spinner + "Arming…" | engine flips to timestamps mode, allocates the ring |
| `recording` | stop square + "Stop (n/N)" + progress bar | live counter; optional faint frame-ms spark |
| `ready` | red dot + "Capture" (re-armable), Download enabled | auto-switch to the table, sorted by GPU ms |

Recording must be unmistakable (capture has overhead) — reuse the editor's play-mode tint instinct
(`Layout.tsx` rings the dock in play mode) with a subtle header tint while recording.

## View 1 — sortable per-pass table (default)

The cheapest, most actionable view and the landing view after every capture. Columns: **Pass · GPU ms ·
% of span · % of budget**, sorted by GPU ms descending, each row colored by `passStatus()` from
`perfThresholds.ts` (the same grading the HUD's per-pass bars use, so the two tabs read as one system).
Reuse the existing `Bar` cell renderer (`RenderStatsPanel.tsx:102`). Keep the **"numbers are relative —
passes overlap on the GPU"** note (`RenderStatsPanel.tsx:429`) and label the column **"% of span"**, since
the total is `spanBegin..spanEnd`, not a sum. For nested captures the table is indentable by `depth`.

## View 2 — time-ordered flame chart / two-lane timeline

The view users picture when they hear "profiler" (the reference screenshots). x = time; two lanes sharing
one axis: a **GPU lane** (the nested pass spans) and a **CPU lane** (the render-thread phases), aligned by
the Phase 4 calibration. Use **`flame-chart-js`** (Canvas, multi-lane, React-wrappable; node shape
`{name, start, duration, children}` is exactly the `captureTree.ts` output) — the chosen library over
`d3-flame-graph` (SVG, aggregate-oriented) and `react-flame-graph`. Color spans by magnitude via
`perfThresholds`. When `!correlated`, render the GPU lane on its own zeroed axis and label it so the
disconnect is honest, not faked.

## View 3 — aggregate icicle (over the captured window)

For `frames:N`/`rolling` captures: "where does time go on average across the window." A Brendan-Gregg-style
aggregate **in icicle layout** (root on top — the modern default), collapsing spans by pass-path weighted
by summed GPU ms. Lower priority than the table and flame chart (the engine has exact instrumented
durations, so per-frame structure is the richer signal), but it is the trend complement for multi-frame
captures.

## Cross-highlight

Clicking a hot table row sets `selectedPass` (Phase 6 slice); the flame/timeline and icicle tabs
cross-highlight that pass's span(s), and the selection persists across the three sub-tabs. This is the
"inspect a hot pass" moment — the table says *which*, the flame chart shows *where in the frame and how
consistently*.

## Files touched

| What | File | Symbols |
|---|---|---|
| Capture controls + state machine UI | `editor/src/panels/ProfilerPanel.tsx`, `editor/src/components/CaptureControls.tsx` (new) | `ProfilerPanel`, `CaptureControls` |
| Per-pass table | `editor/src/components/CaptureTable.tsx` (new) | reuses `Bar`, `passStatus` |
| Flame chart / timeline | `editor/src/components/CaptureFlame.tsx` (new) | `flame-chart-js` wrapper |
| Aggregate icicle | `editor/src/components/CaptureIcicle.tsx` (new) | — |
| Dep | `editor/package.json` | `flame-chart-js` |
| Shared grading | `editor/src/lib/perfThresholds.ts` | `passStatus`, `GRAPH_COLORS` (reused as-is) |

## Validation

- `bun run check` + `bun run lint`/`format` clean; `flame-chart-js` added and building.
- `make run`: the full flow works — click Capture (1 frame) → table populates sorted by GPU ms → click the
  hot row → it cross-highlights in the flame chart → switch to icicle, selection persists.
- The software-GPU and uncorrelated banners appear under llvmpipe; the "% of span" label and relative-note
  are present.
- Capture is request-scoped (no new polling lane); switching away from the Profiler tab does not start a
  metrics fetch.
