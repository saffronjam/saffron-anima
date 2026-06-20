# Phase 5 — Animation command domain

**Status:** COMPLETED

**Depends on:** 09-control-plane:phase-1-socket-server-and-dispatch, 04-animation (the animation player runtime, clips, foot-IK), 03-ecs-and-scene + 08-host-and-viewport (sceneEdit for the active rig / overlay state), 06-rendering (skeleton + debug overlays)

## Goal

Register the 13 animation-domain commands (`register_animation_commands`): animation playback state
(get-state, play, set-playing, seek, set-loop, stop-preview), clip listing, the skeleton overlay
(get/set/highlight + joint pick), the debug overlays (get/set), and foot-IK (get/set). Reach is
`sceneEdit` (15), `assets` (4), `renderer` (2) — the handlers drive the per-rig animation player and
the overlay render state.

## Why this shape (NO LEGACY)

- **Reach: `sceneEdit` (the active rig + overlay flags), `assets` (clip lookup), `renderer` (overlay
  geometry/joint pick).** The animation player runtime lives in `04-animation`; these handlers are the
  thin command surface over it. The skeleton/debug overlay render state is renderer-side
  (`get-debug-overlays`/`set-debug-overlays`/`pick-skeleton-joint` touch `ctx.renderer`).
- **Playback commands share `AnimationStateResult`.** `get-animation-state`/`play-animation`/
  `set-animation-playing`/`seek-animation`/`set-animation-loop`/`stop-preview` all return the same
  `AnimationStateResult` (the live player state of a rig); the entity is selected via `EntitySelector`
  in `AnimationStateParams`/`PlayAnimationParams`/etc. Kept uniform.
- **The ~430-line C++ animation self-test does NOT port as a runtime function.** Per the locked rules
  and `13-testing-and-verification` (PP-13), that self-test is the *oracle* for `04-animation`'s
  `#[test]` units (IK/sampling fidelity), not a control command and not an in-engine self-test. The
  control layer only exposes the playback/overlay/IK *commands*; their correctness is asserted by e2e
  fixtures + the animation crate's own unit tests.
- **Foot-IK is a per-entity toggle+params** (`get-foot-ik`/`set-foot-ik` ↔ `FootIkResult`); the IK
  solver itself is `04-animation`. `set-foot-ik` on a non-rig entity is a no-op/typed-result, not a
  crash (the C++ guards it).
- **Skeleton highlight + joint pick feed the editor's bone selection** — `set-skeleton-highlight`
  highlights a joint, `pick-skeleton-joint` ray-picks one from a viewport coordinate
  (`PickSkeletonJointParams`/`PickSkeletonJointResult`), reaching the renderer for the overlay
  geometry. Ported as-is.

## Grounding (real files/symbols)

- `engine-old/source/saffron/control/control_commands_animation.cpp`
  - `registerAnimationCommands` (13 `registerCommand` invocations).
  - Reach: `ctx.sceneEdit` (the active rig + overlay flags), `ctx.assets` (clip lookup in `list-clips`),
    `ctx.renderer` (overlay + joint pick).
- DTOs: `AnimationStateParams`/`AnimationStateResult`, `ListClipsParams`/`ListClipsResult`/
  `AnimationClipDto`, `PlayAnimationParams`, `SetAnimationPlayingParams`, `SeekAnimationParams`,
  `SetAnimationLoopParams`, `SetSkeletonOverlayParams`/`SkeletonOverlayResult`,
  `SetSkeletonHighlightParams`, `PickSkeletonJointParams`/`PickSkeletonJointResult`,
  `DebugOverlaysParams`/`DebugOverlaysResult`, `GetFootIkParams`/`SetFootIkParams`/`FootIkResult` —
  all in `control_dto.cppm`.
- `09-control-plane/catalog.md` — the animation-domain table (13 rows) + fixtures.

## Acceptance gate

- `cargo build -p saffron-control` green with the animation handlers registered; clippy/fmt clean.
- `cargo test -p saffron-control` passes animation-domain unit tests over a stub rig:
  - `set-skeleton-overlay` toggles the overlay flag and reads it back (`skeleton-overlay-on` shape).
  - `set-debug-overlays` with `"bounds"` round-trips (`debug-overlays-bounds` fixture).
  - `get-foot-ik`/`set-foot-ik` on a non-rig entity return a typed result (no crash), and on a rig
    round-trip the IK enable flag (`foot-ik-on` fixture).
  - the playback commands (`play-animation`/`seek-animation`/...) return a coherent
    `AnimationStateResult` against the stub player.
- The wire-contract test validates the fixtured animation commands' live `result` against OpenRPC and
  `help` against the manifest (`empty`, `skeleton-overlay-on`, `debug-overlays-bounds`, `foot-ik-on`,
  `cube-entity`); the rig-dependent commands carry their manifest skip reason.
- All entity ids in animation results stay decimal strings.
