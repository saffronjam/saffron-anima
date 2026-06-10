# Phase 12 — read-only canvas `TimelinePanel`

**Status:** COMPLETED

## Goal

The headline UI: a polished, **read-only** timeline inside the Phase-11 bottom dock, matching the shared
mock — track rows (left) / time ruler (top, ms) / clip bars (lanes) / a scrubbable playhead / transport
controls + a **Loop** toggle / a `Duration · N tracks · N clips` footer. It reflects the playing
animation (reading `get-animation-state` on the reconcile poll) and lets you scrub the playhead to inspect
any frame. Canvas-rendered like `FrameTimeGraph` so it never regresses the render-frequency work. Keyframe
**authoring** is out of scope (Unity's Timeline-vs-Animation-window split) — but the lane renderer is
built so diamonds/curves can drop in later.

## What exists to build on

- The dock + tab system from Phase 11 (`BottomTool='timeline'`, `BottomDock.tsx` renders the active tool).
- Control plane from Phase 5: `client.getAnimationState`, `listClips`, `playAnimation`, `pauseAnimation`,
  `seekAnimation`, `setAnimationLoop`, and the `animationVersion` stamp on `PlayStateResult`/`SelectionResult`.
- The reconcile poll: `startReconcile` (`store.ts:987-1285`) runs `get-selection` ~every 167 ms and gates
  refreshes on version stamps — `if (selection.playVersion !== knownPlayVersion) …` (`:1252`). Add a
  parallel `animationVersion` gate that refetches `get-animation-state` for the selected entity.
- Canvas pattern: `FrameTimeGraph.tsx` (`src/components/`) creates a uPlot/canvas once in a ref, subscribes
  to store data, and calls `plot.setData()` **imperatively on rAF** — never per-tick React state
  (`:184-208`). Its own comment: the webview composites over the live engine viewport, so editor CPU is
  not free. Follow this exactly.
- Scrub + coalesce: `useScrubValue(prop, emit)` (`src/lib/useScrubValue.ts`) — drag-local state, `begin()`/
  `end()` bracket the gesture, emits coalesced to rAF, follows `prop` outside the gesture.
  `makeCoalescer({throttleMs, send})` (`src/control/coalesce.ts`, default 16 ms) keeps ≤1 send in flight.
  `ViewportPanel` uses `makeCoalescer` in a `useMemo([])` for gizmo input (`:95-100`) — the model to copy.
- `Tabs variant="line"` (`src/components/ui/tabs.tsx:25-68`) for any in-panel tabbing; dark tokens in
  `styles.css:46-66` (`--border` = `oklch(1 0 0 / 10%)`, `--muted-foreground` = `oklch(0.708 0 0)`,
  `--primary` = `oklch(0.922 0 0)`); `ResizablePanelGroup`/`ResizableHandle` (`src/components/ui/resizable.tsx`).

## Work

### 1. `TimelinePanel.tsx` — region structure

Build under `editor/src/panels/`, composed of the universal sequencer regions:
- **TrackHeaderList** (left, fixed-width column): one row per track, a type-color accent swatch + name.
  v1 groups under the bound entity (one clip → one track row; per-channel/per-bone rows are deferred).
  Thin rows (~22–26 px). Optionally a `+ Add track` affordance disabled in read-only v1 (matches the mock
  visually without enabling authoring).
- **Ruler** (top): ms ticks with auto-thinning labels (engine time is seconds-native, so ms is the default
  branch); reserve a Seconds↔Frames toggle slot (FPS source = the clip's sampler rate) but ms-only is
  fine for v1.
- **LaneArea** (center, the only 2D-scrolling region): the playing clip as one horizontal **bar** spanning
  its duration on its track. **Render on a canvas** with area-virtualization (only visible ticks/bars),
  using the `FrameTimeGraph` ref-init + rAF-`setData` pattern. Keep the renderer able to draw **diamonds**
  in a separate draw mode so a future authoring lane needs no restructure.
- **Playhead**: one vertical line spanning ruler + lanes with a wide hit target.
- **Transport** (a `Topbar`-style button group): jump-start / step-back / play-pause / step-fwd / jump-end
  + the **Loop** toggle (lucide icons, `flex gap-0.5 rounded-md border border-border p-0.5`,
  `size="icon-sm"`).
- **Footer**: `Duration {d} · {nTracks} tracks · {nClips} clips` + a current/total time readout.

### 2. Data flow (read-only)

- On selecting a rigged entity, fetch `listClips` + `getAnimationState`; render the clip bar(s) and set
  duration/footer.
- The reconcile poll's new `animationVersion` gate refetches `getAnimationState`; the returned `time`
  drives the playhead each poll (the canvas `setData` is scheduled on rAF only when `time`/state actually
  changes — never every render tick, per the `FrameTimeGraph` gotcha).
- **Transport drives Edit-mode preview of the selected entity** (decoupled from the game's play state, like
  UE5 Sequencer / Unity Timeline): play/pause-toggle calls `client.playAnimation(entity, clip, …)` /
  `pauseAnimation(entity)` (which set `previewInEdit` so the clip previews in Edit without entering Play),
  Loop toggle calls `setAnimationLoop`, jump-start/end set `seek-animation` to `0`/`duration`, step nudges
  `time` by one sample interval. During global **Play**, the same panel reflects the rig as the simulation
  drives it. The viewport updates because the Phase-3 evaluator runs every frame in both modes.
- **Scrub**: wrap the playhead position in `useScrubValue<number>(time, emitSeek)` where `emitSeek` pushes
  to a `makeCoalescer<number>({throttleMs: 50, send: t => client.seekAnimation(entity, t)})` created in a
  `useMemo([])`. `begin()` on pointer-down, `end()` (flush) on pointer-up so the final value lands. While
  Paused, the Phase-5 `seek-animation` one-shot eval updates the viewport pose live as you drag.

### 3. Style polish

Thin rows, slim ruler, `border-border` separators (the subtle 10% white), type-color accents as the main
visual signal, a high-contrast (`--primary`) playhead, transport in the `Topbar` button-group style,
`Tabs variant="line"` for any dock-level tab chrome — so it reads as a native sibling of Stats/Profiler.
Match the mock: dark background, clip bars with a left-aligned label + icon, ms ruler with `0ms / 250ms /
500ms / …` labels, the Loop toggle top-right, the footer bottom-right.

## Validation (done criteria)

- `bun run check` + `bun run lint` clean.
- Manual (`make run`): select a rigged entity → the timeline shows the clip bar, duration, and footer;
  hit Play → the **playhead advances** smoothly and the mesh animates; toggle Loop → the clip loops;
  **drag the playhead while Paused → the pose scrubs live** in the viewport; the panel stays smooth (no
  per-frame React re-render storm — verify with the dev render-frequency logger).
- `docs/`: add `docs/content/explanations/animation/timeline.md` (the read-only viewer, the regions, how it
  reads playback state, the deferred authoring mode) + hub row.

## Notes / gotchas

- **Canvas, not React, for the lanes/ruler/playhead motion.** Per-tick React state for the playhead would
  re-render the panel every frame and fight the render-frequency work the recent commits added. Init the
  canvas once, pump `setData` on rAF, only when data changes.
- `useScrubValue.end()` must flush **before** the panel's drag-end so the wire reads the final value, not
  a frame-old one (the documented requirement).
- `makeCoalescer` overwrites pending values — a fast scrub only sends the latest `seek`; that's correct for
  scrubbing (intermediate frames aren't critical).
- v1 has **one clip bar per entity**; the mock's multiple tracks/clips are the visual target the structure
  supports, but per-channel/per-bone lanes and multi-clip sequencing are deferred.
- Transport drives **Edit-mode preview** of the selected entity (it sets `previewInEdit` via
  `play-animation`/`seek-animation`), so you see the clip without entering Play — exactly like UE5 Sequencer
  and Unity's Animation/Timeline windows. It is non-destructive because the Phase-3 pose lands in the
  runtime `PoseBuffer`/`PoseOverrideComponent`, never the authored bone transforms. Clear preview
  (deselect / `stop-preview`) reverts the rig to rest pose.
