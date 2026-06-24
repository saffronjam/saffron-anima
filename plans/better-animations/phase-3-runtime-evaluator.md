# Runtime evaluator: one sampler, node binding, per-target write seams

**Status:** COMPLETED

## Progress

- **animation crate — DONE, green (37 tests).** `sample.rs`: factored the keyframe-locate core
  (`locate_keys`/`KeyBracket`) shared by `sample_track` and the new N-wide `sample_weights` (no slerp/
  normalize, holds seeded rest weights when empty); deleted the index-only `sample_clip` + its re-export
  (the `lib.rs` test rewritten onto a `sample_weights` case). `runtime.rs`: `tick_animation` queries
  `Option<&SkinnedMesh>` so skinless node-forest players tick; `RigKind {Skinned, Node}`; the transition/
  cross-fade core (`apply_transition`) + playhead advance (`advance_playback`) factored to take a generic
  `targets: &[Entity]` so node players get the FULL transition path (decision #12); `tick_skinned_rig`
  (unchanged behaviour, sampler gated on `AnimTarget::Bone`) and new `tick_node_rig` (bind node tracks by
  name → `Entity` via a first-match pre-order subtree walk scoped to the container root, cached in
  `node_bindings`, re-resolved on a stale handle; write `PoseOverride` on driven nodes and
  `MorphWeightOverride` from `sample_weights`); `clear_node_overrides` + `clear`/`prune_session` drop the
  bindings + runtime overrides on stop. New tests: node player writes PoseOverride + world reflects it;
  node rig ignores bone tracks (joint-index-coupling regression); weights → MorphWeightOverride + clears
  on stop; per-instance binding isolation (two same-named forests).
- **Verification:** `cargo test -p saffron-animation` 37 pass; `cargo clippy --all-targets` clean;
  `rustfmt --check` clean; full `cargo build --workspace` green (no external user of the deleted
  `sample_clip` export).

**Depends on:** Phase 1 (generalized `AnimTrack`/`AnimClip`, `AnimTarget`, `AnimPath::Weights`,
v2 `.sanim`, node forest + node-forest `AnimationPlayer` at the container root), Phase 2
(`MorphComponent` + the runtime-only `MorphWeightOverride`).

## Goal

Collapse to **one** clip sampler that evaluates a heterogeneous clip (bone-TRS, node-TRS, morph-weight
tracks) and routes each track's value to its write seam: bone `PoseOverride` on a skinned rig's bones,
node `PoseOverride` on a node-forest entity, and a runtime-only `MorphWeightOverride` for weight tracks.
`tick_animation` must drive node-forest players that have **no** `SkinnedMesh`, so skinless-but-animated
subtrees tick. Node tracks bind by durable node name → `Uuid` → `Entity` through a first-match pre-order
subtree walk **scoped to the player's forest**, cached and re-resolved on a stale handle. Node-forest
players get the **FULL** transition/cross-fade state generalized from the bone-handle-shaped path
(decision #12) — no skinned-only shortcut, no hard-sample.

## Design

### One sampler, one keyframe core

`sample_track` (`animation/src/sample.rs:sample_track`) already returns a `Vec4` (T/S in `.xyz`,
quaternion `xyzw` for rotation) over Step/Linear/CubicSpline with `dt`-scaled Hermite tangents. The
keyframe-locate + interpolation math (the `value_offset`/`read_vec4` closures, the
`partition_point` key search, the `h00..h11` Hermite weights) is factored out into a private
`KeyFrame` helper so a second sampler reuses it without copy.

`sample_weights(track, t, out: &mut [f32])` is the N-wide morph-weight sampler. It shares the same
keyframe-locate + Step/Linear/CubicSpline evaluation, but writes `track.morph_count` lanes per key
(not a fixed 3/4), and applies **no** slerp and **no** normalization — morph weights are independent
scalars. `out` is the per-instance weight vector; lanes beyond the clip's key data, and the empty-clip
case, leave `out` as the caller seeded it (rest weights) rather than zeroing. Linear lerps lanes,
CubicSpline runs the per-lane Hermite, Step holds the previous key.

`sample_clip` (`animation/src/sample.rs:sample_clip`) — the public index-only sampler that loops
`clip.tracks`, keys by `track.joint`, and writes `out.local[j]` — is **deleted**. It is a duplicate
code path: `runtime.rs::sample_clip_resolved` is the only sampler the runtime actually calls, and the
durable-name rebind belongs there. With Phase 1's field renames (`joint`→`index`, `joint_name`→
`target_name`) and the new `AnimTarget`, the surviving sampler keys differently per target, so there is
exactly one routing site.

### Per-target write-seam routing lives in `tick_rig`/`tick_animation`, not the sampler

The #1 hazard is the joint-index coupling in the sampler: a Node/Weights track fed to the joint-pose
sampler indexes `out.local[j]` and either panics or writes a garbage bone. The cutover **moves the
write-seam decision out of the index-keyed sampler** and up into `tick_rig`:

- The sampler produces values keyed by their `AnimTarget`/`AnimPath`; it never decides where they land.
- `tick_rig` (and the new node path) own the routing: `AnimTarget::Bone` + T/R/S → a bone `PoseOverride`;
  `AnimTarget::Node` + T/R/S → a node-entity `PoseOverride`; `AnimPath::Weights` → a
  `MorphWeightOverride`. The bone-index rebind is **gated on `AnimTarget::Bone`** so a Node/Weights track
  is never interpreted as a joint index.

`sample_clip_resolved` is rewritten so the bone branch only runs for `AnimTarget::Bone` tracks; Node and
Weights tracks are skipped there and handled by the node/weights producers below.

### `RigKind`: one driver, two rig shapes

`tick_animation` (`animation/src/runtime.rs:tick_animation`) currently queries
`(&AnimationPlayer, &SkinnedMesh, Option<&IdComponent>)` — a `SkinnedMesh` is **required**, so a
node-forest player with no skin never ticks. The query tuple makes the skin **optional**:
`(&AnimationPlayer, Option<&SkinnedMesh>, Option<&IdComponent>)`. The gathered `Rig` carries a
`kind: RigKind`:

```rust
enum RigKind {
    /// A skinned rig: bone handles drive a joint palette.
    Skinned { bone_handles: Vec<Entity>, joint_count: usize },
    /// A node-forest rig at the container root: tracks bind to forest entities by name.
    Node,
}
```

`tick_rig` branches on `kind`:

- **Skinned**: the existing path — seed rest from bone `Transform`s, sample bone-TRS tracks, apply
  transitions + foot-IK, write `PoseOverride` per bone, snapshot `last_pose`. Unchanged in behaviour,
  but the sampler now skips non-`Bone` tracks.
- **Node**: a sibling producer. The rest pose seeds from each bound node entity's `Transform`; bone/node
  sampler routing writes `PoseOverride` onto the **node entities** (decision #16: the player lives at the
  container root, so the binding walk is scoped to that root's subtree). Weight tracks write
  `MorphWeightOverride` onto the entity carrying the matching `MorphComponent`.

`update_world_transforms` already prefers `PoseOverride` over `Transform` in `local_matrix` for **any**
entity, so a node `PoseOverride` composes into the forest's world matrices with no scene-side change.

### Node binding by durable name → Uuid → Entity, cached, first-match pre-order

A node-TRS track's `target_name` is the durable glTF node name. Binding resolves it to a live `Entity`
through a **pre-order subtree name walk scoped to the player's forest root** — never
`Scene::find_entity_by_uuid` (`scene/src/scene.rs:find_entity_by_uuid`), which is an O(n) global scan
that crosses instances and would bind the wrong copy when two instances of the same model are in the
scene. The walk starts at the player's container-root entity and descends `Relationship.children`
(`scene/src/component.rs:Relationship`), matching `Name.name` first-match in pre-order. Forests legally
repeat node names; the import-side `name_to_index` is a last-write-wins `HashMap`, so the runtime walk
deliberately takes the **first** pre-order match to stay deterministic and self-consistent with that map.

The resolved bindings cache on the runtime:

```rust
// On AnimationRuntime:
node_bindings: HashMap<u64, Vec<Option<Entity>>>,
```

Keyed by the player entity's `IdComponent` uuid (the same `u64` key the `transitions`/`last_pose` maps
use), the `Vec` is parallel to the clip's node/weight tracks: `node_bindings[key][track_i]` is the bound
entity for that track, `None` while unresolved. Each tick, a binding is used if the handle is still
`scene.valid(...)`; on a **stale** handle (entity destroyed, or a reimport reordered the forest) the slot
re-resolves by the name walk and the cache slot is rewritten. A first resolution fills the slot; a stale
hit replaces it. This mirrors the existing durable-name rebind on the skinned side (`sample_clip_resolved`
re-resolving by `target_name` through `name_to_index`) — same intent, scoped to entities for the forest.

### FULL node-player transitions (decision #12)

The transition/cross-fade machinery (`TransitionState`, `transitions` map, `outgoing_at`, `pose_diff`/
`blend_joint`/`apply_delta`/`quintic_decay`/`smoothstep01`, the loop-blend seam, the
`AnimationPlayer.transition`/`transition_duration`/`transition_mode`/`prev_clip`/`loop_blend` fields) is
**generalized off bone handles onto node targets**. The current path is bone-handle-shaped:
`outgoing_at(scene, bone_handles, rest, i)` reads the i-th bone handle's `PoseOverride`. For the node
path the same logic reads the i-th **bound node entity's** `PoseOverride` instead. The
`TransitionState { outgoing, offset }` is per-driven-target (per node entity, indexed parallel to the
forest's driven set) exactly as it is per-bone today; `CrossFade` blends the frozen outgoing toward the
incoming with `smoothstep01`, `Inertialize` decays the captured `offset` with `quintic_decay`. Node
players get cross-fade, inertialization, **and** the Loop-wrap blend identically to skinned rigs. There
is no "node players hard-sample" branch — that would be the deferral principle #3 forbids.

To share `outgoing_at`/the freeze loop across both rig shapes without a skin-only signature, the helpers
take a per-target accessor (`outgoing_at` is reworked to a closure/index that yields "the current
`PoseOverride` of driven target `i`") so both the bone-handle and node-entity producers feed the same
transition core. The freeze, the `x = transition / transition_duration` ramp, and the per-target
`blend_joint`/`apply_delta` selection are one block parameterized by that accessor.

### Clear / prune semantics extend to node overrides + bindings

`clear_overrides` (`runtime.rs:clear_overrides`) removes `PoseOverride` from a rig's targets when the rig
goes inactive / has no clip. For the node path it removes `PoseOverride` from the **bound node entities**
and `MorphWeightOverride` from morph-bearing entities (reverting morph to the durable `MorphComponent`
weights — decision: `MorphWeightOverride` is runtime-only and unregistered, removed on stop exactly like
`PoseOverride`). `AnimationRuntime::clear` and `prune_session` (`runtime.rs:clear`/`prune_session`) also
drop `node_bindings` so a project reload / preview re-enter re-resolves bindings fresh against the new
entity set — a stale cached `Entity` from the prior scene must never be reused.

## Changes

| What | Location (file:symbol) | Kind |
|---|---|---|
| Factor keyframe-locate + Step/Linear/CubicSpline into a reusable core | `animation/src/sample.rs` (new private `KeyFrame` helper) | modify |
| N-wide morph-weight sampler (no slerp, no normalize, `morph_count` lanes) | `animation/src/sample.rs:sample_weights` | new |
| Delete the index-only public `sample_clip` | `animation/src/sample.rs:sample_clip` | delete |
| Drop the `sample_clip` re-export | `animation/src/lib.rs` (`pub use sample::{sample_clip, sample_track}`) | modify |
| Rewrite the moved `sample_clip` unit test onto `sample_clip_resolved` + `sample_weights` | `animation/src/lib.rs:sample_clip_writes_tracked_joints_and_keeps_rest` | modify |
| Make `SkinnedMesh` optional in the gather query | `animation/src/runtime.rs:tick_animation` (`for_each::<(&AnimationPlayer, Option<&SkinnedMesh>, Option<&IdComponent>), _>`) | modify |
| `RigKind { Skinned, Node }` + carry it on `Rig`; node rigs route via the container root | `animation/src/runtime.rs:Rig` | modify |
| Gate the bone rebind + write on `AnimTarget::Bone`; skip Node/Weights tracks | `animation/src/runtime.rs:sample_clip_resolved` | modify |
| Node producer: seed rest from bound node `Transform`s, write `PoseOverride` on node entities | `animation/src/runtime.rs:tick_rig` | modify |
| Weights producer: `sample_weights` → `MorphWeightOverride` on the morph-bearing entity | `animation/src/runtime.rs:tick_rig` | modify |
| Node-binding cache + first-match pre-order subtree name walk scoped to the forest root | `animation/src/runtime.rs:AnimationRuntime` (`node_bindings`), new private `resolve_node_binding`/`bind_track` | new |
| Generalize the transition freeze/blend off bone handles via a per-target accessor | `animation/src/runtime.rs:outgoing_at` + the transition block in `tick_rig` | modify |
| Clear node `PoseOverride` + `MorphWeightOverride` + bindings on inactive/clear/prune | `animation/src/runtime.rs:clear_overrides`, `:clear`, `:prune_session` | modify |

## New artifacts

- `sample_weights(track, t, out: &mut [f32])` — the N-wide morph-weight sampler.
- A private `KeyFrame` keyframe-locate helper shared by `sample_track` and `sample_weights`.
- `RigKind { Skinned, Node }` and the node-rig branch of `tick_rig`.
- `AnimationRuntime.node_bindings: HashMap<u64, Vec<Option<Entity>>>` and the first-match pre-order
  subtree name-walk resolver scoped to the player's forest root.

## NO-LEGACY cutover (deleted in THIS change)

- **Delete the duplicate index-only `sample_clip`** (`animation/src/sample.rs:sample_clip`) and its
  `lib.rs` re-export — there is exactly one sampler entry, `sample_clip_resolved`. Every caller of the
  deleted symbol moves in this change: the public re-export is removed and the `lib.rs` unit test
  `sample_clip_writes_tracked_joints_and_keeps_rest` is rewritten against `sample_clip_resolved`
  (which needs the `bone_names`/`name_to_index` args) plus a new `sample_weights` case. `clippy
  -D warnings` catches any stray reference left on the dead path.
- **Replace the bone-only transition path with the generalized per-target one** — there is no
  "skinned transition" + "node hard-sample" pair; the one transition core serves both rig kinds via the
  per-target accessor.
- **Node binding never uses `find_entity_by_uuid`** for track resolution — the global scan is replaced
  by the forest-scoped first-match pre-order walk for this purpose (the global helper stays for genuine
  cross-scene lookups elsewhere; it gains no animation caller).

## Test gate

`cargo test -p saffron-animation`:

- `sample_weights` Step / Linear / CubicSpline endpoints + midpoints; empty clip → `out` keeps the
  seeded rest weights; `morph_count`-wide lanes all sampled.
- A node-forest player (container root + named child entities, **no `SkinnedMesh`**) ticks in Play and
  writes a `PoseOverride` onto the bound node entity; `update_world_transforms` reflects it (mirrors
  `skinning_seam_palette_reflects_animation`'s seam assertion but for a plain node).
- Joint vs Node routing: a clip mixing an `AnimTarget::Bone` track and an `AnimTarget::Node` track on the
  same rig drives only the bone via the joint index and only the node via the binding — neither leaks
  into the other (the joint-index-coupling hazard regression).
- A weights track writes `MorphWeightOverride`; stopping the player (inactive / no clip) clears the
  node `PoseOverride` **and** the `MorphWeightOverride` **and** drops the `node_bindings` slot, so morph
  reverts to the durable `MorphComponent.weights` and the binding re-resolves next play.
- Stale-binding re-resolve: destroy the bound node entity, recreate it under the same name, tick → the
  cache slot re-resolves by the name walk to the new entity (never a stale handle, never a cross-instance
  bind — assert two instances of the same-named forest each bind their own subtree).
- A node-forest cross-fade and an inertialization transition reach the same start (outgoing) /
  steady-state (incoming) oracle as `crossfade_starts_outgoing_ends_incoming` /
  `inertialize_c0_at_switch`, proving the FULL transition path runs on node targets.

End with the milestone gate per AGENTS.md: `just engine`, then `just prepare-for-commit` (format +
`clippy -D warnings`), fixing every warning this change raises.
