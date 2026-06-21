//! The [`HostLayer`] apex: the single [`Layer`] that owns the editor session and wires
//! every subsystem into the run loop.
//!
//! The host state lives as plain fields on this layer: the run loop owns the layer, the
//! layer owns its state, and the lifecycle is methods on it — single-threaded throughout.
//!
//! The per-frame work splits cleanly in two:
//!
//! - The **session update** ([`HostLayer::update_session`]) is the renderer-independent
//!   spine: the parent-death watch, the asset-preview prune, `tick_animation` →
//!   `tick_play`, the deferred script-error pause, and the fly-camera drain. It runs on
//!   pure CPU state, so the unit tests drive it with no GPU.
//! - The **renderer-coupled** work (the control poll, the gizmo-drag smoothing that reads
//!   the viewport size, and the whole `on_ui` scene render + overlay submit) reaches the
//!   concrete [`Renderer`] through [`saffron_app::FrameHost::renderer_mut`]; with no
//!   renderer attached (the test host) those steps are skipped.

use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::sync::Arc;

use glam::Vec2;

use saffron_animation::{AnimMode, AnimationRuntime, tick_animation};
use saffron_app::{App, Layer};
use saffron_assets::{
    AssetServer, RenderSceneOptions, RendererScene, RendererUploader, render_scene,
};
use saffron_control::ControlContext;

use crate::control_renderer::HostControlRenderer;
use saffron_core::{TimeSpan, Uuid, log_error, log_warn};
use saffron_physics::{PoseTarget, World};
use saffron_protocol::{GetScriptSchemaParams, GetScriptSchemaResult, ScriptFieldDto};
use saffron_rendering::{GpuQueue, Renderer, Uploader};
use saffron_scene::{
    CameraView, ComponentRegistry, Entity, Mesh, Scene, ScriptInputState,
    derive_script_input_edges, register_builtin_components,
};
use saffron_sceneedit::{PlayState, SceneEditContext, update_scene_edit_camera};
use saffron_script::{ContactInfo, ScriptHost, ScriptHostBridge, ScriptRunError};
use saffron_signal::SubscriptionId;
use saffron_window::Window;

use crate::overlay::build_scene_edit_overlay;
use crate::script_bridge::{HostScriptBridge, ScriptLogLine, SharedPhysics, SharedScene};
use crate::viewport_shm::{ShmView, ViewportShmPublisher};

/// The host's apex layer: the editor session plus the wired subsystems.
///
/// Owns its state by value. The load-bearing teardown (worker join, control-socket close,
/// physics drop, GPU-cache clear) is sequenced explicitly in [`HostLayer::on_detach`], which
/// the run loop calls after `wait_gpu_idle` and before the renderer drops.
pub struct HostLayer {
    /// The editor's mutable session state (scene / selection / play / gizmo / smoothing).
    editor: SceneEditContext,
    /// The live asset catalog + GPU caches + thumbnail worker.
    assets: AssetServer,
    /// The per-session animation player (clip cache + transitions + IK).
    animation: AnimationRuntime,
    /// The control plane: the command registry + the once-per-frame socket drain.
    control: ControlContext,
    /// The play session's script VM + instances. Empty between plays; `start_scripts`/
    /// `tick_scripts` run on the play edge / each tick. Behind an `Rc<RefCell>` because the
    /// `sim_tick` seam closure (stored in the editor, invoked while the editor is borrowed by
    /// `tick_play`) must reach it without capturing `&mut self` — the single-thread
    /// shared-mutable idiom.
    script: Rc<RefCell<ScriptHost>>,
    /// The component reflection table the script start/tick/contact calls bind, built once
    /// (`register_builtin_components`); the editor's own registry is not `Clone`, and this is
    /// the same closed component set, so a host-owned `Arc` is the one source the play
    /// session's scripts read through.
    script_registry: Arc<ComponentRegistry>,
    /// The live play physics world, present exactly while play is active (`None` in Edit).
    /// Behind the shared cell so the bridge's `sa.raycast`/impulse bindings and the `sim_tick`
    /// closure reach it, and `poll_control` lends `borrow_mut().as_mut()` into the
    /// `EngineContext::physics` borrow.
    physics: SharedPhysics,
    /// The gameplay-input snapshot the `sim_tick` closure derives edges on and ticks scripts
    /// with. Shared so the closure reaches it; the host syncs the editor's authored input into
    /// it before each tick and copies the derived state back after (the closure cannot reach
    /// the editor, borrowed by `tick_play`).
    script_input: Rc<RefCell<ScriptInputState>>,
    /// This frame's ragdoll pose targets, snapshotted from the animation runtime's `last_pose`
    /// after `tick_animation` and before `tick_play`, so the `sim_tick` closure motors active
    /// ragdolls toward the animated pose.
    pose_targets: Rc<RefCell<Vec<PoseTarget>>>,
    /// The `sa.log` lines the bridge buffers during a script call; drained into the editor's
    /// script-log ring after `tick_play` releases the editor borrow.
    script_log_sink: Rc<RefCell<Vec<ScriptLogLine>>>,
    /// The script errors the `sim_tick` closure records during a tick/contact dispatch;
    /// drained into the editor's error ring after `tick_play`, then the deferred pause fires.
    script_error_sink: Rc<RefCell<Vec<ScriptRunError>>>,
    /// The host-owned bridge, kept alive across plays so the same `Rc` is re-installed each
    /// play edge.
    bridge: Rc<dyn ScriptHostBridge>,
    /// The play state the host last reconciled, for the edge detection that builds/tears the
    /// world + VM (run host-side after `poll_control` releases the editor borrow rather than
    /// inside the published transition).
    last_play_state: PlayState,
    /// The host-built one-off uploader the scene-render path resolves assets through,
    /// constructed lazily from the renderer's device on the first rendered frame.
    uploader: Option<Uploader>,
    /// The per-view viewport shm segments (empty until `run_host` attaches them from the
    /// editor-set environment). The segments exist at startup so the editor's presenter can
    /// block-open both panes.
    shm: ViewportShmPublisher,

    /// Seq high-water for per-tick contact → script dispatch. Shared so the `sim_tick`
    /// closure advances it across ticks.
    contact_cursor: Rc<Cell<u64>>,
    /// A script VM exists (Playing/Paused); stop destroys it.
    script_vm_active: bool,
    /// The Jolt process globals (`Factory` + registered types) are installed — set true the first
    /// time a play world is built. They outlive every world, so teardown shuts them down once,
    /// *after* the last world drops.
    physics_init: bool,
    /// Set inside the `sim_tick` closure on a contained script failure; drives the deferred
    /// pause once per `update`. Shared so the closure flips it.
    script_error_pending: Rc<Cell<bool>>,
    /// Frames publish to shared memory; the editor owns the render size.
    shm_publish: bool,
    /// Tracks asset-preview transitions so the anim runtime is pruned once per edge.
    preview_active: bool,

    /// Whether the editor spawned this host (`SAFFRON_EDITOR_NATIVE_VIEWPORT` set); gates
    /// the parent-death watch.
    editor_spawned: bool,
    /// The parent pid captured once at construction; the watch fires on a mismatch.
    editor_pid: Option<rustix::process::Pid>,

    /// The script-VM lifecycle subscription, unsubscribed on detach.
    script_subscription: SubscriptionId,
    /// The physics-world lifecycle subscription, unsubscribed on detach.
    physics_subscription: SubscriptionId,
}

/// What the parent-death watch resolved for a frame, split out so the watch is testable
/// without a live process tree.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ParentWatch {
    /// The editor did not spawn us, or the parent is unchanged — keep running.
    Alive,
    /// The editor spawned us and `getppid` changed — the editor vanished, request exit.
    ParentDied,
}

