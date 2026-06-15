# Phase 8 — Frontend: Timeline channel drill-down + Inspector morph sliders

**Status:** NOT STARTED

**Depends on:** Phase 7 (control plane: `set-morph-weights`/`get-morph-weights`/`list-clip-bindings`,
`AnimationClipDto.channels`, `AnimationStateResult.morphWeights`, `AnimationChannelDto`).

## Why

Morph weights and node/transform tracks are now drivable engine state (Phases 4–7). They surface in the
editor by **extending the existing Timeline + Clips + Inspector**, never as a parallel UI. The locked UX:

- **One clip = one timeline bar.** The "anim" track row is not multiplied; channels are nested
  drill-down metadata, not extra lanes.
- The Inspector grows one **`AnimationChannels`** section (sibling to the read-only `SkinnedMesh` /
  `AnimationPlayer` summaries, placed via `componentOrder`) listing morph targets as name + `0..1`
  slider + numeric rows, value live from `AnimationStateResult.morphWeights` during playback, edits
  backed by `set-morph-weights` with optimistic update and coalesced scrubs.
- The rig gate widens from `isRiggedEntity` (SkinnedMesh ‖ AnimationPlayer) into **one `isAnimatable`
  predicate** (AnimationPlayer ‖ SkinnedMesh ‖ Morph) so a node-TRS container and a morph mesh both
  light up the timeline/transport.

NO LEGACY: there is no second timeline, no bespoke morph track type, no `0..100` weight representation.
`0..1` is the single wire/persisted form (a `%` is display-only if shown at all). Node-TRS plays through
the **existing** transport commands (`play-animation`/`seek-animation`/`set-animation-loop`); no new
playback verb.

## Backend work

**None.** Phase 7 shipped every command, DTO field, and the `morphWeights`/`channels` exposure this
phase reads. If a needed DTO field is missing, that is a Phase 7 gap — fix it there, regenerate
`sa-types.ts`, do not hand-edit the generated file.

## Grounding (real files/symbols)

- `editor/src/panels/TimelinePanel.tsx` — `isRiggedEntity` (the gate to widen; checks
  `"AnimationPlayer" || "SkinnedMesh"` in components), the four store reads, the `TimelineTarget` build
  (`enabled: animationState !== null || isRiggedEntity(components)`).
- `editor/src/components/timeline/shared.ts` — `TimelineTarget`, `TRACK_ACCENT`, `formatTime`, `guard`;
  imports `AnimationClipDto`/`AnimationStateResult` from `../../protocol`.
- `editor/src/components/timeline/TimelineSurface.tsx` — `applyModel` (builds the single-clip model from
  `[{ id: "anim", accent: TRACK_ACCENT }]`), the footer summary line
  `Duration … · N tracks · N clips` (`nTracks`/`nClips`), the imperative canvas/scrub/seek pipeline
  (`seekCoalescer`, `beginScrub`/`moveScrub`/`endScrub`).
- `editor/src/lib/timelineCanvas.ts` — `TimelineModel`/`TimelineTrack`/`TimelineClip`, `TimelineCanvas`
  (imperative, no React state). Stays one clip = one bar; **not** edited to add lanes.
- `editor/src/panels/InspectorPanel.tsx` — `NON_ADDABLE` (`:50`), `NON_REMOVABLE` (`:45`),
  `componentBody` early-return dispatch (`:440`), the `coalescerFor`/`makeCoalescer` +
  `onFieldDragStart`/`onFieldDragEnd` (`setDragActive`) bracketing (`:252-287`), `applyOptimisticComponent`,
  `ReadonlyRow` (`:62`), the rig-body pattern (`SkinnedMesh`/`FootIk`/`KinematicBones` branches),
  `fieldGesture`/`recordFieldEdit` (undo capture).
- `editor/src/lib/componentOrder.ts` — `COMPONENT_ORDER` (placement of `Morph`), `HIDDEN_COMPONENTS`,
  `orderedComponentNames`.
- `editor/src/components/SliderField.tsx` — bounded `0..1` slider with `useScrubValue` +
  `onDragStart`/`onDragEnd` brackets (the exact widget the morph rows reuse; `Material.metallic` uses it
  with `min:0 max:1 step:0.01`).
- `editor/src/components/fieldRenderer.tsx` — `FIELD_HINTS` (`slider` kind, `min`/`max`/`step`),
  `renderField`/`resolveHint`.
- `editor/src/state/store.ts` — `animationState`/`animationClips` slices, `setAnimationState` (`:973`),
  `refreshAnimation` (the `animationVersion`-gated ~6 Hz reconcile fetch — `:1778`), `dragActive`,
  `applyOptimisticComponent`, `componentsBySelected`.
