//! The editor fly-camera: its data, the backend-neutral per-frame fly input, the
//! yaw/pitch → forward/view math, the per-frame `update_scene_edit_camera` (exponentially
//! smoothed look drain + WASD/Space/Shift move), and the serde the control caller
//! round-trips into `project.json`.
//!
//! These are the scene-view eye, distinct from any ECS `Camera` / game camera. SceneEdit
//! stays SDL-free — `look_delta` and the move bools arrive as plain data the host fills.

use glam::{Mat4, Vec2, Vec3};
use saffron_json::json_f32_or;
use saffron_scene::CameraView;
use serde_json::{Map, Value};

use crate::smoothing::SMOOTH_TAU;

/// The viewport's own fly-camera.
///
/// Hold RMB over the viewport to look + WASD to move, Space up / Shift down. `yaw`/`pitch`
/// are degrees; at yaw 0 the camera looks down `-Z`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SceneEditCamera {
    /// The eye position (world space).
    pub position: Vec3,
    /// The yaw angle, degrees.
    pub yaw: f32,
    /// The pitch angle, degrees.
    pub pitch: f32,
    /// Vertical field of view, degrees.
    pub fov: f32,
    /// Near clip plane.
    pub near_plane: f32,
    /// Far clip plane.
    pub far_plane: f32,
    /// Move speed, units per second.
    pub move_speed: f32,
    /// Look speed, degrees per pixel.
    pub look_speed: f32,
    /// Undelivered look pixels, drained exponentially per frame.
    pub look_pending: Vec2,
    /// Latched while RMB is held (so a drag can leave the rect).
    pub controlling: bool,
}

impl Default for SceneEditCamera {
    fn default() -> Self {
        Self {
            position: Vec3::new(3.0, 2.5, 4.0),
            yaw: -37.0,
            pitch: -29.0,
            fov: 45.0,
            near_plane: 0.1,
            far_plane: 100.0,
            move_speed: 6.0,
            look_speed: 0.12,
            look_pending: Vec2::ZERO,
            controlling: false,
        }
    }
}

/// Backend-neutral per-frame fly-cam input the host fills, so SceneEdit stays SDL-free.
///
/// `look_delta` is summed relative-mouse motion (pixels) for the frame; the bools are the
/// move-key state during the RMB hold.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SceneEditCameraInput {
    /// RMB-fly engaged (keyboard grabbed).
    pub active: bool,
    /// Summed relative-mouse motion this frame (pixels).
    pub look_delta: Vec2,
    /// Forward (W).
    pub forward: bool,
    /// Back (S).
    pub back: bool,
    /// Left (A).
    pub left: bool,
    /// Right (D).
    pub right: bool,
    /// Up (Space).
    pub up: bool,
    /// Down (LShift).
    pub down: bool,
}

impl SceneEditCamera {
    /// The camera's forward (world space) from its yaw/pitch.
    ///
    /// Spherical from the degree angles: at yaw 0 / pitch 0 it looks down `-Z`
    /// (`cos(pitch)·sin(yaw), sin(pitch), -cos(pitch)·cos(yaw)`, the C++
    /// `sceneEditCameraForward`).
    #[must_use]
    pub fn forward(&self) -> Vec3 {
        let yaw = self.yaw.to_radians();
        let pitch = self.pitch.to_radians();
        Vec3::new(
            pitch.cos() * yaw.sin(),
            pitch.sin(),
            -pitch.cos() * yaw.cos(),
        )
        .normalize()
    }

    /// The camera as a scene [`CameraView`] (view + projection params), so `render_scene`
    /// and the gizmo draw from the same eye (the C++ `sceneEditCameraView`).
    ///
    /// The view is `look_at_rh(position, position + forward, +Y)` — glam's `look_at_rh`
    /// is the `glm::lookAt` analogue.
    #[must_use]
    pub fn view(&self) -> CameraView {
        CameraView {
            view: Mat4::look_at_rh(self.position, self.position + self.forward(), Vec3::Y),
            fov: self.fov,
            near_plane: self.near_plane,
            far_plane: self.far_plane,
        }
    }

    /// The persisted editor view, the exact key set the C++ `sceneEditCameraToJson` emits:
    /// `{ position: {x,y,z}, yaw, pitch, fov }`. Saved into `project.json` by the control
    /// save/load caller so a reopened project shows the same framing.
    #[must_use]
    pub fn to_json(&self) -> Value {
        Value::Object(Map::from_iter([
            ("position".to_string(), vec3_to_json(self.position)),
            ("yaw".to_string(), f32_value(self.yaw)),
            ("pitch".to_string(), f32_value(self.pitch)),
            ("fov".to_string(), f32_value(self.fov)),
        ]))
    }