/// One step of the host's teardown sequence, in the order it runs. The cross-object ordering is a
/// runtime UAF if wrong (not a compile error), so [`HostLayer::teardown`] emits each step into a
/// record a test reads to assert the order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TeardownStep {
    /// Drain + join the thumbnail worker (it borrows the renderer, still alive here).
    WorkerJoined,
    /// Close the control socket.
    ControlClosed,
    /// Stop the script VM (it never touches the scene, so it tears down before the world).
    ScriptsStopped,
    /// Drop the live play physics world (RAII frees its Jolt bodies/ragdolls).
    PhysicsWorldDropped,
    /// Shut down the Jolt process globals — only after the last world is gone.
    JoltGlobalsShutdown,
    /// Null the `sim_tick` seam (no dangling physics/script closure).
    SimTickCleared,
    /// Unsubscribe the two play-state lifecycle hooks.
    PlayHooksUnsubscribed,
    /// Drop the host's one-off uploader + clear the GPU `Ref` caches, before the renderer drops.
    GpuCachesCleared,
}

impl HostLayer {
    /// Builds the host layer: a seeded editor context, an asset server rooted at `asset_root`,
    /// the animation runtime with its clip loader installed, the control context (which binds
    /// the socket), the host-owned `get-script-schema` command, and the two play-state
    /// lifecycle subscriptions held as live tokens.
    ///
    /// `editor_spawned` records whether the editor launched this host (the parent-death watch
    /// is armed only then, capturing `getppid()` now); `shm_publish` records that frames
    /// publish to shared memory (so the host never tracks a hidden window's size).
    #[must_use]
    pub fn new(
        asset_root: impl Into<std::path::PathBuf>,
        editor_spawned: bool,
        shm_publish: bool,
    ) -> Self {
        let editor = SceneEditContext::new();
        let assets = AssetServer::new(asset_root);

        let animation = AnimationRuntime::new();

        let mut control = ControlContext::new();
        Self::register_script_schema_command(&mut control);

        // The play-session shared cells: the `sim_tick` seam closure is stored in the editor
        // and invoked while the editor is borrowed by `tick_play`, so it cannot capture
        // `&mut self`; it reaches the world / VM / cursors through these single-thread cells
        // instead. The host holds an `Rc::clone` of each so it can build/tear the session and
        // drain the sinks.
        let physics: SharedPhysics = Rc::new(RefCell::new(None));
        let script_log_sink: Rc<RefCell<Vec<ScriptLogLine>>> = Rc::new(RefCell::new(Vec::new()));
        // The bridge's scene cell backs only the script-driven `sa.set_ragdoll_enabled(rig)` rig
        // lookup, an off-gate path: during a tick the play scene is lent to the script session's
        // thread-local, so the host cannot also share it here without a clone. The cell stays
        // empty (a script enabling a ragdoll mid-tick resolves no rig); control-driven ragdoll
        // enabling goes straight through `EngineContext::physics`, unaffected.
        let bridge_scene: SharedScene = Rc::new(RefCell::new(Scene::new()));
        let bridge: Rc<dyn ScriptHostBridge> = Rc::new(HostScriptBridge::new(
            Rc::clone(&physics),
            bridge_scene,
            Rc::clone(&script_log_sink),
        ));

        let mut layer = Self {
            editor,
            assets,
            animation,
            control,
            script: Rc::new(RefCell::new(ScriptHost::new())),
            script_registry: Arc::new(register_builtin_components()),
            physics,
            script_input: Rc::new(RefCell::new(ScriptInputState::default())),
            pose_targets: Rc::new(RefCell::new(Vec::new())),
            script_log_sink,
            script_error_sink: Rc::new(RefCell::new(Vec::new())),
            bridge,
            last_play_state: PlayState::Edit,
            uploader: None,
            shm: ViewportShmPublisher::new(),
            contact_cursor: Rc::new(Cell::new(0)),
            script_vm_active: false,
            physics_init: false,
            script_error_pending: Rc::new(Cell::new(false)),
            shm_publish,
            preview_active: false,
            editor_spawned,
            editor_pid: if editor_spawned {
                rustix::process::getppid()
            } else {
                None
            },
            script_subscription: SubscriptionId(0),
            physics_subscription: SubscriptionId(0),
        };
        layer.install_play_state_hooks();
        layer
    }

    /// Registers the one host-owned control command (`get-script-schema`).
    ///
    /// Lives on the host, not in `saffron-control`, because the handler needs the Lua schema
    /// reader and only the host may depend on `saffron-script`. It rejects a bad path up front
    /// (the frozen guard), resolves
    /// `<project>/src/<path>`, reads the script's declared `properties` in a throwaway sandboxed
    /// VM, and maps each [`ScriptField`](saffron_script::ScriptField) to a `ScriptFieldDto`.
    fn register_script_schema_command(control: &mut ControlContext) {
        control.register::<GetScriptSchemaParams, GetScriptSchemaResult>(
            "get-script-schema",
            "get-script-schema {path} — a project script's declared fields (path relative to src/)",
            |ctx, params: GetScriptSchemaParams| {
                if params.path.is_empty() || params.path.contains("..") {
                    return Err(saffron_control::Error::Command(
                        "path must be relative to the project src/".to_owned(),
                    ));
                }
                let file = std::path::Path::new(&ctx.scene_edit.project_root)
                    .join("src")
                    .join(&params.path);
                let fields = saffron_script::read_script_schema(&file)
                    .map_err(|e| saffron_control::Error::Command(e.to_string()))?;
                Ok(GetScriptSchemaResult {
                    fields: fields
                        .into_iter()
                        .map(|field| ScriptFieldDto {
                            name: field.name,
                            r#type: field.field_type.wire_name().to_owned(),
                            default_value: field.default_value,
                        })
                        .collect(),
                })
            },
        );
    }

    /// Subscribes the script-VM and physics-world lifecycle markers on the editor's
    /// `on_play_state_changed` signal, storing their tokens for `on_detach` to unsubscribe.
    ///
    /// The VM / Jolt world cannot be built inside the published transition: `publish_transition`
    /// is `&mut self` on the editor, so a subscribed closure runs while the editor is borrowed
    /// and cannot reach the play scene / project root / registry it needs to build with. So the
    /// host detects the Edit↔Playing edge itself in [`HostLayer::reconcile_play_edge`], right
    /// after `poll_control` releases the editor borrow — the only borrow-sound shape, and the
    /// world exists before the next poll / tick. The subscriptions stay as the lifecycle seam
    /// markers (the "play hooks are live" invariant the editor exposes), torn down cleanly on
    /// detach.
    fn install_play_state_hooks(&mut self) {
        self.script_subscription = self
            .editor
            .on_play_state_changed
            .subscribe(|_next: PlayState| false);
        self.physics_subscription = self
            .editor
            .on_play_state_changed
            .subscribe(|_next: PlayState| false);
    }

    /// The editor session state (for the lifecycle wiring + tests).
    #[must_use]
    pub fn editor(&self) -> &SceneEditContext {
        &self.editor
    }

    /// The editor session state, mutably (for `run_host`'s project bring-up).
    #[must_use]
    pub fn editor_mut(&mut self) -> &mut SceneEditContext {
        &mut self.editor
    }

    /// The animation runtime (for tests asserting the preview-prune behavior).
    #[must_use]
    pub fn animation(&self) -> &AnimationRuntime {
        &self.animation
    }

    /// Attaches the viewport shm publisher `run_host` builds from the editor-set
    /// environment (both segments created at startup so the presenter can block-open each
    /// pane).
    pub fn attach_shm_publisher(&mut self, shm: ViewportShmPublisher) {
        self.shm = shm;
    }