- `editor/src/control/client.ts` — typed wrappers; Phase 7 adds `setMorphWeights`/`getMorphWeights`/
  `listClipBindings`; this phase consumes them.
- `editor/src/panels/SkeletonTree.tsx` — optional collapsed "Morphs" group (read-only).
- Protocol: `editor/src/protocol/sa-types.ts` (generated). `AnimationStateResult` (`:762`),
  `AnimationClipDto` (`:781`); Phase 7 adds `channels`, `morphWeights`, `AnimationChannelDto`,
  `MorphWeightsResult`. `editor/src/protocol/index.ts` is the hand-kept re-export shim.

**Rebrand note:** the in-flight `plans/rebrand-anima` effort has landed `editor/src/components/anima/`
and the `se-types.ts → sa-types.ts` rename is done (the protocol now lives at
`editor/src/protocol/sa-types.ts`). Reference protocol by `../protocol` (the `index.ts` shim) and follow
whatever that effort settles for naming — do not fight the rename or re-introduce `se-types.ts`.

## Frontend work (ordered)

### 1. One `isAnimatable` predicate replaces `isRiggedEntity`

In `TimelinePanel.tsx`, rename/replace `isRiggedEntity` with `isAnimatable(components)` returning true
when the component map contains **any** of `AnimationPlayer`, `SkinnedMesh`, or `Morph`. Update the
`target.enabled` derivation (`animationState !== null || isAnimatable(components)`). This is the single
gate — delete the old name and every reference (it is only used here). The asset-editor mount derives
`enabled` from preview-active as before; no change there.

Rationale: a `BoxAnimated` node-TRS container carries an `AnimationPlayer` (Phase 4 attaches one to the
animated subtree) so it already passes; a static morph mesh carries only `Morph`, which the old gate
missed — widening lets its transport/Inspector light up.

### 2. `Morph` in `componentOrder` + `NON_ADDABLE`/`NON_REMOVABLE`

- `componentOrder.ts`: add `"Morph"` to `COMPONENT_ORDER` adjacent to the rig group (after
  `AnimationPlayer`, before `Camera`) so its Inspector section lands next to the animation summaries.
  (The generated schema already emits the component; `COMPONENT_ORDER` is ordering-only.)
- `InspectorPanel.tsx`: add `"Morph"` (the durable `MorphComponent` wire name) to `NON_ADDABLE` — it is
  import-managed identity (seeded at spawn from glTF), never created on a bare entity, mirroring
  `SkinnedMesh`/`ModelInstance`. Also add it to `NON_REMOVABLE`, matching the `SkinnedMesh` reasoning at
  `:45` (a morph mesh with the component stripped strands the deformer with no way back).

### 3. Inspector `AnimationChannels` section (morph sliders)

Add a dedicated body branch in `componentBody` for the `Morph` component (early return, same idiom as
the `SkinnedMesh`/`FootIk`/`KinematicBones` branches), titled via `humanizeComponentName`. It renders
the morph **targets** as rows:

- **Names + count:** the morph target names come from Phase 7's `get-morph-weights`
  (`MorphWeightsResult.names`). Fetch once per selection (cache in a ref keyed by `selectedId`,
  refetched on selection change) — names are static per mesh and not in the ~6 Hz poll. The `Morph`
  component DTO carries the authored `weights` array; names come from the command. Use `weights.length`
  for the count when names are still loading.
- **Live value:** read `AnimationStateResult.morphWeights[i]` from the store's `animationState` slice
  when present (live during playback, carried by the existing `animationVersion`-gated reconcile —
  **no new poll**). Fall back to the component's authored `weights[i]` when `morphWeights` is absent
  (not playing). During playback the slider must visibly track the live value.
- **Row widget:** reuse `SliderField` (`min:0 max:1 step:0.01`) plus its numeric readout — the exact
  `0..1` slider used for `Material.metallic`. Each row: `humanizeFieldName(name)` label in the existing
  `78px` grid column + `SliderField` on the right. Canonical `0..1` on the wire; a `%` readout, if
  shown, is display-only (`Math.round(v * 100)`), never stored.
