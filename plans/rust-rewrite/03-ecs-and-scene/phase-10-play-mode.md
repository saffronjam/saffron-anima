# Phase 10 — Play mode

**Status:** COMPLETED

**Depends on:** 03-ecs-and-scene:phase-9-fly-camera

## Goal

Port the editor play-mode state machine: `enter_play` / `pause_play` / `resume_play` / `step_play` /
`stop_play`, the `tick_play` driver with the `sim_tick` simulation seam, `render_camera_view`, the
script-error and script-log bounded rings, and the JSON-roundtrip scene duplicate. Port the C++
`runPlayModeSelfTest` as the regression `#[test]`s. This is where the area earns the "play has no undo —
the discard is the restore" guarantee.

## Why this shape (NO LEGACY)

- **`enter_play` duplicates via JSON round-trip, not `World::clone`.** It runs `scene_to_json` then
  `scene_from_json` into a fresh `Scene` sharing the catalog `Arc` (`scene_edit_play.cpp:83`) — the
  duplicate is exactly "what a save/load would produce." This is why phase-7's serde fidelity is the
  correctness foundation, and why we do *not* reach for a cheaper structural clone (which would diverge
  from the on-disk format and is also not portably available across ECS crates). `stop_play` drops the
  `Option<Scene>` duplicate — the discard is the restore; the authored scene was never writable through
  `active_scene` during play, so there is no restore step to get wrong.
- **The state machine validates each transition and bumps `play_version` + publishes
  `on_play_state_changed`.** `publish_transition` sets the state, bumps `play_version`, and publishes the
  signal (the physics/scripting lifecycle seam). `enter_play` from non-Edit is an error; `pause` requires
  Playing; `resume` requires Paused; `step` requires Paused and `frames >= 1`; `stop` in Edit is an
  idempotent success (`scene_edit_play.cpp:75`–164). Each `Result<()>` is a typed error per PP-1.
- **Selection re-resolves by uuid across the duplicate boundary.** Entt handles index one registry and
  could alias an unrelated entity in the other (`scene/AGENTS.md`): `enter_play` captures the selected
  uuid in the authored scene and re-selects it in the duplicate via `find_entity_by_uuid`; `stop_play`
  does the reverse, and a runtime-spawned selection (no authored twin) clears
  (`scene_edit_play.cpp:93,102,151,162`). The smoothing queues are dropped on every transition (they hold
  handles tied to one registry).
- **`tick_play` is the gated driver; `sim_tick` is the host-filled seam.** It no-ops in Edit, runs when
  Playing or while `step_frames > 0` (consuming one stepped frame at fixed `PlayFixedStep = 1/60`),
  clamps `dt` to `PlayMaxDelta = 1/3`, increments `play_tick`, and invokes `sim_tick(active_scene, dt)`.
  Per PP-1, `sim_tick` is a `Box<dyn FnMut(&mut Scene, f32)>` field (or a host-implemented trait object) —
  the host points it at the script runtime, keeping sceneedit free of script/physics deps
  (`scene_edit_play.cpp:181`).
- **`render_camera_view`** returns the fly-camera in Edit, the active scene's primary `CameraComponent`
  during play, falling back to the fly-camera (never black) when the scene has none
  (`scene_edit_play.cpp:166`).
- **The rings are bounded and seq-monotonic.** `push_script_error` (cap `ScriptErrorRingCap = 256`) and
  `push_script_log` (cap `ScriptLogRingCap = 1024`) drop the oldest at cap, stamp a monotonic `seq` + the
  play tick (+ wall-clock ms for logs, display-only, never determinism). `enter_play` clears the rings but
  keeps `seq` monotonic for the drain cursor (`scene_edit_play.cpp:207`,218; the C++ uses a `Vec::erase`
  front-drop — the Rust port uses a `VecDeque` for O(1) front-drop while preserving the cap behavior).

## Grounding (real files / symbols)

- `engine-old/source/saffron/sceneedit/scene_edit_play.cpp`: `playStateName`/`playStateFromName` (48/62),
  `enterPlay` (75), `pausePlay` (109), `resumePlay` (119), `stepPlay` (130), `stopPlay` (144),
  `renderCameraView` (166), `tickPlay` (181), `pushScriptError` (207), `pushScriptLog` (218),
  `publishTransition`/`selectedUuidIn`/`dropSmoothing` (23/30/41), `runPlayModeSelfTest` (232).
- `engine-old/source/saffron/sceneedit/scene_edit_context.cppm`: `PlayFixedStep`/`PlayMaxDelta` (187/188),
  `ScriptErrorRingCap`/`ScriptLogRingCap` (170/185), the `simTick` field + `enter/stop` decls (251,
  328–337).

## Acceptance gate

- Cargo workspace compiles; the play state machine + `tick_play`/`sim_tick` seam + rings exist.
- `cargo test -p saffron-sceneedit` ports `runPlayModeSelfTest` as `#[test]`s and they pass: Edit-state
  rejections (pause/resume/step), idempotent stop in Edit, `enter_play` lands Playing + bumps
  `play_version` + re-enter rejects, duplicate entity count == edit count, cube uuid resolves in the
  duplicate with carried transform, `active_scene` routes to the duplicate, `render_camera_view` cuts to
  the scene's primary camera and falls back to fly-cam when none, pause/step consumes stepped frames at
  `PlayFixedStep`, stop drops the duplicate + bumps `scene_version` + authored transform survives play
  edits + runtime-spawned entity does not survive, selection restores by uuid / clears for a
  runtime-spawned twin, the preview accessor routes while playState stays Edit, and the script-log ring
  caps at `ScriptLogRingCap` with monotonic seq cleared (seq preserved) on `enter_play`.
- Workspace build green; prior phases still pass.
