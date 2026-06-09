+++
title = 'Profiler panel'
weight = 17
+++

# Profiler panel

The Profiler is one of two performance **tools** — Stats and Profiler — opened from the Topbar's
**wrench** menu into the editor's **right sidebar**, where each is a closeable tab (closing the last
one removes the sidebar and the viewport reclaims the width). Stats is the always-on HUD — a streaming
1 Hz read of headline numbers. The Profiler is the opposite surface: you click Capture, the engine
records a bounded window, and the panel shows that one capture broken down by pass across a merged
CPU+GPU timeline. The two are peers, and the Profiler fetches nothing until you ask — opening it starts
no polling lane.

## Capturing

The capture control is a shadcn **ButtonGroup**: the primary **Capture** button on the left joined to a
secondary frame-count **Select** on its right, which together drive the
[capture](../frame-and-render-graph/renderer-profiling/) over the control plane. Idle shows a red dot
and "Capture"; recording shows a stop square, a `n/N` progress counter, and a thin bar, with the header
tinted so an active capture (which has overhead) is unmistakable. The Select picks `1 / 8 / 64 / 256`
frames (default 1, persisted). Arming forces the GPU profiler on for the window and restores the prior
mode on stop — the panel says so plainly.

Progress is driven by polling the non-destructive `profiler.capture-status`; the capture is drained
(`profiler.capture-stop`) only once the engine reports it ready, so the live counter is honest and a
stop never discards a half-recorded window. A software-rasterizer banner appears whenever the
capture's `softwareGpu` flag is set, and an uncorrelated banner whenever the GPU lane could not be
projected onto the CPU clock — the same honesty rules the engine carries.

## The table

The sidebar tab shows the **per-pass GPU table** of the last capture: one static row **per pass name**,
sorted by GPU ms, each graded by the same `passStatus` thresholds the HUD's per-pass bars use, so the
two read as one system. A multi-frame capture records every pass once per frame, so the rows are folded
by name and every number is divided by the frame count to read as **one average frame** (a pass that
runs more than once per frame is tagged `×k/frame`). Fixed column tracks keep the values lined up under
their labels — `GPU ms`, `% of span`, `% of budget`; a pipeline-statistics capture adds an overdraw /
culling / vertex-reuse line per row (its counts summed across occurrences, so the ratios stay
occurrence-weighted). "% of span" is a share, not a sum, because passes overlap on the GPU — so the
right-aligned span (or `avg frame`) total is shown above the columns for reference.

## The flame graph (a main tab)

The **Flame** button in the capture controls opens a separate **main editor tab** ("Flame graph") with
a large two-lane chart (`flame-chart-js`): a GPU lane of nested pass spans and a CPU lane of
render-thread phases, sharing one axis when the capture is correlated (its own zero when not). Spans
are coloured by magnitude. Promoting it to a full tab gives the flame graph the width a timeline wants —
the narrow sidebar keeps the table, which answers *which* pass, while the tab shows *where in the frame
and how consistently*.

## Export

A finished capture's **Download** icon menu writes the trace as **Chrome Trace JSON** or a **Perfetto
protobuf**, and a separate **open-external** icon button opens it **straight into ui.perfetto.dev** — SQL,
search, and flow arrows for free, without the editor hosting a viewer. Perfetto's `postMessage` handoff
can't cross the webview → desktop-browser boundary, so the bridge instead serves the trace bytes from an
ephemeral `127.0.0.1` port (permissive CORS) and opens `ui.perfetto.dev/#!/?url=…` pointing back at it;
Perfetto fetches and loads the trace itself, no download/drag (the loopback bind is reachable from the
host browser because the toolbox shares the host network namespace). The HUD's alarm badge deep-links here
for a per-pass alarm (and to Stats for a frame-wide one), opening the right tool in the sidebar.

## Code

| What | File | Symbols |
|---|---|---|
| Tool panel + table | `ProfilerPanel.tsx`, `CaptureTable.tsx` | `ProfilerPanel` |
| Right sidebar (tools host) | `panels/RightSidebar.tsx`, `app/Layout.tsx` | `RightSidebar`, `rightTools` |
| Flame graph (main tab) | `CaptureFlame.tsx`, `app/App.tsx` | `FlameGraphWorkspace`, `openFlameTab` |
| Capture controls + export | `CaptureControls.tsx` | ButtonGroup capture, Download menu, open-in-Perfetto, Flame button |
| Capture store slice + transforms | `state/store.ts`, `lib/captureTree.ts`, `lib/perfettoExport.ts` | `captureState`, `spansToFlameTree`, `toPerfettoTrace` |
| Perfetto auto-import (loopback trace server) | `src-tauri/src/lib.rs` | `serve_trace`, `start_trace_server`, `open_external` |
| Alarm deep-link | `AlarmBadge.tsx` | `openRightTool("profiler")` for per-pass alarms |