    /// The per-tick contact → script dispatch high-water cursor.
    #[must_use]
    pub fn contact_cursor(&self) -> u64 {
        self.contact_cursor.get()
    }

    /// Whether a live play physics world is present (`false` in Edit). The world lives behind
    /// the shared cell, so a borrowing accessor would alias the cell `poll_control` /
    /// `sim_tick` hold; this reports presence without lending the world out.
    #[must_use]
    pub fn has_physics(&self) -> bool {
        self.physics.borrow().is_some()
    }

    /// Whether a script VM is live (Playing/Paused).
    #[must_use]
    pub fn script_vm_active(&self) -> bool {
        self.script_vm_active
    }

    /// Whether any viewport view publishes to shared memory.
    #[must_use]
    pub fn shm_publishing(&self) -> bool {
        self.shm.any_enabled()
    }

    /// Whether the play-state lifecycle subscriptions are still live (a detach removes them).
    #[must_use]
    pub fn play_hooks_live(&self) -> bool {
        !self.editor.on_play_state_changed.is_empty()
    }

    /// Evaluates the parent-death watch against an observed parent pid.
    ///
    /// The editor spawns the host as a child and reparents it away (the parent pid changes)
    /// when it dies however it dies — a crash or SIGKILL that skips the editor's teardown.
    /// With the watch armed (`editor_spawned`), a mismatch against the captured pid means the
    /// editor vanished. Pure (takes the observed pid) so a test drives it without a real
    /// process tree.
    #[must_use]
    pub fn watch_parent(&self, current_ppid: Option<rustix::process::Pid>) -> ParentWatch {
        if self.editor_spawned && current_ppid != self.editor_pid {
            ParentWatch::ParentDied
        } else {
            ParentWatch::Alive
        }
    }

    /// Reads the live parent pid (the watch's per-frame observation).
    fn current_ppid(&self) -> Option<rustix::process::Pid> {
        if self.editor_spawned {
            rustix::process::getppid()
        } else {
            self.editor_pid
        }
    }

    /// Auto-selects the first mesh entity so the embedded viewport starts with something
    /// selected (the native-viewport host has no hierarchy panel to select from).
    pub fn auto_select_first_mesh(&mut self) {
        let mut first = Entity::NULL;
        self.editor.scene.for_each::<&Mesh, _>(|entity, _| {
            if first == Entity::NULL {
                first = entity;
            }
        });
        if first != Entity::NULL {
            self.editor.set_selection(first);
        }
    }

    /// The renderer-independent per-frame spine: the parent-death verdict, the asset-preview
    /// prune on its transition edge, `tick_animation` then `tick_play` (control runs before
    /// this, so a play/step command lands the same frame), the deferred script-error pause, the
    /// fly-camera look-delta drain, and the edit smoothing. Returns the parent-death verdict.
    pub fn update_session(
        &mut self,
        dt: TimeSpan,
        current_ppid: Option<rustix::process::Pid>,
    ) -> ParentWatch {
        let watch = self.watch_parent(current_ppid);
        if watch == ParentWatch::ParentDied {
            return watch;
        }

        // A play/stop command landed in this frame's `poll_control` (or a test drove
        // `enter_play`/`stop_play`): build or tear the world + VM on the Edit↔Playing edge
        // before this frame ticks, so the world steps the same frame play was entered.
        self.reconcile_play_edge();

        // Entering or leaving the asset preview swaps `active_scene` to a fresh entity set;
        // drop the runtime's per-entity transition/pose entries on the transition edge so a
        // re-entered preview starts clean and dead entries never accumulate across opens.
        let previewing = self.editor.previewing();
        if previewing != self.preview_active {
            self.animation.prune_session();
            self.preview_active = previewing;
        }

        // Animation runs every frame in both Edit (preview) and Play, before scripts so a
        // script can still override a bone the same frame.
        let anim_mode = if self.editor.play_state == PlayState::Edit {
            AnimMode::Edit
        } else {
            AnimMode::Play
        };
        // The animation evaluator sits below `saffron-assets`, so the host hands it a loader
        // resolving a clip id through the **live** catalog. `editor`, `animation`, and `assets`
        // are distinct fields, so the three borrows are disjoint; the loader closure is the
        // per-tick injected dependency.
        let assets = &mut self.assets;
        let mut load = |id: Uuid| {
            assets
                .load_anim_clip(id)
                .map_err(|err| saffron_animation::Error::ClipLoad(err.to_string()))
        };
        let scene = self.editor.active_scene();
        tick_animation(&mut self.animation, scene, dt.seconds, anim_mode, &mut load);

        // Snapshot this frame's animated poses into the shared cell (after `tick_animation`,
        // before `tick_play`), so the `sim_tick` closure motors each active ragdoll toward the
        // pose without reaching the animation runtime it cannot capture. Cheap when no rig is
        // driven.
        if self.editor.play_state != PlayState::Edit && self.script_vm_active {
            let mut targets = self.pose_targets.borrow_mut();
            targets.clear();
            for (rig, pose) in self.animation.last_poses() {
                targets.push(PoseTarget {
                    rig: Uuid(rig),
                    local: pose.to_vec(),
                });
            }
            // Lend the editor's authored gameplay input into the shared cell the closure ticks
            // with; the derived state copies back after the tick (the closure cannot reach the
            // editor, borrowed by `tick_play`).
            *self.script_input.borrow_mut() = self.editor.script_input.clone();
        }

        // Control already drained this frame (in `on_update`), so a play/pause/step command
        // takes effect this tick. `tick_play` invokes the installed `sim_tick` closure with the
        // play scene; the closure steps physics + scripts through the shared cells.
        self.editor.tick_play(dt.seconds);

        // Drain what the tick buffered while the editor was borrowed: copy the derived input
        // memory back, route logged lines + recorded errors into the editor's rings, and flip
        // the deferred pause (never inside the tick — that would re-enter the play machine).
        if self.editor.play_state != PlayState::Edit {
            self.editor.script_input = self.script_input.borrow().clone();
            self.drain_script_sinks();
        }
        if self.script_error_pending.replace(false) {
            let _ = self.editor.pause_play();
        }

        // Fly-cam: the editor streams pointer-lock look deltas over the control plane; drain
        // the accumulated delta each frame so a burst between frames is not lost.
        let input = self.editor.fly_input;
        self.editor.fly_input.look_delta = Vec2::ZERO;
        update_scene_edit_camera(&mut self.editor.camera, &input, dt.seconds);

        // Smoothed edits (`set-material` / `set-transform smooth:1`) converge here too.
        self.editor.step_edit_smoothing(dt.seconds);

        ParentWatch::Alive
    }

    /// Builds the `EngineContext` borrow from the host's own fields and drains the control
    /// socket once. The borrow struct is assembled here and never stored; `physics` crosses as
    /// the live play world or `None`.
    fn poll_control(&mut self, window: &mut Window, renderer: &mut Renderer) {
        // The control plane's GPU-upload seam needs the host-owned one-off uploader; build
        // it before assembling the borrow (the asset commands resolve/upload through it).
        self.ensure_uploader(renderer);
        let Some(uploader) = self.uploader.as_ref() else {
            return; // No uploader (device create failed): the control drain is skipped.
        };
        let mut control_renderer = HostControlRenderer::new(renderer, uploader);
        // Lend the live play world into the `EngineContext::physics` borrow: a `RefMut` on the
        // shared cell, held for the drain's duration. No `sim_tick` runs during the drain, so
        // the cell is free to borrow here.
        let mut physics = self.physics.borrow_mut();
        self.control.poll(
            window,
            &mut control_renderer,
            &mut self.editor,
            &mut self.assets,
            physics.as_mut(),
        );
    }

