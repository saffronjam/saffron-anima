# Phase 3 — nested GPU scope stack

**Status:** COMPLETED

Generalize the **flat** per-pass GPU timing list into a **tree**. Today the timestamp path records a
`std::vector<std::string>` of pass names and a parallel `std::vector<PassTiming{name; gpuMs}>`
(`renderer_types.cppm:567-571,608`) — one level, one entry per top-level pass. A real profiler needs
nesting: inside the scene pass, depth-prepass vs opaque vs transparent vs the clustered-light compute
dispatch should be *children* of the pass, not new siblings. This is the Tracy `TracyVkZone` model —
a stack of begin/end timestamp pairs whose nesting *is* the hierarchy.

## From names to scope records

Evolve the recorded data (kept flat-and-tagged, decoded into a tree at read-back — never stored as a
literal tree, because the async-compute future makes a single nested wall-clock tree ambiguous):

- Replace `RgTimestamps.names` (`render_graph.cppm:128-133`) and `GpuProfiler.recordedNames`
  (`renderer_types.cppm:607`) — a `std::vector<std::string>` — with a `std::vector<ScopeRecord>` where
  `ScopeRecord = { name; parentIndex; depth }` (and, looking ahead to async compute, a `queue` tag,
  single-valued today).
- A GPU scope grabs the **next free pair of query slots** on enter (begin timestamp) and on exit (end
  timestamp), pushing/popping a small index stack so `parentIndex` = the enclosing open scope and
  `depth` = stack size. Parent spans naturally contain child spans because they bracket a wider command
  range. This subsumes the current per-pass behavior: a top-level pass is just a depth-0 scope.

## Pool sizing

- `MaxProfiledPasses = 64` (`renderer_types.cppm:49`) currently bounds *passes*; it must become a
  nesting-aware **scope** cap (e.g. `MaxProfiledScopes`), and `allocateProfilerPools`
  (`renderer.cppm:742-764`, `info.queryCount = 2 * MaxProfiledPasses` at `753`) sizes to
  `2 * MaxProfiledScopes`. Keep the 2-slots-per-scope invariant and the "stop recording once the next
  pair would overflow" guard that exists today.
- Keep `cmd.resetQueryPool` for this frame's pool at record start (`beginFrame` arm, `renderer.cppm:1436`)
  and the per-frame-in-flight ring untouched — only the *capacity* and the *record shape* change.

## Read-back → tree

`readbackGpuTimings` (`renderer.cppm:787-840`) already reads slot `frame.index` with
`e64 | eWithAvailability`, masks by `timestampMask`, and scales by `timestampPeriod`. Extend it to:

- Emit a flat list of `{ name, startTick→ns, endTick→ns, parentIndex, depth }` instead of `{name, gpuMs}`,
  decoded into a tree by the consumer keyed on `parentIndex`/`depth`.
- **Preserve the span-based `lastGpuTotalMs`** (`spanBegin..spanEnd`, `renderer.cppm:836`), *not* a sum
  of children — overlapping/async passes mean parts do not cleanly sum, and the UI must keep labeling
  per-scope numbers "relative" (the note already in `RenderStatsPanel.tsx:429`).

## Where to add sub-scopes

Default stays per-pass (depth 0 only) so the common case keeps a tiny pool. Behind an opt-in (the
capture's include-flags / name-filter from Phase 5), instrument the expensive pass interiors as nested
scopes — the obvious first target is the scene pass: `scene.depth-prepass`, `scene.opaque`,
`scene.transparent`, and the clustered-light compute dispatch as children of the lighting pass. These
are *sub-scopes*, not new top-level passes, so they nest under their parent in the flame chart.

## Files touched

| What | File | Symbols |
|---|---|---|
| Scope record shape + slot stack | `engine/source/saffron/rendering/render_graph.cppm` | `RgTimestamps`, `executeRenderGraph` |
| Scope cap + pool sizing | `engine/source/saffron/rendering/renderer_types.cppm`, `renderer.cppm` | `MaxProfiledScopes`, `allocateProfilerPools` |
| Tree decode at read-back | `engine/source/saffron/rendering/renderer.cppm` | `readbackGpuTimings`, `GpuProfiler.lastTimings` |
| Optional scene sub-scopes | `engine/source/saffron/rendering/renderer_pipelines.cpp` (pass bodies) | scene/lighting pass interiors |

## Validation

- `make engine` + `make prepare-for-commit` clean.
- Headless with `profiler.set-mode timestamps` and scene sub-scopes enabled: the read-back yields a
  tree where the parent pass's span contains its children's spans (`parent.start ≤ child.start` and
  `child.end ≤ parent.end`), depths are correct, and the overflow guard truncates gracefully at the cap.
- `lastGpuTotalMs` is unchanged in meaning (still the begin..end span) and matches the pre-Phase-3 value
  for a flat (depth-0-only) capture — a regression check that nesting didn't change the total.
