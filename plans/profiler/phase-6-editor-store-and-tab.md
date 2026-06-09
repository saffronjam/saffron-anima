# Phase 6 — editor store + Profiler tab

**Status:** COMPLETED

Give the editor the typed plumbing for captures and a home to put them — the `captureState`/`capture`
Zustand slice, the typed client wrappers for the Phase 5 commands, and the new **`profiler` dock tab**
with an empty panel shell. No visualizations yet; this is the wiring Phase 7 fills in. Crucially, the
Profiler is a **separate, self-contained tab** peer to Stats — Stats and `RenderStatsPanel` are left
**untouched** (the chosen design: side-by-side, not a unified hub).

## The new dock tab

The bottom-left dock is a Godot-style tabbed debugger (`Layout.tsx:174-215`, `LeftBottomTabs`) with
`BottomTab = "inspector" | "environment" | "stats"` (`store.ts:37`). Add a fourth:

- Extend the union: `BottomTab = "inspector" | "environment" | "stats" | "profiler"` (`store.ts:37`).
- Add a `TabsTrigger value="profiler"` and a `TabsContent value="profiler"` mounting a new
  `ProfilerPanel` in `LeftBottomTabs` (`Layout.tsx:194-212`), mirroring the existing `stats` entries.
- `RenderStatsPanel` and the `stats` tab are unchanged.

## The capture store slice

Add a `captureState` enum + a `capture` slice alongside the existing metrics slices, following the
`metricsRangeSec`/`metricsRefreshMs` localStorage-persistence pattern (`store.ts:29-32,433-458`):

- `captureState: "idle" | "arming" | "recording" | "ready"` and `captureProgress { current; total }`.
- `capture: ProfileCaptureDto | null` (the last completed capture) and `captureWindowFrames: number`
  (persisted preference, default **1** — the single-frame default; presets 1/8/64/256).
- `selectedPass: string | null` for the cross-highlight Phase 7 uses across its sub-views.
- Setters following the existing pattern (`setBottomTab` at `store.ts:308`, `setFrameHistory`/
  `setPassTimings` at `422-423`).

Keep the capture data **out** of the 1 Hz metrics lane: capture is on-demand. The heavy
`frameHistory`/`passTimings` poll gated on `bottomTab === "stats"` (`store.ts:965`) stays as-is for the
HUD; the Profiler tab does **not** add a polling lane — it fetches only when the user clicks Capture.

## Client wrappers

Add typed wrappers in `editor/src/control/client.ts` using the generated `CommandParamsMap`/
`CommandResultMap` and the existing `call<C>()` helper (`client.ts:104`):

- `captureStart(params)` → `call("profiler.capture-start", params)`.
- `captureStop()` → `call("profiler.capture-stop")`, returning the `ProfileCaptureDto` (and, for the
  file path case, surfacing `{ path, pending }`).
- Reuse the existing `setProfilerMode` wrapper for the arm path (the same plumbing
  `RenderStatsPanel.tsx:282-287` already uses).

## Span → flame-node transform

Add a pure transform module (`editor/src/lib/captureTree.ts`) mapping `ProfileSpanDto[]` onto the
`flame-chart-js` node shape `{ name, start, duration, children }`, keyed by `parentIndex`/`depth`, with
one tree per lane (CPU, GPU). Phase 7's views consume this ready tree; keeping it a pure function makes it
unit-testable and keeps the panel declarative. Add a separate small capture-span buffer here — **not** the
72k-sample `frameSeries.ts` ring, which backs the live HUD only.

## Files touched

| What | File | Symbols |
|---|---|---|
| `BottomTab` union + tab | `editor/src/state/store.ts`, `editor/src/app/Layout.tsx` | `BottomTab`, `LeftBottomTabs` |
| Capture slice | `editor/src/state/store.ts` | `captureState`, `capture`, `captureWindowFrames`, `selectedPass`, setters |
| Client wrappers | `editor/src/control/client.ts` | `captureStart`, `captureStop` |
| Span transform | `editor/src/lib/captureTree.ts` (new) | `spansToFlameTree` |
| Panel shell | `editor/src/panels/ProfilerPanel.tsx` (new) | `ProfilerPanel` |

## Validation

- `bun run check` (regenerate `@saffron/protocol` + typecheck) clean; `bun run lint`/`format` clean.
- The Profiler tab appears in the dock and selects; the empty shell renders an empty-state prompt.
- A temporary dev-only button calls `captureStart({mode:"single"})` then `captureStop()` and
  `console.log`s a valid flame tree (CPU + GPU lanes) — proving the round-trip and the transform before
  any visuals exist.
- `make run` shows the new tab alongside Stats with both functioning independently.