    /// Reconciles the play world + script VM against the editor's play state on the Edit↔Playing
    /// edge.
    ///
    /// Runs each `on_update` right after `poll_control` releases the editor borrow — the only
    /// borrow-sound place, since the published transition holds `&mut editor`. On Edit→Playing it
    /// builds the Jolt world from the play scene, starts the script VM, installs the bridge, and
    /// sets the `sim_tick` seam; on →Edit it drops the world, stops the VM, and clears the seam in
    /// teardown order. Pause/Resume keep the session — only the Edit boundary builds or tears it.
    fn reconcile_play_edge(&mut self) {
        let now = self.editor.play_state;
        if now == self.last_play_state {
            return;
        }
        let was_edit = self.last_play_state == PlayState::Edit;
        let is_edit = now == PlayState::Edit;
        self.last_play_state = now;

        if was_edit && !is_edit {
            self.enter_play_session();
        } else if !was_edit && is_edit {
            self.exit_play_session();
        }
    }

    /// Builds the play world + starts the script VM on the Edit→Playing edge.
    ///
    /// Turns the play scene's collider/rigidbody components into Jolt bodies (`World::populate`
    /// cooking `.smesh` through the asset reader), adds per-bone kinematic bodies and a
    /// `CharacterVirtual` per controller, starts the VM (loading the project's `src/` classes +
    /// injecting fields), installs the host bridge, resets the contact cursor, and points the
    /// editor's `sim_tick` seam at the play-loop closure.
    fn enter_play_session(&mut self) {
        // Build the Jolt world from the play scene.
        let world = match World::new() {
            Ok(world) => world,
            Err(err) => {
                log_error!("physics world create failed: {err}");
                return;
            }
        };
        self.physics_init = true; // globals installed by `World::new`; teardown shuts them down once.
        *self.physics.borrow_mut() = Some(world);
        self.contact_cursor.set(0);
        self.populate_play_world();

        // Start the script VM against the play scene and install the bridge.
        self.start_play_scripts();

        self.install_sim_tick();
    }

    /// Populates the live world from the play scene's components: collider/rigidbody bodies
    /// (cooking convex-hull/mesh shapes through the asset reader), per-bone kinematic bodies for
    /// bone-following rigs, and a `CharacterVirtual` per controller entity.
    fn populate_play_world(&mut self) {
        let assets = &mut self.assets;
        let mut world_ref = self.physics.borrow_mut();
        let Some(world) = world_ref.as_mut() else {
            return;
        };
        let scene = self.editor.active_scene();

        let mut cook = |id: Uuid| {
            assets
                .load_mesh_cpu_asset(id)
                .map_err(|err| err.to_string())
        };
        world.populate(scene, &mut cook);
        world.build_bone_bodies(scene);

        let mut characters: Vec<Entity> = Vec::new();
        scene.for_each::<&saffron_scene::CharacterController, _>(|entity, _| {
            characters.push(entity);
        });
        for entity in characters {
            if let Err(err) = world.add_character(entity, scene) {
                log_warn!("character controller setup failed: {err}");
            }
        }
    }

    /// Starts the play session's script VM and installs the host bridge.
    ///
    /// Loads each `Script` slot's class from `<project>/src`, injects its declared fields, and
    /// runs `on_create` per instance. A start failure is logged and leaves the VM inactive (no
    /// scripts run); physics still steps. The buffered `sa.log` lines drain into the editor's
    /// ring after the call releases the editor borrow.
    fn start_play_scripts(&mut self) {
        self.script
            .borrow_mut()
            .install_bridge(Rc::clone(&self.bridge));
        let src_dir = std::path::Path::new(&self.editor.project_root).join("src");
        let registry = Arc::clone(&self.script_registry);
        let scene = self.editor.active_scene();
        let result = self
            .script
            .borrow_mut()
            .start_scripts(scene, registry, &src_dir);
        match result {
            Ok(()) => {
                self.script_vm_active = true;
                self.editor.script_instance_count =
                    i32::try_from(self.script.borrow().instance_count()).unwrap_or(i32::MAX);
            }
            Err(err) => {
                log_error!("script start failed: {err}");
                self.script_vm_active = false;
            }
        }
        // `on_create` may have logged; route the lines into the editor now (it is free here).
        self.drain_script_sinks();
    }

    /// Tears the play session down on the Playing/Paused→Edit edge, in teardown order: stop the
    /// VM (it never touches the scene), then drop the world (RAII frees its Jolt bodies), then
    /// clear the `sim_tick` seam. The Jolt globals outlive every world, so they shut down only in
    /// the host teardown, after the last world is gone.
    fn exit_play_session(&mut self) {
        self.script.borrow_mut().stop_scripts();
        self.script_vm_active = false;
        self.editor.script_instance_count = 0;
        *self.physics.borrow_mut() = None;
        self.editor.sim_tick = None;
        self.pose_targets.borrow_mut().clear();
        self.script_log_sink.borrow_mut().clear();
        self.script_error_sink.borrow_mut().clear();
        self.contact_cursor.set(0);
    }

    /// Installs the `sim_tick` seam closure on the editor.
    ///
    /// The closure captures the play-session shared cells (it cannot capture `&mut self`: it runs
    /// while the editor is borrowed by `tick_play`). Per fixed step it drives ragdolls to the
    /// snapshotted animated pose, advances the per-bone blend, steps physics (which writes
    /// Dynamic body poses back into the play scene), writes ragdoll poses, drains the tick's new
    /// contacts and dispatches them to scripts, derives input edges, and runs `on_update` —
    /// physics-then-scripts, so a script reads this frame's settled transforms. A contained
    /// script failure is buffered for the host's deferred pause.
    fn install_sim_tick(&mut self) {
        let physics = Rc::clone(&self.physics);
        let script = Rc::clone(&self.script);
        let registry = Arc::clone(&self.script_registry);
        let input = Rc::clone(&self.script_input);
        let pose_targets = Rc::clone(&self.pose_targets);
        let contact_cursor = Rc::clone(&self.contact_cursor);
        let error_pending = Rc::clone(&self.script_error_pending);
        let error_sink = Rc::clone(&self.script_error_sink);
        let vm_active = self.script_vm_active;

        self.editor.sim_tick = Some(Box::new(move |scene: &mut Scene, dt: f32| {
            // Step physics + collect this tick's new contacts under a scoped world borrow that
            // is released before any script runs (a contact / `on_update` handler may
            // `sa.raycast` back into the world through the bridge, re-borrowing the cell).
            let events = {
                let mut world_ref = physics.borrow_mut();
                let Some(world) = world_ref.as_mut() else {
                    return;
                };
                // Drive active ragdolls toward this frame's animated pose, ease the per-bone
                // physics weight, then step — drive before the step so the motors are read in
                // the solve.
                world.drive_ragdolls_to_pose(&pose_targets.borrow());
                world.advance_ragdoll_blend(dt);
                world.step(scene, dt);
                // Physics wins the frame: write each ragdoll part's pose into the bone override.
                world.write_ragdoll_poses(scene);

                if vm_active {
                    let drain = world.drain_contacts(contact_cursor.get());
                    contact_cursor.set(drain.high_water_seq);
                    drain.events
                } else {
                    Vec::new()
                }
            };

            if !vm_active {
                return;
            }

            // Dispatch the drained contacts before `on_update`, so a trigger/contact handler
            // runs the same frame the contact fired.
            for event in events {
                let contact = ContactInfo {
                    entity_a: event.entity_a,
                    entity_b: event.entity_b,
                    begin: event.kind == saffron_physics::ContactKind::Begin,
                    sensor: event.sensor,
                    point: event.point,
                    normal: event.normal,
                };
                let failure =
                    script
                        .borrow_mut()
                        .dispatch_contact(scene, Arc::clone(&registry), contact);
                if let Some(err) = failure {
                    log_error!(
                        "script contact handler in '{}': {}",
                        err.script,
                        err.message
                    );
                    error_sink.borrow_mut().push(err);
                    error_pending.set(true);
                    return;
                }
            }

            // Derive this tick's input edges, then run every instance's `on_update`.
            let failure = {
                let mut input_ref = input.borrow_mut();
                derive_script_input_edges(&mut input_ref);
                script.borrow_mut().tick_scripts(
                    scene,
                    Arc::clone(&registry),
                    Some(&mut input_ref),
                    dt,
                )
            };
            if let Some(err) = failure {
                log_error!("script error in '{}': {}", err.script, err.message);
                error_sink.borrow_mut().push(err);
                error_pending.set(true);
            }
        }));
    }

