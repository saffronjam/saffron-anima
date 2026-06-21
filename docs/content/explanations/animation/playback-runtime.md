+++
title = 'Playback runtime'
weight = 2
+++

# Playback runtime

The playback runtime is the per-frame step that turns an animation clip into a visible pose. It
reads each rig's `AnimationPlayer`, samples its clip at the current playhead, and writes the result
onto the skeleton — without ever touching the authored rest pose. Like UE5 (Persona) and Unity (the
Animation window), it previews in the editor without entering play: preview is decoupled from the
game's play state.

The authored bone `Transform`s are the rest pose and are never written. The animated pose lives in a
separate runtime-only `PoseOverride` component that world-transform composition prefers when present.
That single rule makes Edit preview non-destructive *by construction* — there is no snapshot to take
or restore, and nothing marks the project dirty.

## The flow

```mermaid
flowchart LR
  clip[AnimClip] -->|sample_clip| pose[PoseBuffer.local]
  rest[bone rest TRS] -->|seed| pose
  pose -->|blend layer| final[final TRS]
  final -->|add_component| ov[PoseOverride]
  ov -->|local_matrix prefers it| world[world transform]
  world --> joints[joint_matrices → GPU skinning]
```

`tick_animation` runs once per frame over every entity with both an `AnimationPlayer` and a
`SkinnedMesh`. It gathers each rig in a single `for_each` query pass, then processes each one
(`tick_rig`) with full scene access:

1. **Gate.** In `Play` every rig is active; in `Edit` only a rig whose player has `preview_in_edit`
   set is. An inactive rig has its overrides removed, so it falls back to the rest pose.
2. **Advance.** When `playing`, the playhead moves by `dt × speed` under the wrap mode (below).
3. **Sample.** A `PoseBuffer` is seeded with each bone's rest local TRS, then `sample_clip` writes
   the tracked joints over it — so an untracked joint, or an untracked channel of a tracked joint,
   keeps its authored value.
4. **Resolve.** Each track binds to its joint by index when sound, re-resolved by the durable node
   name when the index is stale (the [clip/track model](../animation-data-model/) carries both).
5. **Blend.** The per-bone blend layer is inert in v1 (the `override_`/`weight` vectors stay empty,
   so the final pose is `local`), but the call site exists so a later pose producer — foot IK, a
   powered ragdoll — only writes `override_` + `weight`.
6. **Write.** The final TRS is added as a `PoseOverride` component on each driven bone.

`update_world_transforms` then composes each bone from its override (via its quaternion directly,
no Euler round-trip) instead of its `Transform`, and `joint_matrices` feeds the GPU skinning pass.
The runtime needs no change to the skinning math — only the *source* of a bone's local transform
changes.

## Edit preview vs Play

Animation is evaluated every frame in **both** modes, gated internally:

- **Edit** — only a `preview_in_edit` rig animates; everything else stays at rest. Importing a rig
  does not auto-play it (matching UE/Unity, which don't auto-run level animation in-editor). The
  timeline sets `preview_in_edit` + `playing`/`time` to scrub or play the selected entity.
- **Play** — every rig animates as part of the simulation. Play still uses the duplicate-and-discard
  scene for scripts and spawns; animation simply never needs it, because it never mutates authored
  data. Stopping discards the play scene and the authored rest pose returns untouched.

`tick_animation` runs before scripts in the host's update spine, so during Play the pose lands first
and a script can still override a bone through the same `PoseOverride` component the same frame.

## Wrap modes and speed

`speed` scales `dt` (negative plays backward). `wrap` decides the end behaviour:

- **Once** — clamp at the end (or start) and stop `playing`.
- **Loop** — wrap the playhead modulo the duration.
- **PingPong** — bounce at each end, flipping the stored `ping_forward` direction.

## Transitions

A clip change pops if the new clip's first pose differs from the current one. Two mechanisms
smooth it, both keyed per entity by the rig's id uuid in `AnimationRuntime`, captured once at the
switch:

- **Cross-fade** — freeze the outgoing pose and blend it toward the incoming clip by a
  smoothstepped `alpha = transition / transition_duration`. Simple to reason about; the obvious
  fallback if a quintic artifact shows up.
- **Inertialization** (the default) — capture the per-joint *pose offset* between the outgoing
  pose and the incoming clip at the switch, then evaluate **only** the incoming clip and decay the
  offset to zero with a quintic (C², zero-jerk). It is roughly half the cost of a sustained two-clip
  blend, and — the strategic reason it is the default — it reuses the exact `PoseDelta` machinery a
  physics handoff (a powered ragdoll) needs to nudge an animated target.

The offset is `pose_diff(outgoing, incoming)` — additive translation, a delta quaternion
(`outgoing · inverse(incoming)`, decayed via `Quat::IDENTITY.slerp(Δrot, k)`, never a raw component
lerp), and a multiplicative scale ratio. `apply_delta(incoming, offset, k)` returns the outgoing
pose at `k = 1` and the incoming pose at `k = 0`, so the switch frame matches the outgoing pose
exactly (no pop) and the result eases onto the incoming clip.

A **Loop wrap** is just a transition too: when `loop_blend > 0`, crossing the seam captures the
end-pose and inertializes onto the wrapped start-pose over `loop_blend` seconds, so looped
locomotion does not stutter. A transition is started by a control command (the `play-animation`
`--blend` arg); the component carries the idle state (`prev_clip`, `transition`,
`transition_duration`, `transition_mode`) so it round-trips harmlessly at rest.

## In the code

| What | File | Symbols |
|---|---|---|
| Evaluator (gate, advance, sample, blend, write) | `engine/crates/animation/src/runtime.rs` | `tick_animation`, `tick_rig`, `advance_time` |
| Transitions (cross-fade, inertialize, loop-wrap) | `engine/crates/animation/src/algebra.rs` | `pose_diff`, `apply_delta`, `blend_joint`, `quintic_decay` |
| Mode + clip cache + transition state | `engine/crates/animation/src/lib.rs`; `runtime.rs` | `AnimMode`, `AnimationRuntime`, `ClipLoader` |
| Dumb-data player + transition fields | `engine/crates/scene/src/component.rs` | `AnimationPlayer`, `Wrap`, `Transition` |
| Runtime override + composition | `engine/crates/scene/src/component.rs`; `hierarchy.rs` | `PoseOverride`, `local_matrix`, `update_world_transforms` |
| Per-frame host wiring | `engine/crates/host/src/layer.rs` | `tick_animation` call |

> [!NOTE]
> The clip cache resolves through an injected `ClipLoader` closure (the animation crate must not
> depend on `saffron-assets`, so the host hands in a loader borrowing the live catalog). It is keyed
> by clip uuid and cleared on project (re)load. A failed load is negative-cached as an empty clip so
> a broken asset is not re-read every frame.

## Related

- [Animation data model](../animation-data-model/) — the clip/track/pose types this samples and blends
- [Transforms & matrices](../../scene-and-ecs/transform-and-matrices/) — the world composition it feeds
- [Play mode](../../ui-and-editor/play-mode/) — the duplicate-and-discard scene Play uses elsewhere
