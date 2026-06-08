# Phase 9 — Perfetto export + docs/e2e

**Status:** NOT STARTED

Close the loop. Add the second export format (Perfetto protobuf) and the "Open in Perfetto" deep-link,
write the docs concept pages, wire the alarm deep-link to the new tab, and add the e2e capture-contract
test. After this, the profiler is a complete, documented, tested feature.

## Perfetto protobuf export + "Open in Perfetto"

Chrome Trace JSON (Phase 5) already opens in Perfetto, speedscope, and chrome://tracing. Add the native
**Perfetto protobuf** path for large captures (Perfetto's SQL/track-analysis is far better on protobuf):

- Produce the protobuf **client-side** from the `ProfileCaptureDto` the editor already holds — the engine
  stays free of a protobuf dependency, and JS/TS has clean tooling for it. Note `@perfetto/trace_processor`
  is **not** a clean npm package (it is the UI's vendored WASM), so the play is *emit a Perfetto-compatible
  file + deep-link to the hosted UI*, not *embed Perfetto's engine*.
- **Download (protobuf)**: a second export option beside the Phase 7 Chrome-JSON download.
- **Open in Perfetto**: `window.open("https://ui.perfetto.dev")` then the documented PING/PONG
  `postMessage` handshake to hand the trace bytes to the opened tab — the user gets SQL, search, and flow
  arrows for free without the editor hosting a viewer.

Carry `softwareGpu`, `correlated`, target FPS, device name, and (Phase 8) pipeline-stat extras into the
trace metadata/`args` in both formats, so a downloaded capture is self-documenting.

## Alarm deep-link to the Profiler tab

The HUD's alarm badge currently deep-links to Stats (`AlarmBadge.tsx:27`, `setBottomTab("stats")`). For a
GPU-pass-over-budget alarm specifically, deep-link to the **Profiler** tab instead (`setBottomTab("profiler")`)
so the user lands where they can capture and drill into the offending pass. Non-GPU alarms keep linking to
Stats. (Optional, low-cost: a per-pass soft-budget alarm — "pass over per-pass budget" — emitted through
the existing `AlarmState`/`drain-alarms` ring; gated as an open question, not required for v1.)

## Docs (per the keep-docs-current rule)

- New concept page under `docs/content/explanations/frame-and-render-graph/` — **renderer profiling**:
  the capture model (CPU+GPU spans, nesting, calibration), the timestamp/`timestampPeriod`/`timestampValidBits`
  + overlap caveats, the `single`/`frames:N`/`rolling` capture modes, the Chrome-Trace **and** Perfetto
  formats, pipeline statistics, and the software-GPU/uncorrelated honesty rules. Lead with the concept
  and *why*; slim `What | File | Symbols` table (`executeRenderGraph`, `readbackGpuTimings`,
  `profiler.capture-start/stop`, `ProfilerPanel`). Add the matching hub `_index.md` row.
- New page under `docs/content/explanations/ui-and-editor/` — the **Profiler panel**: the tab's place
  beside Stats, the capture state machine, the table/flame/icicle views, and the export/Open-in-Perfetto
  flow. Update its hub `_index.md` row. Run the prose through the `humanizer` pass.

## e2e capture-contract test

Add `tests/e2e/profiler.test.ts` (bun over the control plane, typed via `@saffron/protocol`):

- Boot headless, `profiler.capture-start {mode:"single"}`, run a frame, `profiler.capture-stop`.
- Assert the returned `ProfileCaptureDto` has both a CPU lane and a GPU lane, a non-empty nested span tree
  with valid depths/parents, and that the Chrome-Trace JSON parses into well-formed `X`/`M` events with
  microsecond `ts/dur`.
- Assert `metadata.softwareGpu`/`correlated` are present and the magnitude assertions are **relaxed when
  `softwareGpu`** (the same magnitude-tolerance discipline as the perf-smoke test), keeping the validation
  log clean.
- Optionally assert a `frames:N` capture returns `{ path, pending }` and the written file is valid.

## Files touched

| What | File | Symbols |
|---|---|---|
| Perfetto protobuf + deep-link | `editor/src/lib/perfettoExport.ts` (new), `editor/src/components/CaptureControls.tsx` | `toPerfettoTrace`, Open-in-Perfetto |
| Alarm deep-link | `editor/src/components/AlarmBadge.tsx` | `setBottomTab("profiler")` for GPU-pass alarms |
| Docs | `docs/content/explanations/frame-and-render-graph/`, `docs/content/explanations/ui-and-editor/` (+ hub `_index.md` rows) | — |
| e2e | `tests/e2e/profiler.test.ts` (new) | capture-contract assertions |

## Validation

- `make e2e` green (the new `profiler.test.ts` and the existing suite); `make engine` +
  `make prepare-for-commit`; `bun run check`/`lint`/`format` clean.
- `make run`: Download (Chrome JSON) and Download (Perfetto) both produce files that open in their
  respective viewers; "Open in Perfetto" hands the capture to ui.perfetto.dev via the postMessage handshake.
- A GPU-pass alarm's badge deep-links to the Profiler tab; a non-GPU alarm still links to Stats.
- Both new docs pages build (`hugo`), have `title == # H1`, and carry the `What | File | Symbols` table;
  the hub `_index.md` rows are added.
- Mark the plan **COMPLETED** in `README.md` and each phase file when its gate passes.