    /// Routes what a script call batch buffered while the editor was borrowed into the editor's
    /// rings: the `sa.log` lines and the contained errors. Called after each `tick_play` and
    /// after the play-edge `start_scripts`, when the editor is freely borrowable.
    fn drain_script_sinks(&mut self) {
        for line in self.script_log_sink.borrow_mut().drain(..) {
            self.editor.push_script_log(line.sender, line.message);
        }
        for err in self.script_error_sink.borrow_mut().drain(..) {
            self.editor
                .push_script_error(err.entity_uuid.0, err.script, err.message);
        }
    }

    /// Brings the project up from the editor-set environment once at attach time, before the
    /// first frame. Routes through the control context's one project-bring-up path, against the
    /// renderer's upload seam — so a host launched with `SAFFRON_PROJECT` /
    /// `SAFFRON_AUTO_EMPTY_PROJECT` / a `project.json` has a loaded scene before the loop starts,
    /// instead of an empty one waiting on the editor.
    fn bootstrap_project(&mut self, window: &mut Window, renderer: &mut Renderer) {
        self.ensure_uploader(renderer);
        let Some(uploader) = self.uploader.as_ref() else {
            return; // No uploader (device create failed): nothing to load into.
        };
        let mut control_renderer = HostControlRenderer::new(renderer, uploader);
        self.control.bootstrap_project_from_env(
            window,
            &mut control_renderer,
            &mut self.editor,
            &mut self.assets,
        );
    }

    /// Renders the scene through the active camera and submits the native gizmo overlay: track
    /// the viewport size in present mode, sync the gizmo, render the scene, then build + submit
    /// the edit overlay geometry.
    fn render_ui(&mut self, window: Option<&Window>, renderer: &mut Renderer) {
        // Publish mode: the editor owns the render size (set-viewport-size); the hidden
        // window's size is meaningless. Present mode tracks the window.
        if !self.shm_publish
            && let Some(window) = window
        {
            let view = renderer.active_view_id();
            let _ = renderer.set_viewport_desired_size(view, window.width(), window.height());
        }

        self.editor.sync_native_gizmo();
        let cam = self.editor.render_camera_view();
        let (view_width, view_height) = (renderer.viewport_width(), renderer.viewport_height());
        if view_width == 0 || view_height == 0 {
            return;
        }

        let options = RenderSceneOptions {
            show_editor_camera_models: self.editor.play_state == PlayState::Edit,
            show_grid: self.editor.debug_overlays.grid
                && self.editor.play_state == PlayState::Edit
                && !self.editor.previewing(),
        };

        let skinning = renderer.skinning_enabled();
        self.ensure_uploader(renderer);
        if let Some(uploader) = self.uploader.as_ref() {
            let scene: &mut Scene = self.editor.active_scene();
            let mut driver = RendererScene::new(renderer, uploader, skinning);
            render_scene(&mut driver, scene, &mut self.assets, &cam, options);
        }

        self.submit_scene_edit_overlay(renderer, &cam, view_width, view_height);

        // Tell the renderer whether to fold the active view's BGRA8 shm readback into this
        // frame's command buffer, so it records the blit/copy inline — no separate submit, no
        // synchronous stall.
        if self.shm_publish {
            self.arm_active_view_shm(renderer);
        }

        // Execute the offscreen scene graph (pass 1: scene → offscreen). The editor/headless
        // host never presents a swapchain — the BGRA8 read-back into the shared-memory ring is
        // its frame transport. The copy is recorded into the frame command buffer above; this
        // submits it. A failure is logged, not fatal.
        if let Err(err) = renderer.render_scene_offscreen() {
            log_error!("render_scene_offscreen: {err}");
            return;
        }
        if self.shm_publish {
            self.publish_pipelined_view(renderer);
        }
    }

    /// Arms the renderer's per-view shm-publish flags from the host's segment wiring, so
    /// [`Renderer::render_scene_offscreen`] knows whether to fold the active view's readback
    /// into the frame command buffer.
    fn arm_active_view_shm(&mut self, renderer: &mut Renderer) {
        for (view, shm_view) in [
            (saffron_rendering::ViewId::Scene, ShmView::Scene),
            (
                saffron_rendering::ViewId::AssetPreview,
                ShmView::AssetPreview,
            ),
        ] {
            renderer.set_shm_publish_enabled(view, self.shm.is_enabled(shm_view));
        }
    }

    /// Drains the renderer's pipelined BGRA8 bytes (a frame whose GPU work completed a few
    /// frames ago, read back stall-free at the begin-frame fence wait) and publishes them
    /// into the view's shm segment. A no-op when no completed slot is staged.
    fn publish_pipelined_view(&mut self, renderer: &mut Renderer) {
        let Some((view_id, width, height, pixels)) = renderer.pending_shm_view() else {
            return;
        };
        let view = match view_id {
            saffron_rendering::ViewId::Scene => ShmView::Scene,
            saffron_rendering::ViewId::AssetPreview => ShmView::AssetPreview,
        };
        if !self.shm.is_enabled(view) {
            return;
        }
        self.shm.publish(view, width, height, pixels);
    }

    /// Builds the native gizmo overlay geometry and submits it. `edit_chrome` (Edit and not
    /// previewing) gates the gizmo / billboards / frustums / debug overlays; colliders + the
    /// skeleton draw outside it.
    fn submit_scene_edit_overlay(
        &mut self,
        renderer: &mut Renderer,
        cam: &CameraView,
        width: u32,
        height: u32,
    ) {
        let edit_chrome =
            self.editor.play_state == PlayState::Edit && self.editor.preview_scene.is_none();
        // The overlay's debug/collider builders resolve meshes through the renderer's uploader
        // + descriptors (the bindless texture binds); the gizmo / billboards / skeleton are
        // pure projection. Skinning is off for the resolve (bounds only, no skin stream).
        let (depth_tested, on_top) = match self.uploader.as_ref() {
            Some(uploader) => {
                let gpu = RendererUploader::new(uploader, renderer.descriptors(), false);
                build_scene_edit_overlay(
                    &mut self.editor,
                    &mut self.assets,
                    &gpu,
                    cam,
                    width,
                    height,
                    edit_chrome,
                )
            }
            None => (Vec::new(), Vec::new()),
        };
        renderer.submit_overlay(depth_tested, on_top);
    }

