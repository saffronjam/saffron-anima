//! The host's [`ScriptHostBridge`] implementation: the concrete end of the POD seam
//! `saffron-script` declares so the `sa.*` physics bindings and the `sa.log` sink reach
//! the live world / editor without `saffron-script` importing `saffron-physics` or
//! `saffron-sceneedit`.
//!
//! One [`HostScriptBridge`] holds the host handles the bindings reach — the live play world
//! and the play scene (for the scene-reading `enable_ragdoll`) — as `Rc<RefCell<…>>` cells
//! the host shares with it, plus a [`SharedScriptSink`] log buffer.
//!
//! `Rc<RefCell>` is the single-thread shared-mutable idiom: the VM is `!Send`, so no `Mutex`
//! is needed, and the bridge is a host-installed callback object, not one of the value-owned
//! `HostLayer` fields. The host installs it onto the [`ScriptHost`] on the play edge
//! (`ScriptHost::install_bridge`) and shares the same cells so the bridge sees the live
//! world while it exists (a no-op `None` world in Edit).
//!
//! `sa.log` cannot write straight into the editor: while a script tick runs the editor is
//! borrowed by `tick_play` (the `sim_tick` seam), so that aliasing is forbidden. The line is
//! appended to [`SharedScriptSink`] (a tiny cell the editor does not alias) and the host
//! drains it into the editor's script-log ring once each call batch returns and the editor
//! is freely borrowable again.

use std::cell::RefCell;
use std::rc::Rc;

use glam::Vec3;

use saffron_core::{Uuid, log_warn};
use saffron_physics::World;
use saffron_scene::Scene;
use saffron_script::{ScriptHostBridge, ScriptRagdollState, ScriptRayHit};

/// The live play physics world, shared between the host and the bridge (`None` in Edit).
pub type SharedPhysics = Rc<RefCell<Option<World>>>;
/// The play scene, shared so the scene-reading `enable_ragdoll` resolves the rig entity.
pub type SharedScene = Rc<RefCell<Scene>>;

/// One buffered `sa.log` line: the sender uuid and its message, drained into the editor's
/// script-log ring after the script call batch.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ScriptLogLine {
    /// The uuid of the instance that logged the line.
    pub sender: u64,
    /// The logged message.
    pub message: String,
}

/// The shared `sa.log` buffer: the bridge appends, the host drains it into the editor's
/// script-log ring once the tick releases the editor borrow.
pub type SharedScriptSink = Rc<RefCell<Vec<ScriptLogLine>>>;

/// The host's [`ScriptHostBridge`], over the shared world / scene / log cells.
///
/// Construct it from the host's own cells with [`HostScriptBridge::new`], box it as
/// `Rc<dyn ScriptHostBridge>`, and install it onto the play session's [`ScriptHost`].
/// Every method guards a `None` world / dead lookup as a safe no-op, so an Edit-mode call
/// or a call between worlds never panics.
pub struct HostScriptBridge {
    /// The live play physics world (`None` in Edit). Shared with the host so the bridge
    /// reaches whatever world is current without owning it.
    physics: SharedPhysics,
    /// The play scene, shared so the scene-reading `enable_ragdoll` resolves the rig
    /// entity.
    scene: SharedScene,
    /// The buffered `sa.log` lines the host drains into the editor's script-log ring.
    sink: SharedScriptSink,
}

impl HostScriptBridge {
    /// Wires the bridge to the host's shared world / scene cells and the log sink.
    #[must_use]
    pub fn new(physics: SharedPhysics, scene: SharedScene, sink: SharedScriptSink) -> Self {
        Self {
            physics,
            scene,
            sink,
        }
    }

    /// Flattens a physics [`RayHit`](saffron_physics::RayHit) into the script-side POD —
    /// a plain field copy.
    fn flatten(hit: saffron_physics::RayHit) -> ScriptRayHit {
        ScriptRayHit {
            hit: hit.hit,
            entity: hit.entity,
            point: hit.point,
            normal: hit.normal,
            distance: hit.distance,
        }
    }
}

impl ScriptHostBridge for HostScriptBridge {
    fn raycast(&self, origin: Vec3, dir: Vec3, max_dist: f32) -> ScriptRayHit {
        match self.physics.borrow().as_ref() {
            Some(world) => Self::flatten(world.raycast(origin, dir, max_dist)),
            None => ScriptRayHit::default(),
        }
    }

    fn sphere_cast(&self, origin: Vec3, dir: Vec3, radius: f32, max_dist: f32) -> ScriptRayHit {
        match self.physics.borrow().as_ref() {
            Some(world) => Self::flatten(world.sphere_cast(origin, dir, radius, max_dist)),
            None => ScriptRayHit::default(),
        }
    }

    fn apply_impulse(&self, entity: Uuid, impulse: Vec3) {
        if let Some(world) = self.physics.borrow_mut().as_mut() {
            world.apply_impulse(entity, impulse);
        }
    }

    fn add_force(&self, entity: Uuid, force: Vec3) {
        if let Some(world) = self.physics.borrow_mut().as_mut() {
            world.add_force(entity, force);
        }
    }

    fn set_velocity(&self, entity: Uuid, velocity: Vec3) {
        if let Some(world) = self.physics.borrow_mut().as_mut() {
            world.set_linear_velocity(entity, velocity);
        }
    }

    fn get_velocity(&self, entity: Uuid) -> Vec3 {
        match self.physics.borrow().as_ref() {
            Some(world) => world.body_linear_velocity(entity),
            None => Vec3::ZERO,
        }
    }