- **Edit path (optimistic + coalesced):**
  - On change, optimistically overlay the patched weights vector via `applyOptimisticComponent("Morph",
    { ...current, weights })` (mirrors `pendingClip` and the field machinery), so the slider holds
    without waiting on the wire and the reconcile poll (gated by `dragActive`) won't clobber it
    mid-drag.
  - Route the send through a per-target coalescer built with `makeCoalescer` (one entry per morph index,
    keyed `Morph#<i>`): `send: (w) => client.setMorphWeights(id, { index: i, weight: w })` — the
    single-target shape Phase 7's `SetMorphWeightsParams { index, weight }` supports. One
    `set-morph-weights` per scrub burst, not per tick (the serialized-control rule).
  - Bracket the drag with `SliderField`'s `onDragStart`/`onDragEnd` → `setDragActive(true/false)`, and
    on release re-push the final exact value once (the same release re-push the field machinery does at
    `onFieldDragEnd`). A scrub becomes **one preview per burst**.
  - Undo: record one entry per gesture through the existing `pushEdit`/`recordFieldEdit` pattern (prior
    vs after weights vector) so a morph scrub is undoable like a field scrub, matching `fieldGesture`.
    If that capture does not generalize cleanly to the per-index coalescer, note the choice in the PR
    and omit undo for v1 only — do not ship a half-wired undo.
- **Node/bone channels (read-only rows):** when the selected clip's `AnimationClipDto.channels` carries
  `node-*`/`bone-*` channel entries (the per-path `kind` tokens Phase 7 emits, e.g. `"node-translation"`,
  `"bone-rotation"`), list them below the morph sliders as read-only `ReadonlyRow`-style
  name + kind rows (optionally with an unresolved-binding badge from `list-clip-bindings`,
  `ClipBindingDto.resolved === false`). These are the drill-down detail; morph is the only interactive
  one.

The existing `SkinnedMesh`/`AnimationPlayer` bodies stay unchanged; `AnimationChannels`/`Morph` is the
one new interactive rig body.

### 4. Timeline footer channel breakdown (drill-down metadata, NOT lanes)

The clip stays one bar on the one "anim" track (`timelineCanvas.ts` and the single
`[{ id: "anim" }]` track list in `TimelineSurface.applyModel` / `TimelinePanel.tracks` are unchanged).
Channels surface as **metadata in the footer summary**, not as extra rows:

- The selected clip's `AnimationClipDto.channels` (Phase 7, populated for a single-clip query) feeds a
  breakdown appended to the `TimelineSurface` footer, e.g.
  `… · 6 channels: 4 bone · 1 morph · 1 node`. Derive the counts by grouping `channels` on the `kind`
  prefix (`bone-*` → bone, `node-*` → node, `morph-weights` → morph), matching Phase 7's per-path token
  vocabulary (`"bone-rotation"`, `"node-translation"`, …, `"morph-weights"`). Keep the existing
  `Duration … · N tracks · N clips` portion.
- `channels` is empty in the global `list-clips` catalog (the "0 when unknown" convention), so the
  breakdown needs a single-clip source. Get it from the clip carried by the currently-selected
  `AnimationStateResult` (or the asset-model path `get-asset-model` → `AssetModelResult.clips[].channels`
  for the asset editor / `ClipList`). When the catalog entry has empty `channels`, the breakdown
  degrades to `N tracks · N clips` (no breakdown) — never a crash. Keep this purely derived; add a store
  slice only if a single-clip channels fetch is needed, and if so fold it into the
  `animationVersion`-gated reconcile, not a new timer.

The per-channel list belongs to the Inspector `AnimationChannels` section (step 3); the timeline carries
only the **count** breakdown — the "nested metadata, no extra lanes" rule.

### 5. Node-TRS lights the transport (no new code path)

Because `isAnimatable` now returns true for an `AnimationPlayer` container, a `BoxAnimated` selection
already drives the existing `TimelineTransport` + `TimelineSurface` — play/pause/seek/loop call the same
`client.playAnimation`/`seekAnimation`/`setAnimationLoop` against the container entity id. Verify the
clip Select dropdown and scrub pipeline work unchanged for the container (they are entity-agnostic). No
new commands, no node-specific UI.

### 6. Optional: collapsed `SkeletonTree` "Morphs" group

In `SkeletonTree.tsx` (asset editor left panel), optionally add a read-only, collapsed-by-default
"Morphs" group listing the model's morph target names beneath the bone tree, when the previewed model
has morph targets (from `get-asset-model` / `get-morph-weights`). Read-only navigation only — no
selection wiring required. Ship it only if it does not complicate the bone-tree code.

### 7. `client.ts` wrappers

Confirm the Phase 7 typed wrappers exist and are imported: `setMorphWeights(entity, { weights } | {
index, weight })`, `getMorphWeights(entity)`, `listClipBindings(...)`. If Phase 7 only added them as
`.raw()` stubs, finalize the typed signatures here against the generated `sa-types.ts` (edit `client.ts`,
never the generated file). `listClipBindings` powers the optional unresolved-binding badge on a
node-channel row.

## Performance

- **Slider scrubs coalesce to one preview per burst** — every morph row routes through `makeCoalescer`
  and the `dragActive` bracket, so a drag emits one `set-morph-weights` per edit-burst, not one per
  pointer tick (the serialized-`CONTROL_IO` rule: exactly one round-trip outstanding).
