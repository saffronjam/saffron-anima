# Phase 7 — Control plane: morph + node-TRS commands + channel metadata

**Status:** NOT STARTED

**Depends on:** Phase 4 (node-TRS runtime state), Phase 5 (morph weights runtime state)

## Why

Morph weights and the clip's channel makeup are new drivable/inspectable engine state, so each gets a
control command (AGENTS.md: state worth driving gets one `registerCommand`). Node-TRS playback reuses
the existing player commands (it is the same `AnimationPlayerComponent`) — **no duplicate playback
commands** for node-TRS, per NO LEGACY. This phase also surfaces per-channel metadata so the editor
Timeline/Inspector (Phase 8) can drill into bone/node/morph channels without a parallel UI.

## Grounding

- DTO source of truth: `control_dto.cppm` (`AnimationClipDto:1457`, `AnimationStateResult:1563`,
  `PlayAnimationParams:1529`, `EntitySelector:26`). Regex-parsed by `tools/gen-control-dto/gen.ts` →
  C++ serde, `editor/src/protocol/sa-types.ts`, OpenRPC, manifest, Lua defs.
- Command registration: `control_commands_animation.cpp` `registerAnimationCommands` (`:208`),
  `playerOf`/`animatableDescendant` (`:133-143`), `stateOf` (`:188`), `animationVersion` bump
  (`:302` etc).
- Existing player commands (`play-animation`, `set-animation-playing`, `seek-animation`,
  `set-animation-loop`, `get-animation-state`) already drive `AnimationPlayerComponent` — they work
  unchanged for a node-TRS player on a container entity (it is the same component).
- `gen.ts` rules: every command needs a fixture/skip; new enums need `enumWireNames`+`tsType`+
  `jsonSchemaFor`; vectors/optionals supported; `WireUuid` decimal string; ids are strings end-to-end.

## Decisions (locked)

1. **`set-morph-weights` + `get-morph-weights` (new).**
   - `SetMorphWeightsParams { EntitySelector entity; std::vector<f32> weights; std::optional<i32>
     index; std::optional<f32> weight; }` — set the whole vector, or a single target by `index`+`weight`
     (mirrors `set-ragdoll`'s per-bone-or-uniform shape, `control_commands_physics.cpp`). Resolves the
     entity → its mesh descendant (a `morphableDescendant` helper paralleling `animatableDescendant`),
     writes `MorphComponent.weights` (the authored value; Edit preview is non-destructive — the
     animation override still wins when playing), bumps `animationVersion`.
   - `GetMorphWeightsParams { EntitySelector entity; }` → `MorphWeightsResult { std::vector<f32>
     weights; std::vector<std::string> names; i32 animationVersion; }` — current resolved weights +
     target names (from the mesh's morph targets).
2. **Clip channel metadata, additive (no breaking change).** Extend `AnimationClipDto` (`:1457`) — keep
   `i32 tracks` (count) but add `std::vector<AnimationChannelDto> channels;` where
   `AnimationChannelDto { std::string name; std::string kind; }`, `kind ∈ {"bone-trs","node-trs",
   "morph-weight"}`. Populated lazily (Phase 8's drill-down request), empty in the global `list-clips`
   catalog (the existing "0 when unknown" convention). `list-clips` (`control_commands_animation.cpp:229`)
   and `get-asset-model` (`AssetModelResult.clips`) fill `channels` for a single-clip query.
3. **`AnimationStateResult` exposes live morph values for the Inspector slider.** Add
   `std::optional<std::vector<f32>> morphWeights;` (the entity's current resolved weights) to
   `AnimationStateResult` (`:1563`) so the existing `animationVersion`-gated reconcile poll
   (`store.ts`) carries live morph values to the Inspector with no extra command — the channel-values
   exposure the frontend research recommends, on the existing clock.
4. **One playback path.** No `play-node-animation` / `play-morph-animation`. `play-animation` on the
   container drives node tracks; on the mesh drives bones + morph weights. The morph commands set
   *authored* weights only; *playback* of weights is the clip via `play-animation`. (NO LEGACY: exactly
   one way to play a clip.)
5. **`sa` CLI + Lua.** Add `sa set-morph-weights <entity> <w0> <w1> …` / `sa get-morph-weights <entity>`
   (positional, per the CLI arg-order rule). Add a Lua `sa.setMorphWeights(entity, {...})` binding
   (host-bound, like the animation bindings) so morphs are scriptable per the keep-current rule.

## Edits

- `control_dto.cppm`: `SetMorphWeightsParams`, `GetMorphWeightsParams`, `MorphWeightsResult`,
  `AnimationChannelDto`; extend `AnimationClipDto.channels`, `AnimationStateResult.morphWeights`.
- `control_commands_animation.cpp`: register `set-morph-weights`/`get-morph-weights`; `morphableDescendant`;
  fill `channels` in `list-clips` + the asset-model path; fill `morphWeights` in `stateOf`.
- `tools/gen-control-dto/gen.ts`: fixtures for the two new commands; regenerate all five artifacts.
- `editor/src/control/client.ts`: typed `setMorphWeights`/`getMorphWeights` wrappers.
- `tools/sa/source/main.cpp`: the two verbs.
- Lua binding (host) for `sa.setMorphWeights`.

## Verification

- `make engine`; `bun run check` (TS + OpenRPC); the control-schema contract test green; regenerated
  files committed.
- `sa set-morph-weights <cube> 1 0` deforms the cube; `sa get-morph-weights` reads it back.
- `play-animation` on a `BoxAnimated` container animates node transforms (existing command, no new one).
- e2e (Phase 9) asserts the wire for the new commands.

## Risks

- **`gen.ts` is regex-based:** new enums/vectors must be registered in all three maps or codegen
  mis-emits silently. `AnimationChannelDto.kind` is a plain string (not an enum) to avoid the enum-map
  triple-edit — simplest correct choice.
- **`AnimationStateResult` poll size:** `morphWeights` is optional and only sent for a morph-bearing
  entity, so the ~6 Hz poll stays small.
- **One playback command** must remain the only one — resist adding a morph-specific play verb under UI
  pressure; the clip is the single source.
