# Editor, docs, e2e, and the perf budget

**Status:** COMPLETED (editor + docs gated green; e2e fixtures + drivers authored — see the e2e note)
**Depends on:** Phase 1 (track model), Phase 2 (`MorphComponent`, `.smesh` v3), Phase 3 (runtime evaluator + node bindings), Phase 4 (morph graph pass), Phase 5 (motion + RT), Phase 6 (`AnimationChannelDto`, morph/binding commands)

## Progress — DONE

- **Editor (`bun run check` green, `bun run lint` 0 errors):**
  - `TimelinePanel.tsx`: `isRiggedEntity` → `isAnimatable` (+ `"Morph"`); the lone caller moved.
  - `control/client.ts`: `setMorphWeights` / `getMorphWeights` / `listClipBindings` wrappers; the two
    result types added to the `protocol/index.ts` re-export shim.
  - `InspectorPanel.tsx`: a **Morph** section — one `0..1` `SliderField` per target, labelled by the
    durable `MorphComponent.names`, writing through a dedicated `set-morph-weights` coalescer gated on
    `dragActive` (canonical `0..1`, no `/100`). `"Morph"` was already in `COMPONENT_ORDER` /
    `NON_ADDABLE` / `NON_REMOVABLE` (seeded in Phase 2).
  - `timelineCanvas.ts`: `TimelineClip.channels` carries `{label, times}` per channel; the clip bar
    draws a real keyframe tick at every `AnimationChannelDto.times[i]` (decision #19's real keyframe
    data). `TimelineSurface.tsx` populates the active clip's channels from `AnimationClipDto.channels`.
    `ClipList.tsx` reads `channels.length` (the removed `tracks` count).
- **Docs (`hugo` builds clean):** new `animation/morph-targets.md` + `animation/node-trs-animation.md`;
  the `animation/_index.md` hub intro widened to the three clip sources + two new rows + the timeline row
  reworded off the placeholder framing; `animation-data-model.md`'s `AnimTrack` block updated to the
  generalized model; `smesh-format.md` → v3 + `MESH_FLAG_SKIN`/`MESH_FLAG_MORPH` + `morph_offset` + the
  morph section + the one `save_mesh_to_buffer`; `sanim-format.md` → v2 24-byte record with
  `target`/`morph_count`.
- **e2e fixtures + drivers authored:** `gen_box_animated.py`/`gen_animated_morph_cube.py`/
  `gen_morph_stress.py` (run; the three `.gltf` committed) + `node-trs.test.ts` / `morph.test.ts` /
  `morph-stress.test.ts`, each structurally mirroring the known-good drivers (boot → import → drive →
  `expect(engine.validationErrors()).toEqual([])`). The morph driver round-trips weights + asserts a
  silhouette change across weight 0 vs 1; node-trs asserts the forest is not baked + `list-clip-bindings`
  resolves the node channel; morph-stress asserts the `morph` pass is present in `pass-timings` with
  `gpuMs >= 0` (magnitude gated on `!softwareGpu`).

> **e2e RUN BLOCKED (environment, not code).** In this session the headless host boots and renders
> **validation-clean** (the `validationErrors()` assertions pass), but **every** control call times out at
> the harness's hardcoded 15 s — and this reproduces identically on the pre-existing known-good
> `control.test.ts` / `animation.test.ts`, so it is a session-wide control-plane/llvmpipe contention
> wedge, not a defect in the new fixtures or drivers (whose `import-model` steps complete before the
> wedge). The drivers should be re-run on a non-contended toolbox via `just e2e`.

## Goal

Close the feature in the surfaces a user touches: the editor gains a Timeline that draws **real**
per-channel keyframe strips under each clip bar and an Inspector morph-weight section; the docs grow
two new explanation pages and the two format pages move to the new versions; e2e adds three fixtures
(node-TRS, morph round-trip, morph perf) that drive the headless host over the control plane and assert
validation-clean logs. This phase is the verification cap — it spends no design budget, it proves the
preceding six against the running engine and renders the data they expose.

## Design

### Editor: one animatable gate