    /// Reads the persisted view back (the C++ `sceneEditCameraFromJson`). A missing field
    /// keeps the current value, so a partial document does not zero the camera; a non-object
    /// is ignored entirely.
    pub fn from_json(&mut self, j: &Value) {
        if !j.is_object() {
            return;
        }
        if let Some(position) = j.get("position") {
            self.position = vec3_from_json(position);
        }
        self.yaw = json_f32_or(j, "yaw", self.yaw);
        self.pitch = json_f32_or(j, "pitch", self.pitch);
        self.fov = json_f32_or(j, "fov", self.fov);
    }
}

/// Flies the editor camera one frame from host-gathered input (the C++
/// `updateSceneEditCamera`).
///
/// The accumulated look delta drains exponentially (`alpha = 1 - exp(-dt/TAU)`, the same
/// constant as the gizmo drag) so the ~60 Hz control look samples do not staircase; the
/// drain runs even when inactive, easing the tail out. Pitch clamps to ±89°. `controlling`
/// latches while `active` holds so a drag can leave the viewport rect without dropping
/// control. WASD moves along the camera basis, Space/Shift along world `±Y`, scaled by
/// `move_speed · dt`.
pub fn update_scene_edit_camera(
    camera: &mut SceneEditCamera,
    input: &SceneEditCameraInput,
    dt: f32,
) {
    camera.look_pending += input.look_delta;
    let alpha = 1.0 - (-dt.max(0.0) / SMOOTH_TAU).exp();
    let step = camera.look_pending * alpha;
    camera.look_pending -= step;
    camera.yaw += step.x * camera.look_speed;
    camera.pitch -= step.y * camera.look_speed;
    camera.pitch = camera.pitch.clamp(-89.0, 89.0);

    if !input.active {
        camera.controlling = false;
        return;
    }
    camera.controlling = true;

    let forward = camera.forward();
    let right = forward.cross(Vec3::Y).normalize();
    let world_up = Vec3::Y;
    let speed = camera.move_speed * dt;
    if input.forward {
        camera.position += forward * speed;
    }
    if input.back {
        camera.position -= forward * speed;
    }
    if input.right {
        camera.position += right * speed;
    }
    if input.left {
        camera.position -= right * speed;
    }
    if input.up {
        camera.position += world_up * speed;
    }
    if input.down {
        camera.position -= world_up * speed;
    }
}

/// Wraps an `f32` as a JSON number from its f64 promotion (the nlohmann `float → double`
/// insert) — the byte-equality seam every scalar goes through.
fn f32_value(value: f32) -> Value {
    Value::from(f64::from(value))
}

/// A named-object `vec3` → `{"x","y","z"}` (the C++ `vec3ToJson`), matching the scene serde.
fn vec3_to_json(v: Vec3) -> Value {
    Value::Object(Map::from_iter([
        ("x".to_string(), f32_value(v.x)),
        ("y".to_string(), f32_value(v.y)),
        ("z".to_string(), f32_value(v.z)),
    ]))
}