- **No new poll.** Live morph values ride the existing `animationVersion`-gated ~6 Hz reconcile via
  `AnimationStateResult.morphWeights`; the timeline footer channels ride the same fetch. Morph target
  **names** are fetched once per selection (static), cached in a ref.
- **Timeline stays imperative.** The dock canvas (`timelineCanvas.ts`) is untouched — the playhead and
  bar still draw without React re-render. The new channel UI is React via the Inspector's
  `fieldRenderer`/`SliderField` path and the footer string; it never enters the canvas hot loop.
- **Per-row stability** for a large morph list (a facial rig with ~50–100 targets): each slider row
  subscribes to its own derived primitive (`animationState?.morphWeights?.[i]`) rather than the whole
  vector, so dragging one target does not re-render the others (the "two row renders, not N" rule in
  `store.ts`). Memoize the row component and stable handlers.

## Control commands

None new — all bound in Phase 7. This phase **binds** `set-morph-weights` (optimistic morph edits) and
reads `get-morph-weights` (target names), `AnimationStateResult.morphWeights` (live values),
`AnimationClipDto.channels` (footer breakdown), and optionally `list-clip-bindings` (unresolved-binding
badge). Node-TRS uses the existing transport commands unchanged.

## Docs

The `timeline.md` update (channel drill-down + Inspector sliders) lands in **Phase 9**, per the plan
split — do not write docs in this phase.

## Tests

Run inside the toolbox / editor dir:

- `cd editor && bun install && bun run check` (regenerates `@saffron/protocol` and typechecks — must be
  clean against the Phase-7 DTOs).
- `cd editor && bun run lint && bun run format` (oxlint + oxfmt) — clean.

Behavior to assert (the engine-driving e2e fixtures land in Phase 9 over the control plane; this phase's
acceptance is the editor behavior):

- **A morph mesh** (e.g. `AnimatedMorphCube`): the Inspector shows the `AnimationChannels` section with
  `0..1` weight sliders named per target; dragging a slider calls `set-morph-weights` (one per burst)
  and the viewport deforms; during `play-animation` the slider tracks the live value from `morphWeights`.
- **A `BoxAnimated` container:** selecting it lights the timeline + transport (via `isAnimatable`), and
  play/seek/loop drive node animation through the existing transport commands; the dock timeline shows
  exactly **one** clip bar.
- The dock timeline still shows exactly one clip bar for a morph mesh and for a skinned rig — channels
  are footer metadata, never extra lanes.

## Acceptance criteria

- One clip = one timeline bar; channels are nested drill-down metadata (footer breakdown + Inspector
  list), no extra lanes.
- The Inspector `AnimationChannels` (Morph) section shows `0..1` morph sliders with live values, backed
  by `set-morph-weights`, with optimistic update and coalesced (one-per-burst) scrubs.
- `isAnimatable` (one predicate, replacing `isRiggedEntity`) lights the timeline for node-TRS containers
  and morph meshes; `Morph` is in `NON_ADDABLE` (and import-managed, matching `SkinnedMesh`).
- Node-TRS plays through the existing transport commands (no new playback verb, no parallel UI).
- `bun run check` + `bun run lint` + `bun run format` clean.

## Milestone gate

After the change: `bun run check` clean, then per AGENTS.md run `make prepare-for-commit` (format +
lint) and fix every warning this change raises (editor side: oxlint/oxfmt; the engine is untouched this
phase). Leave the work unstaged.

## Risks

- **`channels` may be empty for the dock clip** (the global catalog convention). The footer breakdown
  must degrade gracefully to the existing `N tracks · N clips` line, never throw. Decide the single-clip
  channels source (state-bound clip vs an asset-model fetch) up front; do not add a second timer.
- **Optimistic vs live during playback:** when not dragging, the slider must follow `morphWeights`
  (live); when dragging, the optimistic overlay + `dragActive` gate must win. Mirror the Inspector's
  field gesture handling (`fieldGesture`, `pendingClip` optimism) exactly so the two don't fight or
  flicker against the poll.
- **Large morph lists** (facial rigs): without per-row subscription, a single scrub re-renders the whole
  list. Apply the memo + own-primitive-subscription rule.
- **Gate exactness:** the 3-way `isAnimatable` must not light the timeline for non-animatable entities —
  it is component-presence only, kept a single source (no second gate).
- **Section order:** add `AnimationChannels`/`Morph` via `componentOrder` so it renders in a stable
  place, not interleaved by component-map iteration order.
- **Generated protocol drift:** never hand-edit `sa-types.ts`. If a field is missing, it is a Phase-7
  gap — regenerate from the DTOs.