`isRiggedEntity` in `panels/TimelinePanel.tsx` currently admits only `AnimationPlayer || SkinnedMesh`.
That misses a morph-only entity (a static mesh with a `MorphComponent` and a clip carrying weight
channels but no skin). It widens to **one** predicate `isAnimatable(components)` returning
`"AnimationPlayer" in c || "SkinnedMesh" in c || "Morph" in c`. The name change is the whole of it —
there is exactly one caller (`target.enabled`), and the morph case is the only new admit. No second
gate survives.

### Editor: Timeline real per-channel strips (decision #19)

The Timeline today draws **one clip = one bar** and stops there (`TimelineClip` in
`lib/timelineCanvas.ts`, fed from `TimelineSurface.tsx`). Decision #19 forbids the structural
placeholder this phase's blueprint draft hedged toward: the channel rows must be **real keyframe
strips drawn from `AnimationChannelDto` keyframe data**, nested under the one clip bar. One clip stays
one bar; each `AnimationChannelDto` of that clip is a real strip beneath it, with a tick at each
keyframe time from the channel's `times` array (the per-channel keyframe data Phase 6 put on the wire).

The data flow: `TimelineSurface` already holds the selected clip's `AnimationClipDto` (via
`target.clips` + the polled `AnimationStateResult.clip`). Phase 6 added `AnimationClipDto.channels:
Vec<AnimationChannelDto>` (replacing the old `tracks` count — decision #1). Each channel carries its
`kind: String` (`"node-translation"|"node-rotation"|"node-scale"|"morph-weights"|"bone"`), its `label`
(decision #20, computed engine-side), and its `times: Vec<f32>` keyframe stamps. `timelineCanvas.ts`
extends its `TimelineClip` row model so a clip owns a `Vec<Channel>` where each channel is a strip with
its own lane and a tick per `times[i]` mapped through the existing seconds→x projection. The strips
nest visually under the clip bar (indented header column, `TRACK_HEADER_WIDTH`), expanded/collapsed by
a chevron on the clip bar — `TimelineSurface` owns the open/closed flag and calls `listClipBindings`
(Phase 6) to resolve labels for node channels (so a broken binding shows the raw glTF name as the
fallback — decision #20, which doubles as the "binding broken" signal).

Channel labels are **hybrid** (decision #20): a node-TRS channel's label is the resolved entity name
when its binding resolves, falling back to the raw glTF node name when it does not; a morph-weights
channel's label is the raw glTF target name. This resolution is computed **engine-side** and delivered
in `AnimationChannelDto.label` (Phase 6), so the editor renders the label verbatim — it does not
re-derive the hybrid rule in TypeScript. `listClipBindings` is the editor's read of the
resolved-vs-raw state for the chevron drill-down header, not a second labelling path.

### Editor: Inspector morph-weight sliders

`InspectorPanel.tsx` renders a section per component in `COMPONENT_ORDER`. A new
`AnimationChannels`-style **Morph** section renders one **0..1 slider per morph target**, labelled by
the durable `MorphComponent.names[k]` (Phase 2), bound by name. Weights are **canonical 0..1
end-to-end — never 0..100, no `/100` in the slider** (cross-cutting decision #9 in `decisions.md`).
Each slider writes through `setMorphWeights` (the new wrapper), coalesced through the existing
`makeCoalescer` path and gated on `store.dragActive` exactly like the Transform/field scrubs, so a drag
emits one coalesced control call per edit-burst, not one per tick. The poll-clobber guard is the same
`dragActive` flag the Inspector already flips for number scrubs.

`Morph` joins `COMPONENT_ORDER` (after `AnimationPlayer`, before `Camera`) so the section orders
deterministically, and joins both `NON_ADDABLE` and `NON_REMOVABLE` in `InspectorPanel.tsx` — the
component is import-managed identity (decision #11), its weight-vector length must match the mesh's
target count, so a user can neither add nor remove it (parallel to `SkinnedMesh`).

### Editor: three control-client wrappers

`control/client.ts` gains `setMorphWeights`, `getMorphWeights`, `listClipBindings` — thin typed
wrappers over the Phase 6 commands, matching the existing `playAnimation`/`seekAnimation` shape
(one `call(...)` each, params typed by the generated DTOs).

### Docs (same change as the concept)

Two new explanation pages plus the two format pages move to the new versions, in the same change that
ships the editor surface — the AGENTS.md keep-docs-current rule. House style throughout: front-matter
`title` equals the body `# H1` (a sentence-case noun phrase), a slim `What | File | Symbols` table
(symbols, not line numbers), `mermaid` for flow, KaTeX for the weight-sum math, GitHub-alert callouts,
and the prose run through the humanizer pass. The `animation/_index.md` hub gains a row per new page;
its intro paragraph widens from "skeletal animation deforms a rigged mesh" to cover node-TRS and morph
as the other two clip-driven sources.

### e2e fixtures + tests

Three fixtures generated by `gen_*.py` (the `gen_leg.py` pattern), each with a `*.test.ts` driver that
boots a headless host and drives it over the control plane (typed via `@saffron/protocol`), asserting
responses and a validation-clean log (`expect(engine.validationErrors()).toEqual([])`):

- **`BoxAnimated`** — node-TRS, a forest of >1 node with a translate/rotate clip. The driver asserts
  the import did **not** bake the node transform into vertices (the spawned forest carries live
  `Transform`s; playing the clip moves the entity), and `list-clip-bindings` resolves the node binding
  to the spawned entity name.
- **`AnimatedMorphCube`** — a cube with one morph target and a weight clip. The driver round-trips
  `set-morph-weights`/`get-morph-weights` (a written 0..1 vector reads back identically) and asserts
  the deform actually moves geometry (a non-zero deformed-buffer delta via the render-stats / capture
  surface), proving the Phase 4 kernel ran.
- **`MorphStressTest`** — a high-target-count mesh for the perf budget below.

### Perf budget

The morph graph pass auto-times through the existing per-pass profiler: the render graph wraps each
pass body in `RgTimestamps::begin_scope`/`end_scope` (`rendering/src/render_graph.rs`, around
`profiler.rs:RgTimestamps`) — there is **no** `GpuScope` RAII type to add; the morph pass gets timed
for free by being a named graph pass. `morph-stress.test.ts` calls `pass-timings` (the
`RenderPassTimingsDto` surface `perf.test.ts` already exercises) and asserts the morph pass is
**present** by name and `gpuMs >= 0` unconditionally; the magnitude bound is gated on
`!softwareGpu` / `caps.timestampsSupported` (llvmpipe times are not a budget), mirroring the existing
`pass-timings` test's `timestampsSupported` gate.

## Changes

| What | Location (file:symbol) | Kind |
|---|---|---|
| `isRiggedEntity`→`isAnimatable` (+`"Morph"`) | `editor/src/panels/TimelinePanel.tsx:isRiggedEntity` | modify |
| Per-channel strip rows nested under the clip bar | `editor/src/lib/timelineCanvas.ts:TimelineClip` (+ a `channels` strip model + tick draw) | modify |
| Chevron drill-down + `listClipBindings` label resolve | `editor/src/components/timeline/TimelineSurface.tsx` | modify |
| `TimelineTarget` carries the selected clip's channels | `editor/src/components/timeline/shared.ts:TimelineTarget` | modify |
| Drop the "one clip → one track row in v1 … defer" comment | `editor/src/components/timeline/shared.ts:TRACK_ACCENT` doc | modify |
| Morph slider section (0..1, name-bound, coalesced) | `editor/src/panels/InspectorPanel.tsx` (new section reusing `makeCoalescer`/`dragActive`) | modify |
| `"Morph"` in `COMPONENT_ORDER` | `editor/src/lib/componentOrder.ts:COMPONENT_ORDER` | modify |
| `"Morph"` in `NON_ADDABLE` + `NON_REMOVABLE` | `editor/src/panels/InspectorPanel.tsx:NON_ADDABLE` / `NON_REMOVABLE` | modify |
| `setMorphWeights`/`getMorphWeights`/`listClipBindings` wrappers | `editor/src/control/client.ts` | modify |
| `morph-targets.md` explanation | `docs/content/explanations/animation/morph-targets.md` | new-file |
| `node-trs-animation.md` explanation | `docs/content/explanations/animation/node-trs-animation.md` | new-file |
| Hub intro + two new rows | `docs/content/explanations/animation/_index.md` | modify |
| `.smesh` page → v3 + SKIN/MORPH flags + `morph_offset` + `load_mesh_morph_from_bytes` | `docs/content/explanations/geometry-and-assets/smesh-format.md` | modify |
| `.sanim` page → v2 24B record + `AnimTarget`/`AnimPath::Weights` | `docs/content/explanations/geometry-and-assets/sanim-format.md` | modify |
| `BoxAnimated` fixture + generator | `tests/e2e/fixtures/gen_box_animated.py`, `tests/e2e/fixtures/BoxAnimated.gltf` | new-file |
| `AnimatedMorphCube` fixture + generator | `tests/e2e/fixtures/gen_animated_morph_cube.py`, `tests/e2e/fixtures/AnimatedMorphCube.gltf` | new-file |
| `MorphStressTest` fixture + generator | `tests/e2e/fixtures/gen_morph_stress.py`, `tests/e2e/fixtures/MorphStressTest.gltf` | new-file |
| Node-TRS driver (forest-not-baked, binding resolve) | `tests/e2e/node-trs.test.ts` | new-file |
| Morph driver (weight round-trip + deform delta) | `tests/e2e/morph.test.ts` | new-file |
| Morph-stress perf driver (pass presence + `gpuMs>=0`) | `tests/e2e/morph-stress.test.ts` | new-file |

## New artifacts

- `isAnimatable` (the single Timeline gate, replacing `isRiggedEntity`).
- `editor/src/control/client.ts` wrappers `setMorphWeights` / `getMorphWeights` / `listClipBindings`.
- The Inspector **Morph** section (0..1 name-bound sliders).
- `docs/.../animation/morph-targets.md`, `docs/.../animation/node-trs-animation.md`.
- `tests/e2e/fixtures/{BoxAnimated,AnimatedMorphCube,MorphStressTest}.gltf` + their `gen_*.py`.
- `tests/e2e/{node-trs,morph,morph-stress}.test.ts`.

## NO-LEGACY cutover (this change)

- **`isRiggedEntity` is deleted, not aliased.** It becomes `isAnimatable`; the lone caller
  (`target.enabled` in `TimelinePanel.tsx`) moves with it. No `isRiggedEntity` identifier survives.
- **The "one clip → one track row in v1 … per-channel/per-bone rows … defer" framing is deleted.**
  The `TRACK_ACCENT` doc comment in `shared.ts` and the `lib/timelineCanvas.ts` clip-bar-only render
  are replaced by the real per-channel strip renderer (decision #19). No structural-placeholder lane
  remains beside the real one.
- **`AnimationClipDto.tracks` has no editor reader after this change.** Phase 6 replaced it with
  `channels`; this phase makes the editor consume `channels` (the strip count is `channels.length`),
  so the old `tracks`-count read in `TimelineSurface.tsx` is removed in the same change (decision #1).
- **The docs format pages drop the old version language.** `smesh-format.md`'s two-version
  (`MESH_FORMAT_VERSION` + `_SKINNED`) description and `sanim-format.md`'s v1 20B record description are
  rewritten to v3-flags and v2-24B respectively — the superseded version prose does not survive next to
  the new one.
- **The `timeline.md` hub row's "deferred authoring mode … track rows, clip bars" wording** updates to
  the per-channel strip reality (the row's "Covers" cell loses the placeholder framing).

## Test gate

- Editor gate: `cd editor && bun run check` (gen-protocol + `tsc --noEmit` — typechecks the new
  wrappers, the `AnimationChannelDto` strip model, the widened `isAnimatable`), `bun run lint` (oxlint),
  `bun run format` (oxfmt).
- e2e: `just e2e` runs `node-trs.test.ts` (forest-not-baked + binding resolve), `morph.test.ts`
  (`set-morph-weights`/`get-morph-weights` 0..1 round-trip + non-zero deform delta), and
  `morph-stress.test.ts` (morph pass present in `pass-timings`, `gpuMs >= 0` unconditional, magnitude
  asserted only when `caps.timestampsSupported && !softwareGpu`); each ends in
  `expect(engine.validationErrors()).toEqual([])`.
- Docs: `cd docs && hugo` builds clean (the two new pages + hub + two format pages), front-matter
  `title` equals each body `# H1`.
- Milestone gate: `just engine` then `just prepare-for-commit` (cargo fmt + oxfmt; `cargo clippy
  --workspace -- -D warnings` + oxlint), then the full reproducible gate **`just check`** (workspace
  build + shaders → present-only smoke → control-schema contract → frontend bun build → e2e) — this is
  the verification cap for the whole feature.