/// Reads a `vec3` from a named object, each component defaulting to `0` (the C++
/// `vec3FromJson`).
fn vec3_from_json(j: &Value) -> Vec3 {
    Vec3::new(
        json_f32_or(j, "x", 0.0),
        json_f32_or(j, "y", 0.0),
        json_f32_or(j, "z", 0.0),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn vec3_close(a: Vec3, b: Vec3, eps: f32) {
        assert!(a.distance(b) < eps, "{a:?} != {b:?}");
    }

    #[test]
    fn forward_at_yaw_zero_looks_down_neg_z() {
        let cam = SceneEditCamera {
            yaw: 0.0,
            pitch: 0.0,
            ..SceneEditCamera::default()
        };
        // cos0·sin0 = 0, sin0 = 0, -cos0·cos0 = -1.
        vec3_close(cam.forward(), Vec3::NEG_Z, 1e-6);
    }

    #[test]
    fn forward_matches_cpp_at_a_sample_yaw_pitch() {
        let cam = SceneEditCamera {
            yaw: -37.0,
            pitch: -29.0,
            ..SceneEditCamera::default()
        };
        // cos(pitch)·sin(yaw), sin(pitch), -cos(pitch)·cos(yaw), then normalized — the
        // C++ `sceneEditCameraForward` values for the default eye angles.
        let yaw = (-37.0f32).to_radians();
        let pitch = (-29.0f32).to_radians();
        let expected = Vec3::new(
            pitch.cos() * yaw.sin(),
            pitch.sin(),
            -pitch.cos() * yaw.cos(),
        )
        .normalize();
        vec3_close(cam.forward(), expected, 1e-6);
        // It is a unit vector (the C++ normalizes).
        assert!((cam.forward().length() - 1.0).abs() < 1e-6);
    }

    #[test]
    fn view_is_look_at_from_eye_along_forward() {
        let cam = SceneEditCamera::default();
        let view = cam.view();
        let expected = Mat4::look_at_rh(cam.position, cam.position + cam.forward(), Vec3::Y);
        assert_eq!(view.view, expected);
        assert_eq!(view.fov, cam.fov);
        assert_eq!(view.near_plane, cam.near_plane);
        assert_eq!(view.far_plane, cam.far_plane);
    }

    #[test]
    fn wasd_moves_the_position_along_the_basis() {
        let mut cam = SceneEditCamera {
            yaw: 0.0,
            pitch: 0.0,
            look_pending: Vec2::ZERO,
            ..SceneEditCamera::default()
        };
        let start = cam.position;
        let dt = 0.1;
        let input = SceneEditCameraInput {
            active: true,
            forward: true,
            ..SceneEditCameraInput::default()
        };
        update_scene_edit_camera(&mut cam, &input, dt);
        // At yaw/pitch 0 forward is -Z; W advances along it by move_speed·dt.
        let expected = start + Vec3::NEG_Z * (cam.move_speed * dt);
        vec3_close(cam.position, expected, 1e-5);
        assert!(cam.controlling, "active latches controlling");
    }

    #[test]
    fn up_and_down_move_along_world_y() {
        let mut cam = SceneEditCamera {
            look_pending: Vec2::ZERO,
            ..SceneEditCamera::default()
        };
        let y0 = cam.position.y;
        let dt = 0.1;
        update_scene_edit_camera(
            &mut cam,
            &SceneEditCameraInput {
                active: true,
                up: true,
                ..SceneEditCameraInput::default()
            },
            dt,
        );
        assert!(cam.position.y > y0, "Space moves up world Y");
        let y1 = cam.position.y;
        update_scene_edit_camera(
            &mut cam,
            &SceneEditCameraInput {
                active: true,
                down: true,
                ..SceneEditCameraInput::default()
            },
            dt,
        );
        assert!((cam.position.y - (y1 - cam.move_speed * dt)).abs() < 1e-5);
    }

    #[test]
    fn look_drains_monotonically_toward_the_pending_target() {
        let mut cam = SceneEditCamera {
            yaw: 0.0,
            pitch: 0.0,
            look_pending: Vec2::ZERO,
            ..SceneEditCamera::default()
        };
        // One look sample of +x (yaw) pixels arrives, then we drain with no further input.
        let dt = 1.0 / 60.0;
        let target_yaw = 100.0 * cam.look_speed;
        update_scene_edit_camera(
            &mut cam,
            &SceneEditCameraInput {
                active: true,
                look_delta: Vec2::new(100.0, 0.0),
                ..SceneEditCameraInput::default()
            },
            dt,
        );
        let mut prev = cam.yaw;
        // The remaining pixels keep draining each frame, converging monotonically toward
        // the full applied yaw without overshoot.
        for _ in 0..200 {
            update_scene_edit_camera(&mut cam, &SceneEditCameraInput::default(), dt);
            assert!(cam.yaw >= prev - 1e-6, "yaw advances monotonically");
            assert!(cam.yaw <= target_yaw + 1e-3, "no overshoot past the target");
            prev = cam.yaw;
        }
        assert!(
            (cam.yaw - target_yaw).abs() < 1e-2,
            "the drain converges to the full sample ({} vs {target_yaw})",
            cam.yaw
        );
        assert!(
            cam.look_pending.length() < 1e-2,
            "the pending look drains out"
        );
    }

    #[test]
    fn pitch_clamps_to_plus_minus_89() {
        let mut cam = SceneEditCamera {
            pitch: 0.0,
            look_pending: Vec2::ZERO,
            ..SceneEditCamera::default()
        };
        // A large sustained down-look (positive look_delta.y lowers pitch) over many frames
        // must clamp at -89.
        for _ in 0..2000 {
            update_scene_edit_camera(
                &mut cam,
                &SceneEditCameraInput {
                    active: true,
                    look_delta: Vec2::new(0.0, 1000.0),
                    ..SceneEditCameraInput::default()
                },
                1.0 / 60.0,
            );
        }
        assert!((cam.pitch - (-89.0)).abs() < 1e-3, "pitch clamps at -89");

        // And the other extreme.
        for _ in 0..2000 {
            update_scene_edit_camera(
                &mut cam,
                &SceneEditCameraInput {
                    active: true,
                    look_delta: Vec2::new(0.0, -1000.0),
                    ..SceneEditCameraInput::default()
                },
                1.0 / 60.0,
            );
        }
        assert!((cam.pitch - 89.0).abs() < 1e-3, "pitch clamps at +89");
    }

    #[test]
    fn controlling_latches_and_unlatches_with_active() {
        let mut cam = SceneEditCamera::default();
        assert!(!cam.controlling);
        update_scene_edit_camera(
            &mut cam,
            &SceneEditCameraInput {
                active: true,
                ..SceneEditCameraInput::default()
            },
            1.0 / 60.0,
        );
        assert!(cam.controlling, "active latches");
        update_scene_edit_camera(&mut cam, &SceneEditCameraInput::default(), 1.0 / 60.0);
        assert!(!cam.controlling, "inactive unlatches");
    }

    #[test]
    fn inactive_still_drains_the_look_tail() {
        let mut cam = SceneEditCamera {
            yaw: 0.0,
            look_pending: Vec2::new(50.0, 0.0),
            ..SceneEditCamera::default()
        };
        let before = cam.yaw;
        // No active hold, but a queued look pending still eases out.
        update_scene_edit_camera(&mut cam, &SceneEditCameraInput::default(), 1.0 / 60.0);
        assert!(cam.yaw > before, "the tail keeps draining while inactive");
        assert!(!cam.controlling);
    }

    #[test]
    fn json_round_trips_position_yaw_pitch_fov() {
        let cam = SceneEditCamera {
            position: Vec3::new(1.5, -2.0, 3.25),
            yaw: 12.5,
            pitch: -7.0,
            fov: 60.0,
            ..SceneEditCamera::default()
        };
        let json = cam.to_json();
        // The exact key set.
        let object = json.as_object().unwrap();
        assert_eq!(object.len(), 4);
        assert!(object.contains_key("position"));
        assert!(object.contains_key("yaw"));
        assert!(object.contains_key("pitch"));
        assert!(object.contains_key("fov"));
        // position is a named {x,y,z} object.
        let position = object["position"].as_object().unwrap();
        assert_eq!(position.len(), 3);
        assert!(position.contains_key("x"));

        let mut restored = SceneEditCamera::default();
        restored.from_json(&json);
        vec3_close(restored.position, cam.position, 1e-6);
        assert!((restored.yaw - cam.yaw).abs() < 1e-6);
        assert!((restored.pitch - cam.pitch).abs() < 1e-6);
        assert!((restored.fov - cam.fov).abs() < 1e-6);
    }

    #[test]
    fn json_partial_leaves_unset_fields_unchanged() {
        let mut cam = SceneEditCamera {
            position: Vec3::new(9.0, 9.0, 9.0),
            yaw: 11.0,
            pitch: 22.0,
            fov: 33.0,
            ..SceneEditCamera::default()
        };
        // Only yaw is present; everything else keeps its current value.
        let partial = serde_json::json!({ "yaw": 45.0 });
        cam.from_json(&partial);
        assert!((cam.yaw - 45.0).abs() < 1e-6, "yaw is overwritten");
        assert_eq!(cam.position, Vec3::new(9.0, 9.0, 9.0), "position untouched");
        assert!((cam.pitch - 22.0).abs() < 1e-6, "pitch untouched");
        assert!((cam.fov - 33.0).abs() < 1e-6, "fov untouched");
    }

    #[test]
    fn json_non_object_is_ignored() {
        let mut cam = SceneEditCamera {
            yaw: 5.0,
            ..SceneEditCamera::default()
        };
        cam.from_json(&Value::Null);
        assert!((cam.yaw - 5.0).abs() < 1e-6);
        cam.from_json(&serde_json::json!(42));
        assert!((cam.yaw - 5.0).abs() < 1e-6);
    }
}
