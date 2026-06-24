# Control plane: morph and binding commands, channel metadata, Lua bridge

**Status:** COMPLETED
**Depends on:** Phase 2 (`MorphComponent` / `MorphWeightOverride`), Phase 3 (the per-target runtime
write seams and the node-binding cache)

## Progress — DONE (workspace build + clippy clean; protocol/xtask/script/runtime tests green)

- **Counts re-derived from the live tree (the coordination note in flight).** assets-connectors had
  already landed 2 DTOs since this plan's baseline, so the real pre-edit counts were `ts_decls = 259`,
  `struct_fragments = 240` (not 257/238). After this plan's `+6` DTOs the asserts are now
  **`COMMANDS = 158`, `ts_decls = 265`, `struct_fragments = 246`, animation domain = 16**.
- **DTOs** (`protocol/src/dto.rs`): new `AnimationChannelDto` (kind/label/target_name/times/width/values),
  `SetMorphWeightsParams`, `GetMorphWeightsParams`, `MorphWeightsResult` (shared set/get),
  `ListClipBindingsParams`, `ClipBindingsResult`. `AnimationClipDto.tracks: i32` → `channels:
  Vec<AnimationChannelDto>` (deleted, not kept beside). `AnimationStateResult` + always-present
  `morph_weights: Vec<f32>`.
- **Command table** (`protocol/src/command.rs`): `set-morph-weights`/`get-morph-weights`/
  `list-clip-bindings` after `set-foot-ik`; `DTO_TYPE_NAMES` + 6; `animation_domain()` + 3 (last →
  `list-clip-bindings`); module docs + partition comment 155→158; the three new commands are
  `COMMAND_SKIPS` ("needs an imported morph mesh — covered in make e2e", mirroring `set-kinematic-bones`)
  since the default fixture scene has no morph mesh.
- **codegen.rs**: `decl_entry!`/`frag_entry!` rows for the 6 DTOs; count asserts 265/246.
- **control** (`commands_animation.rs`): `channel_kind`/`channel_width`/`channels_of` mapper (path/target
  → wire kind, `values.len() == times.len() * width`); `find_named_in_forest` (scoped pre-order name
  walk, the control twin of the runtime binding walk); `morph_weights_of` (override-or-component +
  durable names); `morph_entity`; `state_of` now reports `morph_weights`; `list-clips` loads each clip
  for real channels (raw target labels); the 3 commands registered. `commands_asset.rs::container_clips`
  loads each sub-asset clip for real channels (the `get-asset-model` path). Unit test
  `morph_weight_commands_round_trip_and_reject_length_mismatch`.
- **Lua bridge**: `ScriptHostBridge::set_morph_weights` + `NoopBridge` no-op + `Entity:set_morph_weights`
  (entity.rs Rust method + `add_method`, bindings.rs `binding!` row + the binding test list);
  `RuntimeScriptBridge::set_morph_weights` writes the override-or-component on the play scene (no-op on a
  dead lookup / length mismatch). The recording test bridge gained the method + a `Call` variant.
- **Artifacts regenerated** (`xtask gen-protocol`): `sa-types.ts`, `openrpc.generated.json`,
  `command-manifest.generated.json`, `sa.generated.luau` — all four round-trip byte-identical
  (`cargo test -p xtask` green).
- **Pre-existing failure, NOT this plan's:** `commands_asset::tests::asset_commands_register_in_manifest_order`
  fails because a prior landed commit (`186af6fa`, assets-connectors) added the `export-app` command to
  the table + registration but left this test's hardcoded `FROZEN` list stale. Per the concurrent-work
  rule it is the connectors agent's list to reconcile; better-animations does not touch it.

## Goal

