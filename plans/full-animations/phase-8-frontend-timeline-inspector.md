# Phase 8 — Frontend: Timeline channel drill-down + Inspector morph sliders

**Status:** NOT STARTED

**Depends on:** Phase 7 (the control DTOs the UI binds to)

## Why

Morph weights and node/transform tracks must surface inside the **existing** Timeline + Clips +
Inspector model — extend it, never a parallel UI. One clip stays one timeline bar; channels are nested
metadata for inspector drill-down and the rig gate widens to include morph/node-animated entities.

## Grounding (the existing UI seams)

- `TimelinePanel.tsx` `isRiggedEntity` (checks `"AnimationPlayer" || "SkinnedMesh"` in components) gates
  the panel; mounts `TimelineTransport` + `TimelineSurface` against a `TimelineTarget` (`shared.ts`).
- `TimelineTarget { entityId; state; clips; enabled }` (`shared.ts`). `TRACK_ACCENT`, one clip → one
  track row (the locked v1 constraint).
- `TimelineSurface.tsx` / `timelineCanvas.ts`: imperative canvas, one lane row, self-advancing playhead,
  `resyncPlayhead` snaps to `animationState.time` on poll. `TimelineModel.clips = [{trackId:"anim",...}]`.
- `InspectorPanel.tsx`: data-driven component bodies via `fieldRenderer`; `SkinnedMesh`/`AnimationPlayer`
  are read-only summaries (no FIELD_HINTS yet). `componentOrder` lays out sections.
- `SkeletonTree.tsx`: asset-editor bone tree; bone rows drive `set-skeleton-highlight`.
- `store.ts`: `animationState` (`AnimationStateResult`), `animationClips`; reconcile poll on
  `selectionVersion`, `animationVersion`-gated, ~6 Hz, focus+phase gated.

## Decisions (locked)

1. **One clip → one timeline bar stays. Channels are inspector drill-down, not new lanes.** The dock
   timeline is unchanged structurally: the `"anim"` track row is not multiplied. `AnimationClipDto.channels`
   (Phase 7) feeds a footer/count ("Clip · 6 channels: 4 bone · 1 morph · 1 node") and the Inspector
   drill-down — matching the locked frontend constraints (one bar; channels nested metadata).
2. **Inspector gains an `AnimationChannels` section (sibling, not merged).** Below the read-only
   `SkinnedMesh`/`AnimationPlayer` summaries, render a new section listing the selected entity's morph
   targets as per-target weight rows: `name + 0..1 slider + numeric field`, value from
   `AnimationStateResult.morphWeights` (live during playback) backed by `set-morph-weights`. This is the
   inspector counterpart to a morph weight track; it is the single details view (no separate channel
   panel). Implemented as a new `FieldKind`/section in the generic `fieldRenderer` path, React-driven
   (the canvas stays imperative).
3. **Canonical weight range 0..1 on the wire** (Phase 7 DTO), displayed as a 0..1 slider (optionally a
   `%` label) — one persisted representation, never 0..100 + 0..1 dual (the single-format rule).
4. **Rig gate widens to "animatable", one predicate.** `isRiggedEntity` becomes `isAnimatable`:
   `"AnimationPlayer" || "SkinnedMesh" || "Morph"` in the selection's components — so a node-TRS
   container (has `AnimationPlayer`) and a morph mesh (has `Morph`) both light up the timeline/transport.
   One predicate, updated in place (no second gate). `TimelineTarget.enabled` derives from it.
5. **`store.ts` carries live channel values.** `animationState.morphWeights` is already polled (Phase 7
   added it to `AnimationStateResult`); the Inspector reads it for live slider values. Optionally add
   `animationChannels` (per-clip metadata) fetched alongside `animationState` on the
   `selectionVersion` bump, reusing the existing parallel fetch (no new poll).
6. **`SkeletonTree` morph rows (asset editor, optional).** Under the bone tree, an optional collapsed
   "Morphs" group lists morph-target names (read-only), so the asset editor mirrors the inspector. No
   keyframing UI (out of scope v1, per the locked constraints).

## Edits

- `editor/src/protocol/sa-types.ts`: regenerated (Phase 7) — no hand-edit.
- `shared.ts`: `TimelineTarget` unchanged; channel-count helper for the footer.
- `TimelinePanel.tsx`: `isRiggedEntity` → `isAnimatable` (3-way predicate); pass channel metadata to the
  surface footer.
- `TimelineSurface.tsx`/`timelineCanvas.ts`: footer shows the channel breakdown; one bar unchanged.
- `InspectorPanel.tsx`: add the `AnimationChannels` morph-weight section (sliders → `set-morph-weights`,
  live value from `animationState.morphWeights`).
- `SkeletonTree.tsx`: optional collapsed Morphs group.
- `store.ts`: read `morphWeights` from `animationState`; optionally fetch per-clip `channels`.
- `editor/src/control/client.ts`: `setMorphWeights`/`getMorphWeights` (Phase 7).

## Verification

- `cd editor && bun install && bun run check` (regen protocol + typecheck); `bun run lint`/`format`.
- A morph mesh selected: the Inspector shows weight sliders; dragging one calls `set-morph-weights` and
  the viewport deforms; during clip playback the slider tracks the live value.
- A `BoxAnimated` container selected: the timeline/transport lights up (gate widened) and play/seek/loop
  drive the node animation via the existing transport commands.
- The dock timeline still shows exactly one clip bar (no extra lanes).

## Risks

- **Optimistic vs authoritative weight values:** the slider shows live `animationState.morphWeights`
  during playback but an authored edit is optimistic until the next poll — mirror the existing clip-select
  optimism (`pendingClip`) so the slider doesn't flicker against the poll.
- **Gate regression:** widening `isRiggedEntity` must not light the timeline for non-animatable
  entities — the 3-way predicate is exact (component presence), keep it a single source.
- **Inspector section order:** add `AnimationChannels` via `componentOrder` so it renders in a stable
  place, not interleaved by component-map iteration order.