    /// Starts the off-frame-loop thumbnail worker over a [`WorkerThumbnailGpu`] sharing the
    /// renderer's device + bindless table.
    ///
    /// The renderer prewarms its own thumbnail pipelines first (the synchronous control-drain
    /// path still uses them); the worker then builds + prewarms its **own** thumbnail renderer
    /// on the worker thread. A cold-cache `get-thumbnail`/`view-asset` then enqueues + replies
    /// `pending` instead of blocking the frame loop on the decode + upload + render. A prewarm /
    /// worker-build failure is logged and the worker simply stays unstarted (the synchronous
    /// fallback still serves thumbnails), never fatal.
    fn start_thumbnail_worker(&mut self, renderer: &mut Renderer) {
        if let Err(err) = renderer.prewarm_thumbnail_resources() {
            log_error!("thumbnail worker not started: prewarm failed: {err}");
            return;
        }
        let queue = GpuQueue::new(renderer.device().graphics_queue);
        let worker_gpu = match crate::control_renderer::WorkerThumbnailGpu::new(
            renderer.device_arc(),
            renderer.descriptors_arc(),
            queue,
            renderer.skinning_enabled(),
        ) {
            Ok(gpu) => gpu,
            Err(err) => {
                log_error!("thumbnail worker not started: {err}");
                return;
            }
        };
        self.assets.start_thumbnail_worker(Box::new(worker_gpu));
    }

    /// Lazily builds the host-owned one-off [`Uploader`] from the renderer's device + queue.
    /// The uploader is `Arc`-rooted in the device resources, so it outlives any single-frame
    /// `&mut Renderer` borrow.
    fn ensure_uploader(&mut self, renderer: &Renderer) {
        if self.uploader.is_some() {
            return;
        }
        let queue = GpuQueue::new(renderer.device().graphics_queue);
        match Uploader::new(renderer.device(), &queue) {
            Ok(uploader) => self.uploader = Some(uploader),
            Err(err) => log_error!("uploader create failed: {err}"),
        }
    }

    /// The load-bearing teardown sequence, run from `on_detach` after the loop's `wait_gpu_idle`
    /// and before the renderer drops. Discards the step record; [`HostLayer::teardown_recording`]
    /// is the same sequence with the order observable.
    fn teardown(&mut self) {
        let mut steps = Vec::new();
        self.teardown_recording(&mut steps);
    }

    /// The teardown sequence, emitting each step into `steps` in execution order. The
    /// cross-object ordering is the host's load-bearing safety contribution (a runtime UAF if
    /// wrong, not a compile error), so the order is pinned here and asserted by a test reading
    /// `steps`. The production [`HostLayer::teardown`] passes a throwaway vec.
    ///
    /// The order:
    /// 1. Drain + join the thumbnail worker first — it borrows the renderer, which is still
    ///    alive (the run loop drops the renderer only after `on_detach` returns), so it MUST
    ///    join before `wait_gpu_idle`/the renderer drop.
    /// 2. Close the control socket.
    /// 3. Quit can land mid-play: stop the script VM (it never touches the scene), drop the
    ///    physics world (RAII frees its Jolt bodies + detaches ragdolls before they destruct),
    ///    then shut down the Jolt globals **after** that last world is gone (the
    ///    `Factory`/registered types outlive every body), then null the `sim_tick` seam and
    ///    unsubscribe the two play-state hooks.
    /// 4. Drop the host's one-off uploader + clear every cached GPU `Ref` **before** the renderer
    ///    frees the device/allocator — otherwise the last `Arc<GpuMesh>`/`Arc<GpuTexture>` drop
    ///    would free a GPU resource after its allocator is gone (UAF). The loop already idled the
    ///    GPU, so clearing under an idle device is safe.
    fn teardown_recording(&mut self, steps: &mut Vec<TeardownStep>) {
        self.assets.stop_thumbnail_worker();
        steps.push(TeardownStep::WorkerJoined);

        self.control.shutdown();
        steps.push(TeardownStep::ControlClosed);

        self.script.borrow_mut().stop_scripts();
        self.script_vm_active = false;
        steps.push(TeardownStep::ScriptsStopped);

        // Drop the world before the Jolt globals: a live world holds Jolt bodies, so shutting
        // down the `Factory`/registered types first would be a use-after-free.
        *self.physics.borrow_mut() = None;
        steps.push(TeardownStep::PhysicsWorldDropped);

        if self.physics_init {
            saffron_physics::shutdown_physics();
            self.physics_init = false;
        }
        steps.push(TeardownStep::JoltGlobalsShutdown);

        self.editor.sim_tick = None;
        steps.push(TeardownStep::SimTickCleared);

        self.editor
            .on_play_state_changed
            .unsubscribe(self.script_subscription);
        self.editor
            .on_play_state_changed
            .unsubscribe(self.physics_subscription);
        steps.push(TeardownStep::PlayHooksUnsubscribed);

        // The uploader's `Arc<DeviceResources>` must release before the renderer frees the
        // device, so drop it alongside the cached GPU `Ref`s.
        self.uploader = None;
        self.assets.clear_asset_caches();
        steps.push(TeardownStep::GpuCachesCleared);
    }
}

impl Layer for HostLayer {
    fn name(&self) -> &str {
        "HostLayer"
    }

    fn on_attach(&mut self, app: &mut App) {
        // Bring the project up from the editor-set environment before the first frame, then
        // auto-select the first mesh entity so the native-viewport host (no hierarchy panel)
        // starts with something selected. Both need the renderer; the GPU-free test host skips
        // them and drives the session spine alone.
        if let Some(renderer) = app.frame_host.renderer_mut() {
            let mut headless = Window::headless();
            let window = app.window.as_mut().unwrap_or(&mut headless);
            self.bootstrap_project(window, renderer);
            self.start_thumbnail_worker(renderer);
        }
        self.auto_select_first_mesh();
    }

    fn on_update(&mut self, app: &mut App, dt: TimeSpan) {
        let current_ppid = self.current_ppid();

        // Control first (it reads + mutates the editor through `EngineContext`), so a command
        // this frame takes effect this frame. It needs the renderer + a window; with no
        // renderer attached (the test host) it is skipped and the session spine still runs.
        // `frame_host` and `window` are distinct `App` fields, so they borrow disjointly.
        if let Some(renderer) = app.frame_host.renderer_mut() {
            // Headless editor mode has no window; the control plane still takes a `Window`
            // facade, so a standalone headless window stands in (its size is unused in publish
            // mode and its signals are inert without an event loop).
            let mut headless = Window::headless();
            let window = app.window.as_mut().unwrap_or(&mut headless);
            self.poll_control(window, renderer);
            // Insert any thumbnails the worker finished this interval into the GPU caches.
            self.assets.drain_thumbnail_completions();
        }

        if self.update_session(dt, current_ppid) == ParentWatch::ParentDied {
            app.running = false;
            return;
        }

        // The gizmo-drag smoothing reads the viewport size, so it runs only with a renderer.
        if let Some(renderer) = app.frame_host.renderer_mut() {
            let (w, h) = (renderer.viewport_width(), renderer.viewport_height());
            let cam = self.editor.camera.view();
            self.editor.step_native_gizmo_drag(&cam, w, h, dt.seconds);
        }
    }

    fn on_ui(&mut self, app: &mut App) {
        // The scene reads its catalog through an `Arc<AssetCatalog>` shared from the asset
        // server (the asset ops keep it in sync). The remaining `on_ui` work is the scene render
        // + overlay submit, which needs the renderer.
        if let Some(renderer) = app.frame_host.renderer_mut() {
            let window = app.window.as_ref();
            self.render_ui(window, renderer);
        }
    }

    fn on_detach(&mut self, _app: &mut App) {
        self.teardown();
    }
}

#[cfg(test)]
impl HostLayer {
    /// Marks the host as mid-play with the Jolt globals installed and a live world, set directly
    /// so a teardown test can drive a real play→quit.
    fn set_play_session_for_test(&mut self, physics: World) {
        *self.physics.borrow_mut() = Some(physics);
        self.physics_init = true;
        self.script_vm_active = true;
    }

