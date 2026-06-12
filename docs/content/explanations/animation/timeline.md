+++
title = 'Timeline panel'
weight = 4
+++

# Timeline panel

The Timeline panel is the editor's read-only sequencer for the selected rig. It opens in the
bottom dock and lays out a clip the way every sequencer does: track rows on the left, a time ruler
across the top, clip bars in the lanes, a playhead you can drag, transport buttons with a Loop
toggle, and a `Duration · N tracks · N clips` footer. It is a *viewer* — it reflects the engine's
animation player and lets you scrub to inspect any frame, but it does not author keyframes.

It is the front-end counterpart to the [playback runtime](../playback-runtime/): the engine owns
the pose, the panel shows where the playhead is and drives Edit-mode preview over the control plane.

## Regions

The panel composes the universal sequencer layout from four regions:

- **Track headers** (left, fixed column) — one thin row per track with a type-color accent swatch
  and the clip name. v1 groups one clip under one track row for the bound entity; per-channel and
  per-bone rows are deferred.
- **Ruler** (top) — millisecond ticks with auto-thinning labels. Engine time is seconds-native, so
  ms is the default unit; the tick step is the smallest of a fixed ladder (10ms … 60s) that keeps
  labels from crowding.
- **Lanes** (center) — the playing clip drawn as one horizontal bar spanning its duration on its
  track, tinted with the track accent and labelled inside the bar.
- **Playhead** — one vertical line spanning the ruler and lanes, with a wide transparent grip for
  the scrub gesture.

The ruler, bars, and playhead are all drawn on **one 2D canvas**, not in React. The webview
composites over the live engine viewport, so editor CPU is not free, and a per-tick React re-render
of the panel would fight the render-frequency work. The canvas is created once and fed its model and
playhead position imperatively on `requestAnimationFrame`, the same ownership model as the frame-time
graph. So the playhead advancing on every poll touches only the canvas; the component tree never
re-renders for it.

## Reading playback state

The panel never polls on its own. It reads the selected rig's player off the store slice the
shared reconcile poll fills. The poll runs `get-selection` at ~6 Hz and gates refreshes on version
stamps; alongside the existing play-state gate it carries an **`animationVersion`** stamp. When that
stamp bumps — a play, seek, pause, or loop change, whether from this panel or the `se` CLI — or when
the selection changes to a different entity, the poll refetches `get-animation-state` and
`list-clips` for the selected entity and stores them. The returned `time` drives the playhead.

`get-animation-state` rejects when the entity has no animation player, which is the *not rigged /
nothing playing yet* case, so the slice clears silently. A rig that has not been played yet has no
player but does carry a `SkinnedMesh` component, so the panel gates "is this animatable" on the
selection's component map (filled by the same poll) rather than on the player alone — otherwise the
catalog's clips, which `list-clips` returns for any entity, would show a phantom track on an
unrigged mesh.

## Edit-mode preview transport

The transport drives **Edit-mode preview** of the selected entity, decoupled from the game's global
play state — the same model as UE5 Sequencer and Unity's Timeline window. Play and pause call
`play-animation` / `pause-animation`, which set `previewInEdit` so the clip previews in the viewport
without entering Play; the Loop toggle calls `set-animation-loop`; jump-to-start/end seek to `0` and
`duration`; step nudges the time by one sample interval. During global Play the same panel reflects
the rig as the simulation drives it. The preview is non-destructive — the pose lands in the runtime
pose buffer, never the authored bone transforms — so deselecting or `stop-preview` reverts the rig
to rest.

Dragging the playhead scrubs. The grip's position wraps a drag-local scrub value that emits a
coalesced `seek-animation` (≤1 send in flight, latest wins — intermediate frames are not critical
for scrubbing), and flushes the final value on release so the wire reads where you let go, not a
frame-old value. While paused, each seek runs the one-shot evaluator, so the viewport pose updates
live as you drag.

## Deferred authoring

Keyframe **authoring** is out of scope here, mirroring the Timeline-vs-Animation-window split in
other engines. The lane renderer is already built for it: it has a separate `diamonds` draw mode and
a `keys` model field, so a future authoring lane (diamonds for keys, curves for interpolation) drops
in without restructuring the layout or the seconds↔pixels transform. v1 also draws a single clip bar
per entity; multi-clip sequencing and per-bone lanes are the visual target the structure supports
but does not yet populate.

## In the code

| What | File | Symbols |
|---|---|---|
| The panel: regions, transport, scrub wiring | `editor/src/panels/TimelinePanel.tsx` | `TimelinePanel`, `isRiggedEntity` |
| The canvas renderer (ruler, bars, playhead, diamonds mode) | `editor/src/lib/timelineCanvas.ts` | `TimelineCanvas`, `TimelineModel`, `LaneMode` |
| The poll gate + state slice | `editor/src/state/store.ts` | `startReconcile`, `refreshAnimation`, `setAnimationState` |
| Typed control wrappers | `editor/src/control/client.ts` | `getAnimationState`, `listClips`, `playAnimation`, `seekAnimation`, `setAnimationLoop` |
| Scrub + coalesce primitives | `editor/src/lib/useScrubValue.ts`; `editor/src/control/coalesce.ts` | `useScrubValue`, `makeCoalescer` |

> [!NOTE]
> The panel is read-only. Keyframe editing, multi-clip sequencing, and per-bone lanes are deferred;
> the renderer's `diamonds` mode and `keys` field are the seams they plug into.

## Related

- [Playback runtime](../playback-runtime/) — the evaluator whose `time` the playhead shows
- [Skeleton overlay](../skeleton-overlay/) — the viewport bones the preview moves
- [Animation data model](../animation-data-model/) — the clip and pose types behind the bars
- [Rig editor](../../ui-and-editor/rig-editor/) — the same transport + surface components, mounted against the preview rig
