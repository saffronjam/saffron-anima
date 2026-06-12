# Phase 9 — the timeline in the rig editor

**Status:** COMPLETED (kept as a build record; superseded by the general Asset editor — see this dir's README)

**Depends on:** phase 2 (the preview scene + the spawned rig entity), phase 6 (the skeleton tree's
highlight channel), phase 7 (the clip list), phase 8 (the extracted timeline components).

## Goal

Mount the shared timeline (phase 8) in the rig-editor workspace's bottom strip, targeted at the
preview rig: play/pause/step/loop and **full-rate scrubbing of the live preview** — the headline
interaction of the whole feature (UE5's bottom Asset Editor timeline). Because the preview rig is
the selected entity in the active (preview) scene, the existing state pipeline and command wrappers
drive it without protocol changes.

## What exists to build on

- `TimelineTransport` + `TimelineSurface` with the `TimelineTarget` seam (phase 8).
- The state source: entering the preview selects the rig (phase 2) **and the selection stays on the
  rig** — phase 6's bone highlighting deliberately uses the highlight channel, not scene selection,
  so the selection-keyed `animationState`/`animationClips` slices stay filled for the whole tab
  lifetime (`store.ts:1216-1240`, `:1410-1416`). The workspace builds its `TimelineTarget` from
  those slices, gated on the rig tab being active. (If a future change ever moves selection off the
  rig, source the state keyed to `rigEntity` instead — but with phase 6 as specified, the
  selection-keyed slice is safe.)
- Scrub-to-live-pixels: `seek-animation` sets `previewInEdit` + the playhead
  (`control_commands_animation.cpp:234-247`); the evaluator poses the rig that frame
  (`animation.cpp:589-594`); the frame reaches the workspace's subsurface pane — the 50 ms
  coalescer (`TimelinePanel.tsx:100-119`) is already tuned for the serialized wire.
- Clip switching comes from the clip list (phase 7) — the transport's clip `Select` stays hidden
  here.
- Keyboard: the editor's binding helper (`matchesBinding`, used by `AssetTile.tsx:193-207`) for
  Space = play/pause scoped to the workspace.

## Work

### 1. Mount + target

The workspace's bottom strip renders `TimelineTransport` (no clip Select) + `TimelineSurface` with
`TimelineTarget { entityId: rigEntity, state: animationState, clips: [], enabled: previewActive }`.
`rigEntity` comes from the enter-result (phase 2 — the rig entity uuid `instantiateModel` spawned
from the model container, not the mesh asset uuid that keys the tab); `enabled` keys off the tab
being active and the preview being entered (error states render the empty surface).

### 2. Interaction completeness

- Space = play/pause while the workspace has focus (not while a rename/input is focused — the
  binding helper's existing guards).
- Step buttons step one sample (`STEP_SEC`, the dock's `1/30` default) — if the clip's authored
  sample rate ever lands on `AnimationClipDto`, prefer it; otherwise keep the constant and note it.
- Loop toggle drives `set-animation-loop` exactly as the dock does.
- The footer reads `Duration · 1 track · N clips` from the rig's linked clips (phase 7's count),
  not the global catalog.

### 3. Hardening the takeover seams

- Scrubbing immediately after tab activation (enter still in flight): the coalescer's
  single-in-flight pump already serializes; ensure the transport disables until the first
  `animationState` lands (the `enabled` flag).
- Closing the tab mid-scrub: pointer-capture release + `scrub.end()` flush happen before
  `exit-rig-preview` (effect cleanup order) so no stray seek lands on the authored scene's
  selection after restore.

## Validation (done criteria)

- `bun run check` + `bun run lint` clean.
- Manual (`make run`): open the leg rig → press play: the leg bends in the live preview while the
  playhead advances; drag-scrub anywhere on the lane: the pose follows the pointer fluidly (no
  visible stepping — this is the moment the subsurface decision pays off); loop on/off; step
  back/forward; Space toggles; switch clips via the list (phase 7) and the lane re-lays.
- `make e2e`: headless equivalent — enter preview, `seek-animation` to three times, assert
  `get-animation-state.time` tracks and a bone's `get-world-transform` changes between them
  (the pose really followed); scrub-burst (rapid seeks) stays validation-clean and the final state
  matches the last seek.
- `docs/`: the rig-editor page gains the timeline/transport section; cross-link the dock Timeline
  page (same components, different target).

## Notes / gotchas

- The dock Timeline and the rig timeline are **never live simultaneously** (the dock is hidden with
  the scene tab) — but both read the same `animationState` slice; the `enabled` flags keep the
  hidden one inert. If the revamp later allows both visible, the slice needs splitting — note it,
  don't build it.
- Don't add zoom/pan here even though scrubbing begs for it on long clips — `secToX` centralizes
  the future transform (`timelineCanvas.ts:123`); it is a self-contained follow-up.
- The 50 ms seek coalescer + ~20 Hz poll means the *playhead* can lag the *pose* by a frame during
  scrubs — the canvas's optimistic `setPlayhead` on pointer-move already masks it (the dock does
  the same); keep that behavior in the shared surface.