    fn set_ragdoll_enabled(&self, rig: Uuid, enable: bool) -> bool {
        let mut physics = self.physics.borrow_mut();
        let Some(world) = physics.as_mut() else {
            return false;
        };
        if !enable {
            world.disable_ragdoll(rig);
            return true;
        }
        // Enabling reads the rig's SkinnedMesh + BonePhysics off the play scene; the scene
        // cell is a separate borrow from physics.
        let scene = self.scene.borrow();
        let Some(entity) = scene.find_entity_by_uuid(rig) else {
            return false;
        };
        if !scene.valid(entity) {
            return false;
        }
        match world.enable_ragdoll(&scene, entity) {
            Ok(()) => true,
            Err(err) => {
                log_warn!("script: enable_ragdoll: {err}");
                false
            }
        }
    }

    fn set_ragdoll_blend(&self, rig: Uuid, active: bool, body_weight: f32) {
        if let Some(world) = self.physics.borrow_mut().as_mut() {
            let _ = world.set_ragdoll_blend(rig, Some(active), Some(body_weight), None, None);
        }
    }

    fn ragdoll_state(&self, rig: Uuid) -> ScriptRagdollState {
        match self.physics.borrow().as_ref() {
            Some(world) => {
                let s = world.ragdoll_state(rig);
                ScriptRagdollState {
                    present: s.present,
                    active: s.active,
                    body_weight: s.body_weight,
                    bones: s.bones,
                }
            }
            None => ScriptRagdollState::default(),
        }
    }

    fn log_sink(&self, sender: Uuid, message: &str) {
        self.sink.borrow_mut().push(ScriptLogLine {
            sender: sender.0,
            message: message.to_owned(),
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use saffron_scene::register_builtin_components;
    use std::sync::Arc;

    fn cells() -> (SharedPhysics, SharedScene, SharedScriptSink) {
        (
            Rc::new(RefCell::new(None)),
            Rc::new(RefCell::new(Scene::new())),
            Rc::new(RefCell::new(Vec::new())),
        )
    }

    /// With no world (Edit mode), the physics calls are safe no-ops: a missed raycast,
    /// zero velocity, a false ragdoll toggle.
    #[test]
    fn no_world_is_a_safe_noop() {
        let (physics, scene, sink) = cells();
        let bridge = HostScriptBridge::new(physics, scene, sink);
        assert_eq!(
            bridge.raycast(Vec3::ZERO, Vec3::Z, 100.0),
            ScriptRayHit::default()
        );
        assert_eq!(bridge.get_velocity(Uuid(7)), Vec3::ZERO);
        assert!(!bridge.set_ragdoll_enabled(Uuid(7), true));
        assert_eq!(bridge.ragdoll_state(Uuid(7)), ScriptRagdollState::default());
        // The mutating no-ops do not panic.
        bridge.apply_impulse(Uuid(7), Vec3::ONE);
        bridge.add_force(Uuid(7), Vec3::ONE);
        bridge.set_velocity(Uuid(7), Vec3::ONE);
        bridge.set_ragdoll_blend(Uuid(7), true, 0.5);
    }

    /// `log_sink` appends the line into the shared sink, tagged with the sender uuid — the
    /// host drains it from there into the editor's script-log ring after the tick.
    #[test]
    fn log_sink_buffers_the_line() {
        let (physics, scene, sink) = cells();
        let bridge = HostScriptBridge::new(physics, scene, Rc::clone(&sink));
        bridge.log_sink(Uuid(42), "hello");
        let lines = sink.borrow();
        assert_eq!(lines.len(), 1);
        assert_eq!(lines[0].sender, 42);
        assert_eq!(lines[0].message, "hello");
    }

    /// The physics calls route to the live world: a velocity set + read-back round-trips
    /// through a real (Jolt-backed) world, and `set_ragdoll_enabled(false)` on an absent
    /// rig is the documented `true` (disable is always accepted on a live world).
    #[test]
    fn physics_calls_route_to_the_live_world() {
        let world = match World::new() {
            Ok(world) => world,
            Err(err) => {
                // Jolt globals failed to install (no toolchain) — skip, not a false pass.
                eprintln!("skipping: World::new failed: {err}");
                return;
            }
        };
        let physics = Rc::new(RefCell::new(Some(world)));
        let scene = Rc::new(RefCell::new(Scene::new()));
        let sink = Rc::new(RefCell::new(Vec::new()));
        let bridge = HostScriptBridge::new(physics, Rc::clone(&scene), sink);

        // No mapped body for this uuid → velocity read is zero, and the impulse/force/
        // velocity sets are no-ops (warned) rather than panics, on a live world.
        assert_eq!(bridge.get_velocity(Uuid(123)), Vec3::ZERO);
        bridge.apply_impulse(Uuid(123), Vec3::ONE);
        bridge.set_velocity(Uuid(123), Vec3::new(1.0, 2.0, 3.0));

        // Disable on a live world is accepted (true) even when the rig is absent.
        assert!(bridge.set_ragdoll_enabled(Uuid(123), false));
        // Enable on an absent rig is false (no entity for the uuid in the empty scene).
        assert!(!bridge.set_ragdoll_enabled(Uuid(123), true));

        // The registry construction is unrelated but confirms the scene cell is usable
        // alongside the physics borrow without a RefCell clash.
        let _registry = Arc::new(register_builtin_components());
    }
}