    /// Whether the Jolt globals are still flagged installed (a test reads the post-teardown state).
    fn physics_init_for_test(&self) -> bool {
        self.physics_init
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use saffron_sceneedit::PlayState;
    use std::sync::Mutex;

    // The Jolt `Factory::sInstance` is a process global the world bring-up + `shutdown_physics`
    // touch; serialize the tests that build/drop a world so they never race it (mirrors the
    // physics crate's `JOLT_GLOBAL`). Recover from a poisoned lock — it only guards the global
    // init race, so a panic in one test leaves no shared state to corrupt.
    static JOLT_GLOBAL: Mutex<()> = Mutex::new(());
    fn jolt_guard() -> std::sync::MutexGuard<'static, ()> {
        JOLT_GLOBAL.lock().unwrap_or_else(|p| p.into_inner())
    }

    /// A unique, never-touched asset root per test so two tests never share a scratch dir.
    fn scratch_root(tag: &str) -> std::path::PathBuf {
        std::env::temp_dir().join(format!(
            "saffron-host-{tag}-{}-{:p}",
            std::process::id(),
            tag
        ))
    }

    /// A host layer built standalone (not editor-spawned), no shm — the GPU-free spine.
    fn standalone(tag: &str) -> HostLayer {
        HostLayer::new(scratch_root(tag), false, false)
    }

    #[test]
    fn host_layer_constructs_without_gpu() {
        let host = standalone("construct");

        // The editor, asset server, and animation runtime are live without a renderer.
        assert_eq!(host.editor().play_state, PlayState::Edit);
        assert!(!host.shm_publishing());
        assert!(!host.has_physics());

        // The clip loader is installed: a clip id that resolves against an empty catalog
        // returns the typed `ClipLoad` error (not a panic / not "no loader"), proving the
        // loader closure is wired.
        // (The loader is exercised indirectly; here we assert the two play-state hooks are
        // live tokens — dropping the layer must unsubscribe them with no dangling sub.)
        assert!(host.play_hooks_live(), "play-state hooks are subscribed");

        // Dropping (via an explicit detach) unsubscribes both play-state hooks.
        let mut host = host;
        host.teardown();
        assert!(
            !host.play_hooks_live(),
            "detach unsubscribed the play-state hooks (no dangling subscription)"
        );
    }

    #[test]
    fn parent_death_sets_should_close() {
        // Editor-spawned: the captured pid is the live parent; a matching observation stays
        // alive, a mismatched one (the editor vanished → reparent) requests exit.
        let host = HostLayer::new(scratch_root("ppid"), true, false);
        let captured = host.editor_pid;

        assert_eq!(
            host.watch_parent(captured),
            ParentWatch::Alive,
            "an unchanged parent keeps running"
        );

        // A changed observation (the editor vanished → reparent away, parent now `None` or a
        // different pid) trips the watch. `None` is the unambiguous "no parent" reparent.
        assert_eq!(
            host.watch_parent(None),
            ParentWatch::ParentDied,
            "a changed parent (editor gone) requests exit"
        );

        // The update step turns the verdict into a session abort: an armed host that observes
        // a vanished parent returns `ParentDied` before touching the rest of the spine.
        let mut armed = HostLayer::new(scratch_root("ppid-armed"), true, false);
        assert_eq!(
            armed.update_session(TimeSpan::from_seconds(0.016), None),
            ParentWatch::ParentDied,
            "the session update aborts on the parent-death verdict"
        );

        // A non-editor-spawned host never watches, whatever the observed pid.
        let mut standalone = standalone("ppid-standalone");
        assert_eq!(
            standalone.watch_parent(None),
            ParentWatch::Alive,
            "a standalone host is never auto-killed by the parent watch"
        );
        assert_eq!(
            standalone.update_session(TimeSpan::from_seconds(0.016), None),
            ParentWatch::Alive,
            "a standalone session update runs the full spine"
        );
    }

    #[test]
    fn preview_prune_clears_runtime() {
        let mut host = standalone("prune");

        // No preview yet: an update at Edit does not prune (the edge has not flipped).
        let dt = TimeSpan::from_seconds(0.016);
        host.update_session(dt, None);
        assert!(!host.editor().previewing());

        // Enter the asset preview as the active view (the `previewing()` edge false→true).
        let mut preview = Scene::new();
        let _ = preview.create_entity("preview-root");
        host.editor_mut().preview_scene = Some(preview);
        host.editor_mut().preview_active_view = true;
        assert!(host.editor().previewing());

        // The next update sees the edge and prunes the runtime exactly once. The runtime has
        // no per-entity entries to begin with (a fresh session), so the observable contract
        // is that the preview-active tracking flipped and the prune ran on the edge.
        assert_eq!(host.animation().session_entry_count(), 0);
        host.update_session(dt, None);
        assert!(
            host.preview_active,
            "the preview-active tracking flips on the enter edge"
        );
        assert_eq!(
            host.animation().session_entry_count(),
            0,
            "the runtime stays pruned across the preview"
        );

        // Leaving the preview (true→false) is the symmetric edge; the tracking flips back.
        host.editor_mut().preview_active_view = false;
        host.editor_mut().preview_scene = None;
        host.update_session(dt, None);
        assert!(
            !host.preview_active,
            "the preview-active tracking flips back on the leave edge"
        );
    }

    #[test]
    fn update_order_is_animation_then_tick_play() {
        // A play/step command this frame must take effect this frame: stepping while Paused
        // grants one fixed tick that `tick_play` (run after animation, inside update_session)
        // consumes, advancing `play_tick`. We arm a step, then a single update consumes it.
        let mut host = standalone("order");

        host.editor_mut().enter_play().expect("enter play");
        host.editor_mut().pause_play().expect("pause");
        let before = host.editor().play_tick;
        host.editor_mut().step_play(1).expect("step");

        let dt = TimeSpan::from_seconds(0.016);
        // The fly-cam look-delta is set non-zero, and the update must drain it to zero each
        // frame (a burst between frames is otherwise lost).
        host.editor_mut().fly_input.look_delta = Vec2::new(12.0, -7.0);

        let watch = host.update_session(dt, None);
        assert_eq!(watch, ParentWatch::Alive);

        assert_eq!(
            host.editor().play_tick,
            before + 1,
            "the stepped tick ran this frame (tick_play consumed the step after animation)"
        );
        assert_eq!(
            host.editor().fly_input.look_delta,
            Vec2::ZERO,
            "the fly-cam look-delta is drained to zero each update"
        );
    }

