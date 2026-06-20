//! The host-callback POD seam: the [`ScriptHostBridge`] trait and the two Jolt-free
//! POD structs ([`ScriptRayHit`], [`ScriptRagdollState`]) the physics-reaching bindings
//! exchange with the host.
//!
//! Ports the eleven C++ `std::function` bridges (`raycast`/`sphereCast`/`applyImpulse`/
//! `addForce`/`setVelocity`/`getVelocity`/`setRagdollEnabled`/`setRagdollBlend`/
//! `ragdollState` + `logSink`, `script.cppm:136`–148). C++ kept `Saffron.Script` off a
//! physics/sceneedit module edge by routing every physics reach through POD closures the
//! host installed; in Rust that is one [`ScriptHostBridge`] trait with one method per
//! bridge over POD args (`glam::Vec3`, [`Uuid`], the two POD structs), so this crate stays
//! `saffron-core` + `saffron-scene` only — the host (which *does* depend on physics +
//! sceneedit) implements it.
//!
//! "Unset = a safe no-op" (`script_runtime.cpp:525`): [`ScriptHost`](crate::ScriptHost)
//! defaults its bridge to [`NoopBridge`], so a session without a host-installed bridge
//! (every unit test, an Edit-mode read) sees `raycast` miss, `get_velocity` return zero,
//! and the ragdoll/log calls no-op — never a panic.

use glam::Vec3;

use saffron_core::Uuid;

/// A physics ray/sphere hit surfaced to Lua, Jolt-free POD.
///
/// The host fills it from `World::raycast`/`sphere_cast` (a plain field copy off the
/// physics crate's `RayHit`); this keeps `saffron-script` free of a physics edge — the
/// `sa.raycast`/`sa.spherecast` binding only ever sees this POD, then shapes it into the
/// `{hit, distance, point, normal, entity}` Lua table. Ports the C++ `ScriptRayHit`
/// (`script.cppm:99`–112), with the six loose `px/py/pz/nx/ny/nz` floats folded into two
/// `glam::Vec3` (the interface stays glam-POD).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ScriptRayHit {
    /// Whether the ray/sweep hit anything.
    pub hit: bool,
    /// The owner-entity uuid of the struck body (`Uuid(0)` = none).
    pub entity: Uuid,
    /// World-space contact point.
    pub point: Vec3,
    /// World-space surface normal at the hit.
    pub normal: Vec3,
    /// Distance along the ray from the origin.
    pub distance: f32,
}

/// A rig's live ragdoll state surfaced to Lua, Jolt-free POD.
///
/// The host fills it from `World::ragdoll_state`; `sa.Entity:ragdoll_state()` shapes it
/// into the `{present, active, body_weight, bones}` Lua table. Ports the C++
/// `ScriptRagdollState` (`script.cppm:91`–97).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ScriptRagdollState {
    /// `true` when the rig has a live ragdoll instance this play session.
    pub present: bool,
    /// `true` when the ragdoll's motors drive toward the animation (active vs passive).
    pub active: bool,
    /// The mean per-bone target weight (`0` = pure animation, `1` = pure physics).
    pub body_weight: f32,
    /// The ragdoll's bone count.
    pub bones: i32,
}

/// The host-callback seam the physics-reaching `sa.*` bindings dispatch through.
///
/// One method per C++ `std::function` bridge, over POD args only — so `saffron-script`
/// reaches the live physics world and the editor's script-log ring without importing
/// `saffron-physics` or `saffron-sceneedit`. The host implements it (`saffron-host`),
/// routing each method to a `World`/edit-context call; an unset bridge is
/// [`NoopBridge`] (the C++ "unset = a safe no-op").
///
/// `ScriptHost` holds the installed bridge as an `Rc<dyn ScriptHostBridge>` and lends it
/// to the session for the duration of a start/tick/contact call — the bindings reach it
/// through [`crate::session::with_bridge`]. It is read-only during a session, so it
/// crosses as a shared `Rc` clone (conventions §3 bucket 1), not a moved value.
pub trait ScriptHostBridge {
    /// Cast a ray `origin + dir * max_dist` against the live world (the C++
    /// `host.raycast`). A miss returns [`ScriptRayHit::default`].
    fn raycast(&self, origin: Vec3, dir: Vec3, max_dist: f32) -> ScriptRayHit;

    /// Sweep a sphere of `radius` along `origin + dir * max_dist` — a thicker probe than
    /// [`Self::raycast`] (the C++ `host.sphereCast`).
    fn sphere_cast(&self, origin: Vec3, dir: Vec3, radius: f32, max_dist: f32) -> ScriptRayHit;

