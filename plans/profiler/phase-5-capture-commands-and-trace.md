# Phase 5 — capture commands + Chrome-Trace

**Status:** COMPLETED

The wire seam. Phases 2–4 gave the engine a nested, CPU↔GPU-correlated span set per frame; this phase
defines the **span data model**, a **bounded capture state machine**, the two control commands that arm
and retrieve a capture, an engine-side **Chrome Trace Event JSON** serializer, and the codegen + `se`
plumbing. After this, a capture is a thing you can request, receive, and open in chrome://tracing — with
no editor UI yet.

## The span data model

Define a single record the whole stack speaks (stored flat-and-queue-tagged, decoded to a tree by
consumers — never a literal nested tree, so the async-compute future collapses cleanly):

```
ProfileSpan {
  name; lane (CPU | GPU); startNs; endNs;
  parentIndex; depth; passKind;
  optional pipelineStats;   // filled in Phase 8
}
ProfileCapture {
  spans[]; frameMarkers[];  // frame boundaries for multi-frame captures
  metadata { softwareGpu; correlated; deviceName; timestampPeriod; targetFps; mode; filter };
}
```

CPU spans come from Phase 2; GPU spans from Phase 3, projected onto the CPU axis by Phase 4. The
`metadata` block carries the `softwareGpu`/`correlated` honesty flags so a downloaded capture is
self-documenting.

## Bounded capture state machine (engine)

Add a small recorder to the renderer that the commands drive:

