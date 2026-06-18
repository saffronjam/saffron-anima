# Phase 9 — Editor fly-camera

**Status:** COMPLETED

**Depends on:** 03-ecs-and-scene:phase-8-sceneedit-crate-and-context

## Goal

Port the viewport fly-camera: the yaw/pitch → forward/view math, the per-frame
`update_scene_edit_camera` (exponentially-smoothed look drain + WASD/Space/Shift move), and the
`scene_edit_camera_to_json`/`from_json` serde the control caller round-trips into `project.json`. Small,
self-contained, pure glam.

## Why this shape (NO LEGACY)

- **The look smoothing is the same `tau = 0.025` exponential the gizmo/edit smoothing uses.** Look samples
  stream over the control plane at ~60 Hz; `update_scene_edit_camera` accumulates the pending delta and
  drains it `alpha = 1 - exp(-dt/tau)` each rendered frame so the look does not staircase
  (`scene_edit_camera.cpp:61`). The drain runs even when inactive (easing the tail). This is the same
  constant phase-11 shares — keep it a single named const.
- **`controlling` latches while RMB is held** so a drag can leave the viewport rect without dropping
  control (`scene_edit_camera.cpp:80`). Pitch clamps to ±89°.
- **Forward is yaw/pitch spherical, view is `look_at`.** `scene_edit_camera_forward`
  (`cos(pitch)·sin(yaw), sin(pitch), -cos(pitch)·cos(yaw)`, `scene_edit_camera.cpp:18`); the view is
  `Mat4::look_at_rh(position, position+forward, +Y)` (glam's `look_at_rh` is the `glm::lookAt` analogue).
  `scene_edit_camera_view` returns a `CameraView` so `render_scene` and the gizmo draw from the same eye.
- **Serde keeps the exact key set** (`position`/`yaw`/`pitch`/`fov`), and `from_json` keeps the current
  value for missing fields (`scene_edit_camera.cpp:38`) so a partial save does not zero the camera. This
  block round-trips into `project.json` via the control save/load caller (PP-6 area), not from here.
- **Input is a backend-neutral struct.** `SceneEditCameraInput` (`active`/`look_delta`/move bools) is
  filled by the host (which owns SDL/winit); sceneedit stays input-backend-free.

## Grounding (real files / symbols)

- `engine-old/source/saffron/sceneedit/scene_edit_camera.cpp`: `sceneEditCameraForward` (18),
  `sceneEditCameraView` (26), `sceneEditCameraToJson` (38), `sceneEditCameraFromJson` (46),
  `updateSceneEditCamera` (61, `tau=0.025`, pitch clamp ±89, the `controlling` latch).
- `engine-old/source/saffron/sceneedit/scene_edit_context.cppm`: `SceneEditCamera` (28),
  `SceneEditCameraInput` (45).

## Acceptance gate

- Cargo workspace compiles.
- `cargo test -p saffron-sceneedit`:
  - `scene_edit_camera_forward` matches C++ values at yaw 0 (looks down −Z) and a sample yaw/pitch.
  - `update_scene_edit_camera` over a few frames moves the position with WASD active and converges the
    look toward the pending delta (smoothing monotonic toward target); pitch clamps at ±89; `controlling`
    latches/unlatches with `active`.
  - `scene_edit_camera_to_json`/`from_json` round-trip preserves position/yaw/pitch/fov; a partial doc
    leaves unset fields unchanged.
- Workspace build green; prior phases still pass.