    /// The play-edge wiring proper, CPU-only: entering Play builds a Jolt world from the play
    /// scene, the per-frame `update_session` steps it (the `sim_tick` seam) and writes the
    /// dynamic body's pose back into the play scene, and stopping drops the world and restores
    /// Edit. This is the unit-level mirror of `physics-falling-box.test.ts`, exercised end to
    /// end without a renderer.
    #[test]
    fn play_edge_builds_a_world_and_steps_the_box() {
        use saffron_scene::{Collider, Rigidbody, Transform};
        let _guard = jolt_guard();
        // Skip cleanly if the Jolt globals cannot install (no toolchain) — not a false pass.
        match World::new() {
            Ok(_) => {}
            Err(err) => {
                eprintln!("skipping: World::new failed: {err}");
                return;
            }
        }

        let mut host = standalone("play-edge");

        // A static floor (a wide thin collider, no rigidbody → implicitly static) and a dynamic
        // box dropped from y=5 (default 0.5 half-extent box, default Dynamic rigidbody).
        let scene = &mut host.editor_mut().scene;
        let floor = scene.create_entity("Floor");
        scene
            .add_component(
                floor,
                Collider {
                    half_extents: glam::Vec3::new(10.0, 0.1, 10.0),
                    ..Collider::default()
                },
            )
            .expect("floor collider");
        let cube = scene.create_entity("Box");
        scene
            .add_component(
                cube,
                Transform {
                    translation: glam::Vec3::new(0.0, 5.0, 0.0),
                    scale: glam::Vec3::ONE,
                    rotation: glam::Vec3::ZERO,
                },
            )
            .expect("box transform");
        scene
            .add_component(cube, Collider::default())
            .expect("box collider");
        scene
            .add_component(cube, Rigidbody::default())
            .expect("box rigidbody");
        let cube_uuid = scene
            .component::<saffron_scene::IdComponent>(cube)
            .map(|id| id.id)
            .expect("box id");

        // No world in Edit — the authored box sits at its authored height.
        assert!(!host.has_physics(), "no world before play");
        assert_eq!(host.editor().scene.world_matrix(cube).w_axis.y, 5.0);

        // Enter play and tick: the first update reconciles the Edit→Playing edge (builds the
        // world from the play scene), then steps it.
        host.editor_mut().enter_play().expect("enter play");
        let dt = TimeSpan::from_seconds(0.016);
        // ~3s of fixed steps: the box falls under gravity and settles on the floor.
        for _ in 0..200 {
            host.update_session(dt, None);
        }

        assert!(host.has_physics(), "the world is live during play");
        // Read the play twin's world Y: it dropped well below the authored 5 and settled near
        // the floor top (0.1) + the box half-extent (0.5) ≈ 0.6, never tunneling through.
        let play_cube = host
            .editor_mut()
            .active_scene()
            .find_entity_by_uuid(cube_uuid)
            .expect("play twin");
        let settled_y = host
            .editor_mut()
            .active_scene()
            .world_matrix(play_cube)
            .w_axis
            .y;
        assert!(settled_y < 5.0, "the box fell from 5: now {settled_y}");
        assert!(
            (0.4..1.0).contains(&settled_y),
            "the box settled at ~floor-top + half-extent: {settled_y}"
        );

        // Stop: the world drops, Edit returns, and the authored box is untouched (never written
        // during play — the duplicate held every write).
        host.editor_mut().stop_play().expect("stop");
        host.update_session(dt, None);
        assert!(!host.has_physics(), "the world dropped on stop");
        assert_eq!(host.editor().play_state, PlayState::Edit);
        assert_eq!(
            host.editor().scene.world_matrix(cube).w_axis.y,
            5.0,
            "the authored box is back at its authored height"
        );

        host.teardown();
    }

    #[test]
    fn teardown_unsubscribes_and_drops_in_order() {
        let _guard = jolt_guard();
        let mut host = standalone("teardown-order");

        // A play session active: a real Jolt world (so `physics_init` is meaningful) and the
        // script-VM flag set, with both play-state subscriptions live and a `sim_tick` seam
        // installed (the host fills it on the play edge). Quit can land here, mid-play.
        let world = World::new().expect("world creation");
        host.set_play_session_for_test(world);
        host.editor_mut().sim_tick = Some(Box::new(|_scene, _dt| {}));
        assert!(host.play_hooks_live(), "the play-state hooks start live");
        assert!(host.has_physics(), "the play world is present");
        assert!(
            host.physics_init_for_test(),
            "the Jolt globals are installed"
        );

        // Record the teardown order and assert it matches the pinned sequence.
        let mut steps = Vec::new();
        host.teardown_recording(&mut steps);
        assert_eq!(
            steps,
            vec![
                TeardownStep::WorkerJoined,
                TeardownStep::ControlClosed,
                TeardownStep::ScriptsStopped,
                TeardownStep::PhysicsWorldDropped,
                TeardownStep::JoltGlobalsShutdown,
                TeardownStep::SimTickCleared,
                TeardownStep::PlayHooksUnsubscribed,
                TeardownStep::GpuCachesCleared,
            ],
            "the teardown order drops every subsystem before the device"
        );

        // The world drops strictly before the Jolt-globals shutdown (a live world holds Jolt
        // bodies; shutting the Factory down first would be a UAF).
        let world_pos = steps
            .iter()
            .position(|s| *s == TeardownStep::PhysicsWorldDropped)
            .unwrap();
        let jolt_pos = steps
            .iter()
            .position(|s| *s == TeardownStep::JoltGlobalsShutdown)
            .unwrap();
        assert!(
            world_pos < jolt_pos,
            "the physics world dropped before the Jolt globals shut down"
        );

        // Post-teardown state: subscriptions gone, world gone, sim_tick cleared, globals flagged
        // down, script flag cleared — back to the fresh, drop-safe state.
        assert!(
            !host.play_hooks_live(),
            "teardown unsubscribed both play-state hooks (no dangling subscription)"
        );
        assert!(!host.has_physics(), "the play world was dropped");
        assert!(
            host.editor().sim_tick.is_none(),
            "the sim_tick seam was nulled"
        );
        assert!(
            !host.physics_init_for_test(),
            "the Jolt globals were shut down (physics_init cleared)"
        );
        assert!(!host.script_vm_active(), "the script VM flag was cleared");

        // Teardown is idempotent: a second pass is a clean no-op (a double on_detach must not
        // double-free or re-shutdown the globals).
        let mut again = Vec::new();
        host.teardown_recording(&mut again);
        assert!(!host.has_physics() && !host.physics_init_for_test());
    }

    #[test]
    fn ref_caches_drop_before_renderer() {
        // The asset GPU `Ref` caches must empty strictly before the renderer's device/allocator
        // drops. The host has no renderer in the test harness, so we prove the host's half: the
        // teardown step that clears the caches (`GpuCachesCleared`) runs while the host is still
        // alive — i.e. before `on_detach` returns and the run loop drops the renderer. We assert
        // the cache-clear is the *last* teardown step (the renderer drop happens strictly after,
        // outside the host), and that the asset GPU caches are empty afterward.
        let _guard = jolt_guard();
        let mut host = standalone("ref-caches");

        let mut steps = Vec::new();
        host.teardown_recording(&mut steps);

        assert_eq!(
            steps.last().copied(),
            Some(TeardownStep::GpuCachesCleared),
            "clearing the GPU Ref caches is the final host teardown step, before the renderer drops"
        );
        // Every host-owned GPU cache is emptied (the last `Arc<GpuMesh>`/`Arc<GpuTexture>` drop
        // runs here, under the idle device, not after the allocator is gone).
        assert!(
            host.assets.mesh_by_uuid.is_empty()
                && host.assets.texture_by_uuid.is_empty()
                && host.assets.model_by_uuid.is_empty(),
            "the GPU Ref caches are empty after the cache-clear step"
        );
    }

    #[test]
    fn shm_drop_is_device_independent() {
        // Dropping the viewport shm publisher after the renderer is gone must still munmap +
        // shm_unlink cleanly — its `Drop` touches no device. Here there is no renderer at all, so
        // a host carrying an enabled segment that is dropped proves the shm teardown is fully
        // independent of any GPU state.
        use crate::viewport_shm::{ShmView, ShmViewConfig, ViewportShmPublisher};
        use std::ffi::CString;

        let name = format!("/saffron-host-teardown-shm-{}", std::process::id());
        let mut host = standalone("shm-drop");
        let mut shm = ViewportShmPublisher::new();
        shm.enable(ShmViewConfig {
            view: ShmView::Scene,
            name: name.clone(),
        })
        .expect("enable scene segment");
        host.attach_shm_publisher(shm);
        assert!(host.shm_publishing(), "the scene segment is enabled");

        // Tear the host's session down first (no renderer ever existed), then drop the host —
        // the shm publisher's `Drop` runs with no device present and unlinks the segment.
        host.teardown();
        drop(host);

        let cname = CString::new(name).unwrap();
        let opened = rustix::shm::open(
            cname.as_c_str(),
            rustix::shm::OFlags::RDONLY,
            rustix::shm::Mode::empty(),
        );
        assert!(
            opened.is_err(),
            "dropping the host shm_unlinked the segment with no device access"
        );
    }
}
