# Phase 4 — The player runtime: `tick_animation`, transitions, loop-blend, foot-IK

**Status:** COMPLETED

**Depends on:** 04-animation:phase-1-crate-sampling-pose-algebra, 04-animation:phase-2-two-bone-ik

## Goal

Port the per-session runtime: `AnimationRuntime` (the clip cache + injected loader + transition and
last-pose state), `AnimMode`, `TransitionState`, the playhead advance, the foot-IK producer
(`apply_foot_ik`), and the driver `tick_animation` that samples + advances every rig and writes a
`PoseOverrideComponent` onto each driven bone. This is the first phase that touches `saffron-scene`
(reading `SkinnedMeshComponent`/`AnimationPlayerComponent`/`FootIkComponent`, writing
`PoseOverrideComponent`) and the scene FK/world helpers.

## Why this shape (NO LEGACY)

- **The clip loader is an injected `Box<dyn Fn>`, not a hard dependency on `saffron-assets`.** Clip
  bytes live in a `.smodel` SANM chunk whose reader is in assets; animation must not depend on assets
  (the DAG forbids it). So `AnimationRuntime` holds
  `clip_loader: Option<Box<dyn Fn(Uuid) -> Result<AnimClip>>>`, installed by the host
  (`host.cppm:1005`). Unset ⇒ no clip loads, exactly as the C++ null-function guard. This is one
  injected dependency, so a boxed `Fn` (not a trait, not an enum) is correct (`conventions.md` itable
  rule: single injected callback → `Box<dyn Fn>`).
- **The clip cache is a negative cache by construction.** A broken asset is cached as
  `AnimClip::default()` so it is not re-read every frame, and a `log_warn` is emitted once on the miss
  — the C++ `clipCache.emplace(value, AnimClip{})` shape. `clip.value == 0` short-circuits to "no clip"
  before any lookup. (We keep this as `HashMap<u64, AnimClip>` owned by the runtime, not
  `Option<Arc>`, because clips are read by value into a per-frame `PoseBuffer`; nothing shares them.)
- **`tick_animation` is infallible and borrows `&mut Scene`.** It returns `()`; loader errors are
  swallowed into the negative cache. It iterates with the chosen ECS query (the `forEach<Player,
  Skinned>` equivalent from `03-ecs-and-scene`) and mutates the scene through the scene crate's
  component-insert API (`emplace_or_replace` → the ECS `insert`). No `Arc`/lock: single-threaded,
  exclusive `&mut Scene`.
- **Transition state is keyed by entity uuid, captured once at the switch frame.** `TransitionState`
  (frozen outgoing pose + per-joint offset) is built once when `transition <= 0` or the key is absent,
  then the offset is decayed (`quintic_decay`) for inertialization or the outgoing pose cross-faded
  (`smoothstep01`) for cross-fade — the exact C++ branch. The state map and `last_pose` map are keyed
  by the `IdComponent` uuid `u64`. We port the erase-on-completion and erase-on-no-clip cleanup so the
  maps do not leak across project reload.
- **`apply_foot_ik` resolves the chain by FK from this frame's sampled pose, not the cached world
  transform.** This is a deliberate correctness choice in C++ (reading the cached
  `WorldTransformComponent` would feed the solver last frame's post-IK output and oscillate). The port
  keeps the FK walk from the chain root's parent down through `final_local`, the foot-lift-to-ground,
  the solve, and the parent-world-rotation strip back to local space. It is gated on an enabled
  `FootIkComponent`, so non-IK rigs pay nothing.
- **`last_pose` snapshot is kept though it has no consumer yet.** It is the reserved finite-difference
  source the `05-physics` ragdoll handoff will read. Per NO LEGACY this is *not* speculative dead code
  — it is a tested seam with a named near-term consumer, ported so phase-5-physics has its target. We
  keep it and its tests; we do not add a fake consumer.
- **Rest pose comes from the scene's Euler→quat convention.** `rest_pose_of` reads a bone's
  `TransformComponent` and converts its Euler `rotation` to a quat *matching `transform_matrix`*
  (`Quat::from_euler` in the scene's `Rz*Ry*Rx` order — the scene crate owns that convention, so
  animation calls the scene helper rather than re-deriving the order).

## Grounding (real files/symbols)

- `engine-old/source/saffron/animation/animation.cppm` — `AnimMode` (`:88`), `TransitionState`
  (`:96`), `AnimationRuntime` (`:104`: `clipCache`, `clipLoader`, `transitions`, `lastPose`),
  `tickAnimation` decl (`:123`).
- `engine-old/source/saffron/animation/animation.cpp`
  - `restPoseOf` (`:42`, Euler→quat via `glm::quat(rotation)`), `clearOverrides` (`:62`), `loadClip`
    (`:75`, negative cache + `logWarn`), `advanceTime` (`:100`, Once/Loop/PingPong).
  - `parentWorldRotation` (`:206`), `applyFootIk` (`:228`: chain handle/index guards, FK from
    `finalLocal` not the cache (`:259`), `target.y = max(.., groundHeight)` (`:291`), solve + strip
    parent rotation (`:299`–`:302`)).
  - `sampleClipResolved` (`:308`, durable-name re-bind of a stale index).
  - `tickAnimation` (`:603`): active gate (`:610`), clip resolve + clear-on-none (`:613`–`:627`), rest
    seed + name↔index maps (`:629`–`:648`), `sampleInto`/`outgoingAt` (`:649`–`:672`), `advanceTime` +
    Loop-wrap loopBlend seed (`:674`–`:686`), transition capture/blend (`:690`–`:732`), foot-IK gate
    (`:737`), `emplace_or_replace<PoseOverrideComponent>` write (`:743`–`:758`), `lastPose` snapshot
    (`:762`).
- `engine-old/source/saffron/scene/scene.cppm` — `forEach` (`:730`), `transformMatrix` (`:410`),
  `worldRotation`/`worldTranslation` (`:904`,`:898`), `SkinnedMeshComponent`/`AnimationPlayerComponent`/
  `PoseOverrideComponent`/`FootIkComponent`/`FootChain`/`RelationshipComponent` (`:84`,`:97`,`:128`,
  `:149`,`:137`,`:52`).
- `engine-old/source/saffron/host/host.cppm` — `state->animation.clipLoader = ...` (`:1005`), the
  `tickAnimation(...)` call (`:1493`), and the physics-after-animation ordering note (`:1159`).
- `conventions.md` (itable → `Box<dyn Fn>` for the injected loader; negative-cache shape; sum types).

## Acceptance gate

- `cargo build -p saffron-animation` and the workspace build are green; the crate's only new dep edges
  are to `saffron-scene` (components + FK helpers) and `saffron-geometry` (clip types), already
  declared.
- Crate root `#![deny(unsafe_code)]`; `cargo clippy` + `cargo fmt --check` clean.
- `cargo test -p saffron-animation` passes the phases 1–3 tests still, plus at least a smoke runtime
  test that constructs a `Scene` with one rig + a directly-seeded clip cache, ticks once in
  `AnimMode::Play`, and asserts a `PoseOverrideComponent` appears on the driven bone (the full
  behavioural runtime suite is phase 5).
- No public `Result`-returning tick/sample functions regress to `Result<T, String>`; the loader is the
  only `Result` site and uses the crate's typed `Error` (`conventions.md` error model).
