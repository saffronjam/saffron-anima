# Phase 2 — CPU phase + per-pass scopes

**Status:** NOT STARTED

Turn the two EMA scalars the engine has today — `cpuFrameMs` and `cpuWaitMs`
(`renderer_types.cppm:589-590`) — into a real **per-phase and per-pass CPU timeline**. Phase 1 of
perf-telemetry deliberately kept CPU timing as smoothed headline numbers; the profiler needs the
opposite: precise, un-smoothed `steady_clock` spans, captured per frame, that line up pass-for-pass
with the GPU timestamps. This is the prerequisite for the "CPU on the left, GPU on the right of the
same bar" model (Godot's Visual Profiler) and for the merged timeline Phase 4 finishes.

## The marker primitive

Adopt the integer-ID + RAII-scope pattern (Unity's `ProfilerMarker`, **not** a string per sample) so
the hot path allocates nothing and `Off` pays zero cost — consistent with the engine's no-exceptions /
RAII style and the existing `ProfilerMode` gate:

- A small registry maps a compile-time-ish string id (e.g. `"executeRenderGraph"`, `"onUpdate"`) to a
  stable integer index once; the per-frame record stores `{ markerId, startNs, endNs, depth, parent }`,
  never the string.
- An RAII `CpuScope` grabs `steady_clock::now()` on construct and on destruct, pushing the span onto the
  current frame's CPU-span buffer. Nesting comes from a thread-local depth/parent counter (the render
  loop is single-threaded today; document that assumption so async recording later is a conscious change).
- Gate behind `renderer.profiler.mode != ProfilerMode::Off` (the same guard `beginFrame`/`endFrame`
  already use at `renderer.cppm:1337,1436,2600`) so the present-only host stays free.

## Where the scopes go

**Frame-lifecycle phases** — wrap the seams the architecture already names (AGENTS.md lifecycle), around
the existing CPU accumulation in `endFrame` (`renderer.cppm` end-of-frame block near the `FrameSample`
push, ~`2796`):

- `onUpdate`, command recording / submit-lambda replay, `beginFrameGraph` (cull + scene-pass build),
  `executeRenderGraph`, and present/submit. These become the top-level CPU lane.
- Keep the existing `cpuFrameMs`/`cpuWaitMs` EMA numbers for the HUD — they stay on the display path;
  the raw spans are a *separate* recorded series (smoothing belongs on display, not on the capture).

**Per-pass CPU scopes** — wrap each `pass.execute` invocation inside `executeRenderGraph`
(`render_graph.cppm:297-419`), at the *same* seam as the GPU timestamp writes (`309/406`). The result:
each pass carries both a CPU span (time to *record/dispatch* the pass on the CPU) and a GPU span (time
to *execute* it on the device) under one name — exactly the two-sided bar the UI wants.

## Storage

Mirror the GPU timestamp ring's slot discipline:

- A per-frame-in-flight CPU-span buffer indexed by `renderer.frame.index` (the same slot the timestamp
  pools use), reset/armed in `beginFrame` (`renderer.cppm:1436`) and finalized in `endFrame`.
- The buffer is plain host memory (no GPU sync), so it is readable immediately at end of frame — no
  N-frame deferral like the GPU read-back. Keep CPU and GPU spans in *separate* lanes in the record;
  Phase 4 maps them onto one axis, Phase 5 serializes both.
- Size the buffer to the same nesting-aware cap Phase 3 introduces for GPU scopes, so CPU and GPU depth
  stay symmetric.

## Files touched

| What | File | Symbols |
|---|---|---|
| `CpuScope` RAII + marker registry | `engine/source/saffron/rendering/renderer_types.cppm` (+ a small `.cpp`) | `CpuScope`, marker registry, per-frame CPU-span buffer |
| Phase scopes around lifecycle seams | `engine/source/saffron/rendering/renderer.cppm` | `beginFrame`, `endFrame`, the lifecycle call sites |
| Per-pass CPU scope | `engine/source/saffron/rendering/render_graph.cppm` | `executeRenderGraph` |
| Gate + ring reset | `engine/source/saffron/rendering/renderer.cppm` | `beginFrame` (arm), profiler-mode guard |

## Validation

- `make engine` + `make prepare-for-commit` clean.
- A headless run with `profiler.set-mode timestamps`: a temporary log (or the existing e2e perf gate)
  confirms the per-frame CPU-span buffer populates with the expected phase names and one CPU span per
  executed pass, with sane (positive, monotonic) `startNs/endNs` and correct nesting depth.
- With `profiler.set-mode off`, the scopes compile to a cheap branch and record nothing (assert the
  buffer is empty / the cost is negligible).
- No new wire surface yet — these spans become visible over the control plane in Phase 5.