- **Modes:** `single` (one frame — **the default**), `frames:N` (bounded; default N≈60, hard-capped so
  the span ring can't OOM), `rolling` (the always-on window — reuse the existing `FrameHistory` ring,
  `renderer_types.cppm:624-645`, don't build a parallel one).
- **States:** `idle → arming → recording → ready`. `capture-start` arms it (forces
  `ProfilerMode::Timestamps` if currently `Off`, remembering the prior mode), allocates/reuses the span
  ring, and begins copying each frame's finalized spans (from `endFrame`, after `readbackGpuTimings`) into
  the capture buffer; once `single`/`frames:N` reaches its frame count it transitions to `ready`.
- **Aggregation:** for `frames:N`/`rolling`, compute p50/p95/p99 over the window by **reusing
  `FrameHistoryStats`** (`renderer.cppm:949`) — no second percentile path.
- **Filter / include-flags:** an optional pass-name prefix (the group selector from the README) and
  include-flags (`gpu` always, `cpu` default on, `pipelineStats` default off, `perDraw` hard-gated/off)
  select which passes get sub-scope depth and which stats are recorded.

The capture buffer is drained on demand by `capture-stop`, **not** pushed on the ~1 Hz metrics lane —
capture is request-scoped.

## Commands (two new, one reused)

Reuse `profiler.set-mode` (`control_commands_render.cpp:272`) for the always-on mode gate — do not
duplicate it. Add two:

- **`profiler.capture-start`** — params `CaptureStartParams { mode; frames; filter?; includeFlags }`;
  result `CaptureStartResult { captureId; ack }`. Arms the recorder.
- **`profiler.capture-stop`** — result `CaptureStopResult`. Returns the finished capture. Follow the
  **screenshot/thumbnail blob precedent** (`control_dto.cppm:589-608`): a small `single`-frame capture
  comes back **inline** as a structured `ProfileCaptureDto` (the editor renders it directly) plus an
  inline Chrome-Trace JSON string (`ThumbnailResult.base64` style); a large `frames:N` capture is
  **written to a file** and returns `{ path, pending }` (`ScreenshotResult` style, `control_dto.cppm:589-593`),
  with the file containing the Chrome-Trace JSON so the path is immediately viewer-openable and the `se`
  CLI can dump it.

Why two, not one or three: a deep `frames:N` capture spans multiple frames while the control plane drains
once per frame on the render thread, so a single blocking "capture-and-return" call doesn't fit the
non-blocking dispatch model — start/stop matches the frame-stepped reality (UE's `Trace.Start`/`Trace.Stop`).
A third `download` command is redundant: `capture-stop` already produces the artifact, exactly as the
screenshot path returns its file in one call.

## Engine-side Chrome Trace Event serializer

Emit the capture as Chrome Trace Event Format (the de-facto interchange consumed unmodified by Perfetto,
speedscope, and chrome://tracing — and the format in the user's reference screenshots). It is trivial
text from C++:

- `pid = "SaffronEngine"`, `tid ∈ { "CPU render thread", "GPU queue" }`; `M` (metadata) events name the
  lanes.
- **`X` (complete) events** with explicit `ts` + `dur` (microseconds) for the properly-nested pass tree —
  prefer `X` over `B`/`E` because Perfetto enforces strict nesting and the pass tree is already nested.
- `C` (counter) events for frame-time / VRAM; `s`/`t`/`f` flow events only for genuine CPU-submit→GPU-execute
  causality (skip in v1 unless cheap).
- Carry `softwareGpu`, `correlated`, and (Phase 8) pipeline-stat extras in per-event `args` so the file is
  self-documenting.

Keep the serializer engine-side so the `se` CLI and the file-download path get it for free; the *Perfetto
protobuf* variant is produced client-side in Phase 9 from the same `ProfileCaptureDto`.

## DTOs, codegen, CLI

Follow the documented DTO-first pipeline:

- Add `CaptureStartParams`, `CaptureStartResult`, `CaptureStopResult`, and `ProfileCaptureDto`/`ProfileSpanDto`
  to `control_dto.cppm` (keep field order = positional CLI order, per the perf-telemetry convention).
- Register `profiler.capture-start` / `profiler.capture-stop` in `control_commands_render.cpp` with
  `registerCommand<Params, Result>` next to `profiler.set-mode` (272) and `pass-timings` (283).
- Add both to the `commands` array in `tools/gen-control-dto/gen.ts` (`gen.ts:94`, `CommandDef { name,
  params, result, summary }`) with contract fixtures, run `bun run tools/gen-control-dto/gen.ts`, and
  commit the four regenerated outputs (serde, `se-types.ts`, OpenRPC, manifest).
- Add an `se` subcommand (`tools/se`) to arm a capture and dump the resulting trace to a file, per the
  "keep the `se` CLI usable" rule.

## Files touched

| What | File | Symbols |
|---|---|---|
| Span model + recorder | `engine/source/saffron/rendering/renderer_types.cppm`, `renderer.cppm` | `ProfileSpan`, `ProfileCapture`, capture state machine |
| Capture drain seam | `engine/source/saffron/rendering/renderer.cppm` | `endFrame` (copy finalized spans), `capture-start`/`stop` accessors |
| Chrome-Trace serializer | `engine/source/saffron/rendering/` (new `.cpp`) or `control/` | `toChromeTrace(const ProfileCapture&)` |
| DTOs + commands | `engine/source/saffron/control/control_dto.cppm`, `control_commands_render.cpp` | `CaptureStartParams`, `CaptureStopResult`, `ProfileCaptureDto`, `profiler.capture-start/stop` |
| Codegen | `tools/gen-control-dto/gen.ts` (+ regenerated serde/types/openrpc/manifest) | `commands[]` |
| CLI | `tools/se` | new capture verb |

## Validation

- `make engine` + `make prepare-for-commit` clean; `tools/check-control-schema/check.ts` validates the
  two new commands live-vs-schema.
- Headless: `profiler.capture-start {mode:single}` then `profiler.capture-stop` returns a
  `ProfileCaptureDto` with both CPU and GPU lanes, a nested span tree, and a Chrome-Trace JSON string that
  parses and validates (well-formed `X`/`M` events, microsecond `ts/dur`, lane metadata).
- A `frames:N` capture returns `{ path, pending }`, the written file is valid Chrome-Trace JSON, and the
  `metadata` carries `softwareGpu`/`correlated` correctly under llvmpipe.
- The full e2e assertion lands in Phase 9; this phase's gate is the schema contract test + a manual
  `se` dump opening in chrome://tracing.