    /// Apply a center-of-mass impulse to the Dynamic body owned by `entity` (the C++
    /// `host.applyImpulse`). A non-Dynamic / unmapped body is a no-op on the host side.
    fn apply_impulse(&self, entity: Uuid, impulse: Vec3);

    /// Add a continuous force (applied over the next step) to `entity`'s Dynamic body
    /// (the C++ `host.addForce`).
    fn add_force(&self, entity: Uuid, force: Vec3);

    /// Set the absolute linear velocity of `entity`'s Dynamic body (the C++
    /// `host.setVelocity`).
    fn set_velocity(&self, entity: Uuid, velocity: Vec3);

    /// The current linear velocity of `entity`'s Dynamic body, or zero when there is
    /// none (the C++ `host.getVelocity`).
    fn get_velocity(&self, entity: Uuid) -> Vec3;

    /// Go limp / restore the rig identified by `rig` (the C++ `host.setRagdollEnabled`);
    /// returns whether the toggle succeeded.
    fn set_ragdoll_enabled(&self, rig: Uuid, enable: bool) -> bool;

    /// Blend a rig between physics and animation: `active` arms/releases the motors,
    /// `body_weight` sets the global target weight (the C++ `host.setRagdollBlend`).
    fn set_ragdoll_blend(&self, rig: Uuid, active: bool, body_weight: f32);

    /// The rig's live ragdoll state (the C++ `host.ragdollState`).
    fn ragdoll_state(&self, rig: Uuid) -> ScriptRagdollState;

    /// Route a `sa.log(...)` line to the editor's script-log ring, tagged with the uuid
    /// of the instance whose handler is running (the C++ `host.logSink`). Called *after*
    /// the engine log, so a no-op sink still writes the console.
    fn log_sink(&self, sender: Uuid, message: &str);
}

/// The default bridge: every method is a safe no-op (a missed raycast, zero velocity, a
/// dropped log line). Installed on a fresh [`ScriptHost`](crate::ScriptHost) so a session
/// without a host-installed bridge degrades cleanly — the C++ "unset = a safe no-op".
#[derive(Clone, Copy, Debug, Default)]
pub struct NoopBridge;

impl ScriptHostBridge for NoopBridge {
    fn raycast(&self, _origin: Vec3, _dir: Vec3, _max_dist: f32) -> ScriptRayHit {
        ScriptRayHit::default()
    }

    fn sphere_cast(&self, _origin: Vec3, _dir: Vec3, _radius: f32, _max_dist: f32) -> ScriptRayHit {
        ScriptRayHit::default()
    }

    fn apply_impulse(&self, _entity: Uuid, _impulse: Vec3) {}

    fn add_force(&self, _entity: Uuid, _force: Vec3) {}

    fn set_velocity(&self, _entity: Uuid, _velocity: Vec3) {}

    fn get_velocity(&self, _entity: Uuid) -> Vec3 {
        Vec3::ZERO
    }

    fn set_ragdoll_enabled(&self, _rig: Uuid, _enable: bool) -> bool {
        false
    }

    fn set_ragdoll_blend(&self, _rig: Uuid, _active: bool, _body_weight: f32) {}

    fn ragdoll_state(&self, _rig: Uuid) -> ScriptRagdollState {
        ScriptRagdollState::default()
    }

    fn log_sink(&self, _sender: Uuid, _message: &str) {}
}

#[cfg(test)]
mod tests {
    /// The crate-boundary contract (README §1): `saffron-script` stays
    /// `saffron-core` + `saffron-scene` only — the physics reach crosses the POD
    /// [`super::ScriptHostBridge`] seam, never a `saffron-physics`/`saffron-animation`
    /// dependency edge. This guards the manifest against an accidental edge a future
    /// change might add (which would defeat the whole point of the bridge).
    #[test]
    fn no_physics_or_animation_dependency_edge() {
        let manifest = include_str!("../Cargo.toml");
        let deps = manifest
            .split("[dependencies]")
            .nth(1)
            .expect("a [dependencies] section");
        // Stop at the next section header so dev-deps / other tables do not count.
        let deps = deps.split("\n[").next().unwrap_or(deps);
        for forbidden in ["saffron-physics", "saffron-animation", "saffron-sceneedit"] {
            assert!(
                !deps.contains(forbidden),
                "saffron-script must not depend on {forbidden} (the bridge POD seam crosses it)"
            );
        }
    }
}