Expose the two new capabilities over the JSON control plane and to Luau, on the existing single
animation code path. A clip's wire shape carries **real per-channel keyframe data** so the editor draws
true per-channel strips (decision #19); the animation state always reports live morph weights (decision
#8); three new commands read/write morph weights and list a clip's resolved bindings; node-TRS playback
reuses the one `play/seek/loop/state` path — there is **no** new playback verb. The `ScriptHostBridge`
gains one `set_morph_weights` method, auto-exposed to Luau as `Entity:set_morph_weights`. The four
generated protocol artifacts and the live-vs-schema contract test move in the same change.

## Design

### Channel metadata on the wire (decision #19, #8, #17, #20)

`AnimationClipDto` today is a four-field summary (`id`, `name`, `duration`, `tracks: i32`). The
`tracks` count is **replaced** by a `channels: Vec<AnimationChannelDto>` (decision #1) — one entry per
track in the clip, each carrying enough to render a real keyframe strip. `AnimationChannelDto` is:

```rust
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema, TS)]
#[serde(rename_all = "camelCase")]
#[ts(export)]
pub struct AnimationChannelDto {
    /// `"node-translation" | "node-rotation" | "node-scale" | "morph-weights" | "bone"` —
    /// a plain wire string (decision #8/#17), no enum DTO row; serde/ts-rs handle it natively.
    pub kind: String,
    /// The channel's display label: the resolved entity name for a node/bone binding (raw glTF
    /// node name when the binding is unresolved — which doubles as the broken-binding signal),
    /// and the raw glTF target name for a morph-weights channel (decision #20).
    pub label: String,
    /// The raw glTF binding key (node name, or morph target name) — durable, what the runtime
    /// binds on. Distinct from `label` so the editor can show the friendly name yet key on this.
    pub target_name: String,
    /// The keyframe sample times in seconds, ascending — the strip's tick positions.
    pub times: Vec<f32>,
    /// The number of value components per keyframe, so `values.len() == times.len() * width`:
    /// `3` for translation/scale, `4` for a rotation quaternion, `morph_count` for a
    /// `morph-weights` channel. The editor draws one strip per channel keyed on `times`,
    /// independent of `width`.
    pub width: i32,
    /// The per-keyframe values, row-major `times.len() * width`. Translation/scale rows are
    /// `xyz`, rotation rows are quaternion `xyzw`, morph rows are the N weights. The editor
    /// renders value overlays from these where it draws them.
    pub values: Vec<f32>,
}
```

`kind` is a plain `String`, not an enum (decisions #8/#17): the set is open enough that an enum DTO row
would be churn for no payoff, and `serde`/`ts-rs` map a `String` field directly. The channel list is
built where clips are read into a `AnimationClipDto` — `commands_animation.rs:register_animation_commands`
`list-clips` arm (file:symbol `engine/crates/control/src/commands_animation.rs:register_animation_commands`)
and the asset-model path (`AssetModelResult.clips`). Both read the loaded `AnimClip` (its `tracks:
Vec<AnimTrack>` from Phase 1, each with `target`, `path`, `target_name`, `times`, `values`,
`morph_count`) and map one channel per track:

- `kind` from `(target, path)`: `AnimTarget::Bone` → `"bone"`; `AnimTarget::Node` with `AnimPath::T/R/S`
  → `"node-translation"/"node-rotation"/"node-scale"`; `AnimPath::Weights` → `"morph-weights"`.
- `width` = `morph_count` for a weights channel, else the path's component count (`3` for
  translation/scale, `4` for a rotation quaternion); the raw `xyz`/`xyzw` go into `values` row-major, so
  `values.len() == times.len() * width` holds for every channel kind. The editor draws one strip per
  channel keyed on `times`, not on `width`.
- `label` resolves through the same name→entity lookup the binding cache uses (decision #20): for a
  bound node/bone, the resolved entity name; on a miss, the raw `target_name` (the unresolved label is
  the broken-binding tell). For morph, the raw target name from `MorphComponent.names` / the clip's
  `target_name`.

`AnimationStateResult` gains `morph_weights: Vec<f32>` — **always present**, empty when the target has
no `MorphComponent` (decision #8; no `Option`, no `skip_serializing_if`). It is read in `state_of`
(`engine/crates/control/src/commands_animation.rs:state_of`) from the live override-or-component weights
(runtime `MorphWeightOverride` if present, else the durable `MorphComponent.weights`), so a running
morph reports its current weights and a stopped one reverts to the component's rest weights. The
existing `animation_version` carries the change through the editor's poll exactly as it does for the
playhead.

### Three new commands

All three register in `register_animation_commands`
(`engine/crates/control/src/commands_animation.rs:register_animation_commands`), taking the animation
domain from 13 to 16. They route through `player_entity` / `animatable_descendant` like the existing
arms, then through the scene's `MorphComponent` / `MorphWeightOverride`:

| Command | Params → Result | Behaviour |
|---|---|---|
| `set-morph-weights` | `SetMorphWeightsParams { entity, weights: Vec<f32> }` → `MorphWeightsResult` | Resolves the mesh-bearing entity, writes a `MorphWeightOverride` (runtime) when a preview is live else the durable `MorphComponent.weights`; length must equal the component's target count (`Err` on mismatch); bumps `animation_version`. |
| `get-morph-weights` | `GetMorphWeightsParams { entity }` → `MorphWeightsResult { weights, names }` | Reads the live weights (override-or-component) and the durable target names. |
| `list-clip-bindings` | `ListClipBindingsParams { entity, clip }` → `ClipBindingsResult { channels: Vec<AnimationChannelDto> }` | Resolves the clip against the entity's forest, runs the same channel mapper as `list-clips` but with `label`/binding resolution against the *live* forest (so an unresolved channel surfaces as a broken binding). |

`MorphWeightsResult { weights: Vec<f32>, names: Vec<String> }` is shared by `set-`/`get-morph-weights`
(one result type, not two). Node-TRS playback adds **no** command: `play-animation`/`seek-animation`/
`set-animation-loop`/`get-animation-state` already drive any `AnimationPlayer`, and Phase 3 made the
node-forest player live on the container root with the same `AnimationPlayer` component — so the
existing verbs hit node players unchanged.

### Lua bridge

`ScriptHostBridge` (`engine/crates/script/src/bridge.rs:ScriptHostBridge`) gains:

```rust
/// Set the morph-target weights of `entity`'s morph mesh (canonical 0..1). A length
/// mismatch or a non-morph entity is a no-op on the host side.
fn set_morph_weights(&self, entity: Uuid, weights: &[f32]);
```

with a no-op default in `NoopBridge` (`engine/crates/script/src/bridge.rs:NoopBridge`), mirroring
`set_velocity`/`set_ragdoll_blend`. The host implementation lands in
`engine/crates/runtime/src/bridge.rs:RuntimeScriptBridge` — it borrows the play scene and writes the
`MorphWeightOverride` on the resolved entity (the same write seam the control command uses), guarding a
`None` scene / dead lookup as a no-op like the other methods. The binding table entry in
`engine/crates/script/src/bindings.rs` (a `binding!("set_morph_weights", Some("Entity"),
BindingKind::Method, [("weights": "number[]")], None, "...")`, twin of the `set_velocity` row) and the
runtime dispatch closure in `engine/crates/script/src/runtime.rs` (a `session::with_bridge(|bridge|
bridge.set_morph_weights(entity, &weights))` arm, twin of the `set_velocity` closure) complete the
Luau reach as `Entity:set_morph_weights({...})`. The Luau type stub is **auto-generated** by
`engine/xtask/src/protocol/luau.rs` from the binding table — no hand-written stub.

Weights are canonical `0..1` end-to-end (cross-cutting decision #9): the wire, the slider (Phase 7), and
the Luau number array are all `0..1`; there is no `/100` anywhere.

## Changes

| What | Location (file:symbol) | Kind |
|---|---|---|
| `AnimationChannelDto` wire DTO | `engine/crates/protocol/src/dto.rs` | new |
| `SetMorphWeightsParams` | `engine/crates/protocol/src/dto.rs` | new |
| `GetMorphWeightsParams` | `engine/crates/protocol/src/dto.rs` | new |
| `MorphWeightsResult` (shared by set/get) | `engine/crates/protocol/src/dto.rs` | new |
| `ListClipBindingsParams` | `engine/crates/protocol/src/dto.rs` | new |
| `ClipBindingsResult` | `engine/crates/protocol/src/dto.rs` | new |
| `AnimationClipDto.tracks: i32` → `channels: Vec<AnimationChannelDto>` | `engine/crates/protocol/src/dto.rs:AnimationClipDto` | modify |
| `AnimationStateResult` + `morph_weights: Vec<f32>` (always-present) | `engine/crates/protocol/src/dto.rs:AnimationStateResult` | modify |
| Register `set-morph-weights`/`get-morph-weights`/`list-clip-bindings`; 13→16; doc string | `engine/crates/control/src/commands_animation.rs:register_animation_commands` | modify |
| `list-clips` arm maps `channels` (was `tracks: entry.tracks`) | `engine/crates/control/src/commands_animation.rs:register_animation_commands` | modify |
| `state_of` populates `morph_weights` from override-or-component | `engine/crates/control/src/commands_animation.rs:state_of` | modify |
| Channel-mapper helper (`AnimClip` → `Vec<AnimationChannelDto>`, label/binding resolution) | `engine/crates/control/src/commands_animation.rs` | new |
| Asset-model `clips` map populates `channels` | `engine/crates/control/src/` (the `AssetModelResult.clips` builder) | modify |
| 6 `CommandSpec` rows (3 commands × params/result); summaries | `engine/crates/protocol/src/command.rs:COMMANDS` | modify |
| `155` → `158` everywhere: module docs, `table_holds_154_typed_commands_in_frozen_order`, the partition asserts | `engine/crates/protocol/src/command.rs` (module docs, `table_holds_154...`, the domain-partition test) | modify |
| `animation_domain()` 13→16 entries (`+ set-morph-weights, get-morph-weights, list-clip-bindings`); the `animation 13` comment → `16`; `animation.last()` assert | `engine/crates/protocol/src/command.rs:animation_domain` and its partition test | modify |
| `COMMAND_FIXTURES` / `COMMAND_SKIPS` entries for the 3 commands; `FIXTURES.len()+SKIPS.len()==158` | `engine/crates/protocol/src/command.rs:COMMAND_FIXTURES`, `:COMMAND_SKIPS`, the every-command-has-one test | modify |
| `DTO_TYPE_NAMES` + 6 new type names | `engine/crates/protocol/src/command.rs:DTO_TYPE_NAMES` | modify |
| `decl_entry!` rows for the 6 new DTOs | `engine/crates/protocol/src/codegen.rs:ts_decls` | modify |
| `frag_entry!` rows for the 6 new DTOs | `engine/crates/protocol/src/codegen.rs:struct_fragments` | modify |
| `ts_decls().len()` 257 → 263; `struct_fragments().len()` 238 → 244 | `engine/crates/protocol/src/codegen.rs` (`ts_decls_cover_the_full_dto_universe`, `struct_fragments_cover_every_openrpc_struct`) | modify |
| `set_morph_weights(&self, entity: Uuid, weights: &[f32])` trait method | `engine/crates/script/src/bridge.rs:ScriptHostBridge` | modify |
| `NoopBridge::set_morph_weights` no-op | `engine/crates/script/src/bridge.rs:NoopBridge` | modify |
| `RuntimeScriptBridge::set_morph_weights` (writes the `MorphWeightOverride` on the play scene) | `engine/crates/runtime/src/bridge.rs:RuntimeScriptBridge` | modify |
| `binding!("set_morph_weights", …)` table row | `engine/crates/script/src/bindings.rs` | modify |
| `session::with_bridge(\|b\| b.set_morph_weights(…))` dispatch closure | `engine/crates/script/src/runtime.rs` | modify |
| Three contract-test fixture cases (set/get/list-clip-bindings) | `tools/check-control-schema/check.ts` | modify |
| Regenerate `sa-types.ts`, `luau_defs`, OpenRPC, command-manifest | `cargo run -p xtask -- gen-protocol` | regen |

## New artifacts

- DTOs: `AnimationChannelDto`, `SetMorphWeightsParams`, `GetMorphWeightsParams`, `MorphWeightsResult`,
  `ListClipBindingsParams`, `ClipBindingsResult`.
- Commands: `set-morph-weights`, `get-morph-weights`, `list-clip-bindings` (animation domain → 16).
- Bridge: `ScriptHostBridge::set_morph_weights`; the auto-generated `Entity:set_morph_weights` Luau stub.
- Regenerated wire artifacts: `editor/src/lib/protocol/sa-types.ts`, the Luau defs, the OpenRPC JSON,
  and the command-manifest JSON (all four via `xtask gen-protocol`).

## NO-LEGACY cutover

In this same change:

- **`AnimationClipDto.tracks` is deleted, not kept beside `channels`.** Every reader moves: the
  `list-clips` arm and the asset-model `clips` builder stop reading `entry.tracks` and build
  `channels`; the editor's `AnimationClipDto` consumers (Timeline, Phase 7) read `channels.len()` /
  `channels` instead of `tracks`. No duplicate count field survives (decision #1).
- **No second playback verb for node-TRS.** Node players are driven by the existing `play-animation` /
  `seek-animation` / `set-animation-loop` / `get-animation-state` — adding a `play-node-animation` (or
  any parallel command) is forbidden; the one path is generalized in Phase 3 and these commands already
  reach it.
- **`morph_weights` is never an `Option`.** The always-present `Vec<f32>` is the one representation;
  there is no `skip_serializing_if` variant to maintain (decision #8).
- The frozen-count tests are not loosened to "≥" — they are re-pinned to the exact new totals (158
  commands, 263 ts decls, 244 struct fragments). A stale `155`/`257`/`238` left anywhere fails the
  build, which is the intended tripwire.
- The four generated artifacts are regenerated and committed in this change; a drifted artifact fails
  `cargo test -p xtask` (`emit().sa_types == committed`, etc.), so the regen is part of "done."

## Test gate

- `cargo test -p saffron-protocol` — `table_holds_154_typed_commands_in_frozen_order` now asserts 158;
  the domain partition sums to 158 with `animation` at 16; every command has exactly one fixture/skip;
  `DTO_TYPE_NAMES` covers the 6 new types; `ts_decls().len()==263`, `struct_fragments().len()==244`.
- `cargo test -p xtask` — the four artifact round-trip asserts (`emit().sa_types`, `emit().luau_defs`,
  `emit().openrpc`, `emit().manifest` each `== committed`) pass against the regenerated files, proving
  the new DTOs and the `set_morph_weights` Luau stub round-trip byte-identically.
- `cargo test -p saffron-control` — a unit test that `set-morph-weights` then `get-morph-weights`
  round-trips a weight vector and that `get-animation-state` reports those weights in `morph_weights`;
  a length-mismatch returns `Err`; `list-clip-bindings` returns one channel per track with the
  broken-binding label on an unresolved node.
- `cargo test -p saffron-script -p saffron-runtime` — the bridge contract test still holds (no
  physics/animation dependency edge from `saffron-script`); a `RuntimeScriptBridge::set_morph_weights`
  unit test writes the override on a play scene and a no-scene call is a no-op.
- `just schema` — `tools/check-control-schema/check.ts` drives the live host for the three new
  commands and asserts the responses match the regenerated OpenRPC schema (the live-vs-schema contract
  test gates this phase).
- Milestone gate: `just engine` then `just prepare-for-commit` (format + clippy `-D warnings` + oxlint),
  fixing every warning this change raises.
