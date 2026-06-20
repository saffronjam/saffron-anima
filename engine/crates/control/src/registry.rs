//! The command registry, the `EngineContext` live-state seam, dispatch, and the
//! two builtin commands (`ping`, `help`).
//!
//! The C++ `CommandRegistry` is a `vector<CommandTraits>` plus a `byName` map,
//! and `registerCommand<Params, Result>` wraps a typed handler in a closure that
//! parses params, runs, and serializes the result. The port keeps that shape: a
//! [`CommandRegistry`] is a `Vec<Command>` (insertion order preserved, so `help`
//! and the generated manifest iterate it in registration order) plus a
//! `HashMap<&'static str, usize>` index. The typed [`CommandRegistry::register`]
//! is the single place the wire encoding (serde-driven, decimal-string ids) is
//! applied, so every later handler inherits it.

use std::any::TypeId;
use std::collections::HashMap;
use std::sync::{Mutex, OnceLock};

use schemars::JsonSchema;
use serde::Serialize;
use serde::de::DeserializeOwned;
use serde_json::{Value, json};

use std::path::Path;

use saffron_assets::{AssetServer, GpuUploader, ThumbnailGpu};
use saffron_physics::World;
use saffron_protocol::{PingParams, PingResult};
use saffron_rendering::{
    ActiveAlarm, AlarmDrain, CaptureMode, CaptureState, FrameHistoryStats, FrameSample, PassTiming,
    PerfConfig, ProfileCapture, ProfilerMode, ReflectionProbe, RenderStatsFull, ViewId, ViewMode,
};
use saffron_sceneedit::SceneEditContext;
use saffron_window::Window;

use crate::error::{Error, Result};

/// The renderer seam every render-, scene-, and asset-domain command reaches
/// through.
///
/// The C++ `EngineContext` holds a bare `Renderer&`; in Rust the concrete
/// `Renderer` cannot be built headless (its swapchain WSI has no offscreen backing
/// on lavapipe), so the borrow is taken behind this object-safe trait. The live
/// implementation is the host's `HostControlRenderer`, which bundles the renderer
/// with the host-owned one-off `Uploader` (the renderer owns none) so the
/// GPU-upload / scene-render seam below can be handed to the asset loaders; a
/// unit-test stub implements it over plain in-memory state. Growing this trait is
/// the only renderer coupling the control plane has.
///
/// Beyond the render-domain query/toggle methods, three asset/scene-domain seams
/// live here: **view-select** ([`ControlRenderer::set_active_view`] + the
/// desired-size pair), **screenshot** ([`ControlRenderer::capture_viewport`]), and
/// the **GPU-upload** access point ([`ControlRenderer::with_gpu_uploader`]) that
/// hands a transient [`GpuUploader`] to the asset loaders (`import_texture`,
/// `load_mesh_asset`, `ensure_preview_floor_mesh`, `resolve_material_asset`,
/// `pick_entity`, …) the asset/scene handlers drive.
pub trait ControlRenderer {
    /// The full per-frame draw + timing + telemetry snapshot (the C++ `renderStats`).
    fn render_stats(&self) -> RenderStatsFull;

    /// Whether clustered-forward light culling is on.
    fn clustered_enabled(&self) -> bool;
    /// Toggles clustered-forward culling.
    fn set_clustered(&mut self, enabled: bool);
    /// Whether the depth pre-pass is on.
    fn depth_prepass_enabled(&self) -> bool;
    /// Toggles the depth pre-pass.
    fn set_depth_prepass(&mut self, enabled: bool);
    /// Whether the directional shadow map is on.
    fn shadows_enabled(&self) -> bool;
    /// Toggles the directional shadow map.
    fn set_shadows(&mut self, enabled: bool);
    /// Whether image-based ambient lighting is on.
    fn ibl_enabled(&self) -> bool;
    /// Toggles IBL ambient.
    fn set_ibl(&mut self, enabled: bool);
    /// Whether GTAO screen-space ambient occlusion is on.
    fn ssao_enabled(&self) -> bool;
    /// Toggles GTAO.
    fn set_ssao(&mut self, enabled: bool);
    /// Whether screen-space contact shadows are on.
    fn contact_shadows_enabled(&self) -> bool;
    /// Toggles contact shadows.
    fn set_contact_shadows(&mut self, enabled: bool);
    /// Whether screen-space one-bounce GI is on.
    fn ssgi_enabled(&self) -> bool;
    /// Toggles SSGI.
    fn set_ssgi(&mut self, enabled: bool);
    /// Whether DDGI multi-bounce GI is on.
    fn ddgi_enabled(&self) -> bool;
    /// Toggles DDGI.
    fn set_ddgi(&mut self, enabled: bool);
    /// Whether reflection probes contribute.
    fn reflection_probes_enabled(&self) -> bool;
    /// Toggles reflection probes.
    fn set_reflection_probes(&mut self, enabled: bool);
    /// The captured reflection probes in slot order (the `list-probes` source).
    fn reflection_probes(&self) -> Vec<ReflectionProbe>;
    /// Whether the GPU skinning path is on.
    fn skinning_enabled(&self) -> bool;
    /// Toggles GPU skinning.
    fn set_skinning(&mut self, enabled: bool);

    /// Whether the device supports hardware ray tracing.
    fn rt_supported(&self) -> bool;
    /// Whether ray-query shadows ran this frame.
    fn rt_shadows_enabled(&self) -> bool;
    /// Toggles ray-query shadows (the caller gates on [`ControlRenderer::rt_supported`]).
    fn set_rt_shadows(&mut self, enabled: bool);
    /// Whether ReSTIR direct lighting is on.
    fn restir_enabled(&self) -> bool;
    /// Toggles ReSTIR (the caller gates on [`ControlRenderer::rt_supported`]).
    fn set_restir(&mut self, enabled: bool);
    /// The built static-mesh BLAS count.
    fn rt_blas_count(&self) -> u32;

    /// The cached PSO count.
    fn pipeline_count(&self) -> u32;
    /// The high-water bindless texture-slot count.
    fn bindless_texture_count(&self) -> u32;
    /// The reclaimed-and-free bindless slot count.
    fn bindless_free_count(&self) -> u32;

    /// The current debug render-output mode.
    fn view_mode(&self) -> ViewMode;
    /// Selects the debug render-output mode.
    fn set_view_mode(&mut self, mode: ViewMode);

    /// The current AA mode name (`off` / `fxaa` / `taa` / `msaaN`).
    fn aa_mode(&self) -> String;
    /// Applies an AA selection by (samples, fxaa, taa); idles + recreates targets.
    ///
    /// # Errors
    ///
    /// Returns the device error message if the GPU cannot idle or the AA targets
    /// cannot be recreated.
    fn set_aa(&mut self, samples: u32, fxaa: bool, taa: bool) -> std::result::Result<(), String>;

    /// The tonemap exposure in stops.
    fn exposure_ev(&self) -> f32;
    /// Sets the tonemap exposure in stops.
    fn set_exposure(&mut self, ev: f32);

    /// The current GPU profiler mode.
    fn profiler_mode(&self) -> ProfilerMode;
    /// Selects the GPU profiler mode.
    fn set_profiler_mode(&mut self, mode: ProfilerMode);
    /// Whether timestamp queries are supported on the graphics queue.
    fn profiler_timestamps_supported(&self) -> bool;
    /// Whether pipeline-statistics queries are supported.
    fn profiler_pipeline_stats_supported(&self) -> bool;
    /// The last frame's per-pass GPU timings.
    fn pass_timings(&self) -> Vec<PassTiming>;
    /// The last frame's total GPU span (ms).
    fn pass_timings_total_ms(&self) -> f32;

    /// Arms a profiler capture, returning its id.
    fn start_profile_capture(
        &mut self,
        mode: CaptureMode,
        frames: u32,
        filter: String,
        include_cpu: bool,
        include_stats: bool,
    ) -> u32;
    /// Finishes the armed capture and returns the spans + metadata.
    fn stop_profile_capture(&mut self) -> ProfileCapture;
    /// The capture's mode.
    fn profile_capture_mode(&self) -> CaptureMode;
    /// The capture state machine's current state.
    fn profile_capture_state(&self) -> CaptureState;
    /// Frames copied into the in-flight capture so far.
    fn profile_capture_captured_frames(&self) -> u32;
    /// The in-flight capture's target frame count.
    fn profile_capture_target_frames(&self) -> u32;

    /// The rolling frame-time percentile / stutter summary.
    fn frame_history_stats(&self) -> FrameHistoryStats;
    /// The most recent `max_samples` frame samples, oldest→newest.
    fn frame_samples(&self, max_samples: u32) -> Vec<FrameSample>;
    /// The shared frame-budget / threshold config.
    fn perf_config(&self) -> PerfConfig;
    /// Replaces the perf config (clamped into sane ranges).
    fn set_perf_config(&mut self, config: PerfConfig);

    /// Drains perf-alarm events with `seq > since`.
    fn drain_alarms(&self, since: u64) -> AlarmDrain;
    /// The currently-firing perf alarms.
    fn active_alarms(&self) -> Vec<ActiveAlarm>;

    /// The active view's offscreen render width (device pixels).
    fn viewport_width(&self) -> u32;
    /// The active view's offscreen render height (device pixels).
    fn viewport_height(&self) -> u32;

    /// Whether the device is a software rasterizer.
    fn software_gpu(&self) -> bool;

    /// Blocks until the GPU has finished every in-flight frame (the C++ `waitGpuIdle`).
    ///
    /// The asset handlers idle before a destructive cache mutation (`scan-assets`,
    /// `reimport-model`, `delete-unused`) so dropping a cached `Arc<GpuMesh>`/
    /// `Arc<GpuTexture>` never frees a resource a frame still reads.
    fn wait_gpu_idle(&mut self);

    /// Switches the active view the engine renders + publishes (the C++ `setActiveView`).
    ///
    /// Routes the per-view render target + temporal state + shm publisher; the
    /// scene/camera swap that follows a preview enter/exit is the `SceneEditContext`'s
    /// concern, driven by the same handler.
    fn set_active_view(&mut self, view: ViewId);
    /// The render size a view last requested (device pixels), `(0, 0)` until it has been
    /// sized at least once. Read to tell whether a not-yet-shown preview pane needs
    /// seeding before a `set-active-view assetPreview` (the C++ `desiredWidth == 0`
    /// check).
    fn view_desired_size(&self, view: ViewId) -> (u32, u32);
    /// Sets a view's desired offscreen render size, recreating its targets (the C++
    /// `setViewportDesiredSize`).
    ///
    /// # Errors
    ///
    /// Returns the device error message if the GPU cannot idle or the targets cannot be
    /// recreated.
    fn set_view_desired_size(
        &mut self,
        view: ViewId,
        width: u32,
        height: u32,
    ) -> std::result::Result<(), String>;

    /// Captures the active view's offscreen scene color to a PNG file (the C++
    /// `captureViewport`, the `screenshot {target:viewport}` path). Synchronous: idles,
    /// reads back, and writes the file before returning.
    ///
    /// # Errors
    ///
    /// Returns the device / file error message if the capture cannot be performed.
    fn capture_viewport(&mut self, path: &Path) -> std::result::Result<(), String>;

    /// Requests a window-surface (swapchain) capture written at the end of the current
    /// frame (the C++ `requestWindowCapture`, the `screenshot {target:window}` path).
    /// Returns immediately — the `screenshot` reply carries `pending: true`.
    ///
    /// # Errors
    ///
    /// Returns the device / file error message if the capture cannot be armed.
    fn request_window_capture(&mut self, path: &Path) -> std::result::Result<(), String>;

    /// Runs `with` against a transient [`GpuUploader`] over the live renderer (the C++
    /// loaders' bare `Renderer&` upload seam).
    ///
    /// The upload seam borrows the renderer plus the host-owned one-off uploader for the
    /// call's duration; it never escapes the closure. Every asset handler that resolves
    /// or uploads an asset reaches the loaders through it — `import-texture`,
    /// `instantiate-model`, `material-import`, the preview floor, and `pick` (which pairs
    /// it with [`ControlRenderer::viewport_width`] / [`ControlRenderer::viewport_height`]
    /// for the ray-cast aspect).
    fn with_gpu_uploader(&mut self, with: &mut dyn FnMut(&dyn GpuUploader));

    /// Runs `with` against a transient [`ThumbnailGpu`] over the live renderer — the
    /// upload trio plus the render-to-PNG / material-preview primitives (the C++
    /// thumbnail/material-preview `Renderer&` seam).
    ///
    /// `get-thumbnail` / `view-asset` drive [`saffron_assets::request_thumbnail`] through
    /// it, and `preview-render` drives [`ThumbnailGpu::render_material_preview`] +
    /// [`ThumbnailGpu::encode_texture_thumbnail_png`]. The seam never escapes the closure.
    fn with_thumbnail_gpu(&mut self, with: &mut dyn FnMut(&dyn ThumbnailGpu));

    /// Serializes the renderer's settings as the project-file `renderSettings` block (the
    /// [`ProjectHost::render_settings_to_json`] seam the project lifecycle commands save).
    fn render_settings_to_json(&self) -> Value;

    /// Applies a saved `renderSettings` block to the renderer (the
    /// [`ProjectHost::apply_render_settings`] seam project load/open/reload drives).
    fn apply_render_settings(&mut self, settings: &Value);

    /// The LuaLS type-def text written to a project's `library/sa.lua` on create/load (the
    /// `sa_lua_defs` the host generates from the script bindings). Empty under the test
    /// stub.
    fn sa_lua_defs(&self) -> String;
}

/// The slice of live engine state a command may touch.
///
/// References only, assembled fresh each frame in `poll_control` and dropped at
/// the end of the drain — never stored past it (the C++ `EngineContext`). Because
/// the fields are distinct, a handler that needs `&mut` to two subsystems at once
/// borrows them disjointly through the struct, no `RefCell` required. `physics`
/// is the live play world or `None` in Edit / before the first play.
pub struct EngineContext<'a> {
    /// The OS / windowless window facade.
    pub window: &'a mut Window,
    /// The renderer, behind the [`ControlRenderer`] seam.
    pub renderer: &'a mut dyn ControlRenderer,
    /// The editor's mutable scene/selection/version state.
    pub scene_edit: &'a mut SceneEditContext,
    /// The live asset catalog + caches.
    pub assets: &'a mut AssetServer,
    /// The live play physics world, or `None` in Edit.
    pub physics: Option<&'a mut World>,
}

/// The boxed handler type: a closure run on the calling (main) thread that maps
/// `(ctx, params)` to a result `Value` or a typed error. `!Send` and
/// single-thread-confined, like the C++ `std::function`.
type HandlerFn = Box<dyn Fn(&mut EngineContext<'_>, &Value) -> Result<Value>>;

/// A registered control command: a name, one-line help, and its handler.
pub struct Command {
    /// The wire command name (`cmd` in the request envelope).
    pub name: &'static str,
    /// One-line help, surfaced by the `help` command and the editor palette.
    pub help: &'static str,
    run: HandlerFn,
}

impl Command {
    /// Runs the handler against the given context and params.
    ///
    /// # Errors
    ///
    /// Propagates whatever the handler returns — a [`Error::Command`] business
    /// failure or a [`Error::Params`] deserialize failure.
    pub fn run(&self, ctx: &mut EngineContext<'_>, params: &Value) -> Result<Value> {
        (self.run)(ctx, params)
    }
}

/// The fn-pointer command table: insertion-ordered rows plus a name index.
#[derive(Default)]
pub struct CommandRegistry {
    rows: Vec<Command>,
    by_name: HashMap<&'static str, usize>,
}

impl CommandRegistry {
    /// An empty registry.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Registers an untyped command whose handler receives the raw params
    /// `Value` (the C++ untyped `registerCommand` overload). `help` is the one
    /// builtin that needs this — it reflects over the registry.
    pub fn register_raw(
        &mut self,
        name: &'static str,
        help: &'static str,
        run: impl Fn(&mut EngineContext<'_>, &Value) -> Result<Value> + 'static,
    ) {
        let index = self.rows.len();
        self.by_name.insert(name, index);
        self.rows.push(Command {
            name,
            help,
            run: Box::new(run),
        });
    }

    /// Registers a typed command, mirroring the C++
    /// `registerCommand<Params, Result>` template: deserialize `P` from the
    /// params `Value`, run the typed handler, serialize `R` back to a `Value`.
    ///
    /// This is the single site the frozen wire encoding is applied — the typed
    /// DTOs carry the decimal-string-`u64` and kebab-case-enum derives, so every
    /// handler registered this way inherits the contract for free.
    pub fn register<P, R>(
        &mut self,
        name: &'static str,
        help: &'static str,
        handler: impl Fn(&mut EngineContext<'_>, P) -> Result<R> + 'static,
    ) where
        P: DeserializeOwned + JsonSchema + 'static,
        R: Serialize,
    {
        self.register_raw(name, help, move |ctx, params| {
            let folded = fold_positional_args::<P>(params);
            let parsed: P =
                serde_json::from_value(folded).map_err(|e| Error::Params(e.to_string()))?;
            let result = handler(ctx, parsed)?;
            serde_json::to_value(result).map_err(|e| Error::Params(e.to_string()))
        });
    }

    /// Looks up a command by name (the C++ `findCommand`).
    #[must_use]
    pub fn find(&self, name: &str) -> Option<&Command> {
        self.by_name.get(name).map(|&index| &self.rows[index])
    }

    /// The registered commands in insertion order (`help` iterates this).
    #[must_use]
    pub fn rows(&self) -> &[Command] {
        &self.rows
    }

    /// Dispatches one parsed request envelope to a reply envelope (the C++
    /// `dispatch`): echo `id`, find the command, run it, and build
    /// `{ id, ok, result | error }`.
    ///
    /// `id` echoes whatever the request carried (any JSON, absent → `null`);
    /// `ok` is always present; exactly one of `result` / `error` accompanies it.
    pub fn dispatch(&self, ctx: &mut EngineContext<'_>, request: &Value) -> Value {
        let id = request.get("id").cloned().unwrap_or(Value::Null);
        let command = request.get("cmd").and_then(Value::as_str).unwrap_or("");
        let Some(row) = self.find(command) else {
            return json!({
                "id": id,
                "ok": false,
                "error": format!("unknown command '{command}'"),
            });
        };
        // `help` reflects over the live registry, so it is served here rather
        // than from a captured snapshot that would go stale as later phases
        // register their commands. The `help` row still exists (so it lists
        // itself and resolves for the palette), it just has no standalone body.
        if command == "help" {
            return json!({ "id": id, "ok": true, "result": self.help_listing() });
        }
        let params = request.get("params").cloned().unwrap_or_else(|| json!({}));
        match row.run(ctx, &params) {
            Ok(result) => json!({ "id": id, "ok": true, "result": result }),
            Err(error) => json!({ "id": id, "ok": false, "error": error.to_string() }),
        }
    }

    /// The `{ commands: [{ name, help }] }` listing in registration order — the
    /// body of the `help` command (the C++ untyped `help` lambda).
    fn help_listing(&self) -> Value {
        let commands: Vec<Value> = self
            .rows
            .iter()
            .map(|command| json!({ "name": command.name, "help": command.help }))
            .collect();
        json!({ "commands": commands })
    }
}

/// `params[name]` if present, else the index-th element of `params["args"]`,
/// else `Null` (the C++ `positionalOr`).
///
/// This is the lenient read every handler shares so a command accepts either
/// `--name value` (an object key) or a bare positional. Domain handlers use it
/// to extract a selector before resolving it; the typed [`CommandRegistry::register`]
/// wrapper consumes the object form directly.
#[must_use]
pub fn positional_or(params: &Value, name: &str, index: usize) -> Value {
    if let Some(value) = params.get(name) {
        return value.clone();
    }
    params
        .get("args")
        .and_then(Value::as_array)
        .and_then(|args| args.get(index))
        .cloned()
        .unwrap_or(Value::Null)
}

/// The per-type cache of a DTO's positional field order, so a typed command resolves the order
/// once (the first dispatch) rather than rebuilding the `schemars` schema each call.
fn field_order_cache() -> &'static Mutex<HashMap<TypeId, &'static [String]>> {
    static CACHE: OnceLock<Mutex<HashMap<TypeId, &'static [String]>>> = OnceLock::new();
    CACHE.get_or_init(|| Mutex::new(HashMap::new()))
}

/// The declaration-ordered wire field names of DTO `P`, cached per type.
fn field_order<P: JsonSchema + 'static>() -> &'static [String] {
    let id = TypeId::of::<P>();
    let mut cache = field_order_cache()
        .lock()
        .expect("field-order cache poisoned");
    cache.entry(id).or_insert_with(|| {
        // Leaked once per DTO type (a bounded, process-lifetime set), so the borrow is `'static`.
        Box::leak(saffron_protocol::positional_field_order::<P>().into_boxed_slice())
    })
}

/// Folds a request's positional `args` array onto DTO `P`'s named fields before deserializing
/// (the C++ per-field `positional = true` contract: `args[i]` fills the `i`-th declared field).
///
/// A named key always wins over its positional slot. With no `args` array the params pass
/// through untouched, so the common object-form call costs nothing past the field-order lookup.
/// This is the single site the positional-CLI-argument wire shape (what `sa <cmd> <a> <b>` and
/// the e2e harness send as `{ args: [...] }`) is applied, so every typed command inherits it.
fn fold_positional_args<P: JsonSchema + 'static>(params: &Value) -> Value {
    let Some(args) = params.get("args").and_then(Value::as_array) else {
        return params.clone();
    };
    let mut object = match params.as_object() {
        Some(map) => map.clone(),
        None => return params.clone(),
    };
    for (index, field) in field_order::<P>().iter().enumerate() {
        if let Some(value) = args.get(index)
            && !object.contains_key(field)
        {
            object.insert(field.clone(), value.clone());
        }
    }
    Value::Object(object)
}

/// Registers the builtin commands: `ping` then `help`, in that order.
///
/// The C++ registers render → scene → asset → animation → physics; the five
/// domain phases add their `register_*_commands` here. Phase 1 lands only the two
/// builtins (`ping`, `help`), which live at the head of the render group in C++
/// (`registerRenderCommands`).
pub fn register_builtin_commands(reg: &mut CommandRegistry) {
    reg.register::<PingParams, PingResult>("ping", "liveness + engine info", |_ctx, _params| {
        Ok(PingResult {
            pong: true,
            engine: saffron_core::ENGINE_NAME.to_owned(),
            version: saffron_core::ENGINE_VERSION.to_owned(),
            pid: process_id(),
        })
    });

    // `help` reflects over the live registry, so `dispatch` serves it directly
    // from the registration order; the row registered here exists so `help`
    // lists itself and resolves for the editor palette. Its registered body is
    // never invoked.
    reg.register_raw("help", "list available commands", |_ctx, _params| {
        Ok(json!({ "commands": [] }))
    });

    // The domain groups register in the frozen order render → scene → animation → physics
    // → asset. `help` and the manifest-completeness check iterate the registry as a set, so
    // the asset group is the manifest tail (`get-project` … `quit`); the scene group lands
    // between render and animation, mirroring the C++ registration sequence.
    crate::commands_render::register_render_commands(reg);
    crate::commands_scene::register_scene_commands(reg);
    crate::commands_animation::register_animation_commands(reg);
    crate::commands_physics::register_physics_commands(reg);
    crate::commands_asset::register_asset_commands(reg);
}

/// The process id, for the `ping` reply (the C++ `::getpid()`).
fn process_id() -> i32 {
    i32::try_from(std::process::id()).unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::{StubRenderer, with_stub};

    /// Runs `body` against a fresh `EngineContext` over a default renderer stub.
    fn with_ctx<T>(body: impl FnOnce(&mut EngineContext<'_>) -> T) -> T {
        let mut renderer = StubRenderer::default();
        with_stub(&mut renderer, body)
    }

    fn builtins() -> CommandRegistry {
        let mut reg = CommandRegistry::new();
        register_builtin_commands(&mut reg);
        reg
    }

    #[test]
    fn ping_reports_engine_identity_and_pid() {
        let reg = builtins();
        let reply = with_ctx(|ctx| reg.dispatch(ctx, &json!({ "id": 1, "cmd": "ping" })));
        assert_eq!(reply["id"], json!(1));
        assert_eq!(reply["ok"], json!(true));
        let result = &reply["result"];
        assert_eq!(result["pong"], json!(true));
        assert_eq!(result["engine"], json!(saffron_core::ENGINE_NAME));
        assert_eq!(result["version"], json!(saffron_core::ENGINE_VERSION));
        assert_eq!(result["pid"], json!(process_id()));
    }

    /// Every command in the frozen protocol manifest (`saffron_protocol::COMMANDS`) has a
    /// handler in the builtin registry — except `get-script-schema`, which the host registers
    /// (it needs the Lua schema reader) — and the registry registers nothing the manifest does
    /// not declare. This is the manifest-completeness contract (set equality, not order: the
    /// registry iterates in C++ registration order while the manifest table is the generation
    /// order, and each domain's intra-order is locked by its own per-file order test). A
    /// command added to the protocol table without a matching handler — the exact gap that
    /// dropped `get/set-debug-overlays` — trips here.
    #[test]
    fn registry_covers_the_protocol_manifest() {
        use std::collections::BTreeSet;
        const HOST_REGISTERED: &[&str] = &["get-script-schema"];
        let reg = builtins();
        // `help` is the reflective-registry builtin the manifest skips by design
        // (`HELP_COMMAND`, never in `COMMANDS`), so it is not a manifest row.
        let registered: BTreeSet<&str> = reg
            .rows()
            .iter()
            .map(|c| c.name)
            .filter(|&name| name != saffron_protocol::HELP_COMMAND)
            .collect();
        let expected: BTreeSet<&str> = saffron_protocol::COMMANDS
            .iter()
            .map(|c| c.name)
            .filter(|name| !HOST_REGISTERED.contains(name))
            .collect();
        let missing: Vec<&&str> = expected.difference(&registered).collect();
        let unexpected: Vec<&&str> = registered.difference(&expected).collect();
        assert!(
            missing.is_empty(),
            "protocol commands with no registered handler: {missing:?}"
        );
        assert!(
            unexpected.is_empty(),
            "registered commands not in the protocol manifest: {unexpected:?}"
        );
    }

    #[test]
    fn help_lists_commands_in_registration_order() {
        let reg = builtins();
        let reply = with_ctx(|ctx| reg.dispatch(ctx, &json!({ "cmd": "help" })));
        assert_eq!(reply["ok"], json!(true));
        let commands = reply["result"]["commands"].as_array().unwrap();
        // ping is registered first, help second, then the render domain (the frozen
        // C++ order). The two builtins lead; render-stats opens the render group.
        assert!(commands.len() >= 3);
        assert_eq!(commands[0]["name"], json!("ping"));
        assert_eq!(commands[0]["help"], json!("liveness + engine info"));
        assert_eq!(commands[1]["name"], json!("help"));
        assert_eq!(commands[1]["help"], json!("list available commands"));
        assert_eq!(commands[2]["name"], json!("render-stats"));
    }

    #[test]
    fn unknown_command_is_an_error_with_id_echoed() {
        let reg = builtins();
        let reply = with_ctx(|ctx| reg.dispatch(ctx, &json!({ "cmd": "nope" })));
        // No id in the request → null in the reply.
        assert_eq!(reply["id"], Value::Null);
        assert_eq!(reply["ok"], json!(false));
        assert_eq!(reply["error"], json!("unknown command 'nope'"));
        assert!(reply.get("result").is_none());
    }

    #[test]
    fn id_echoes_number_string_and_absent() {
        let reg = builtins();
        let number = with_ctx(|ctx| reg.dispatch(ctx, &json!({ "id": 7, "cmd": "ping" })));
        assert_eq!(number["id"], json!(7));

        let string = with_ctx(|ctx| reg.dispatch(ctx, &json!({ "id": "abc", "cmd": "ping" })));
        assert_eq!(string["id"], json!("abc"));

        let absent = with_ctx(|ctx| reg.dispatch(ctx, &json!({ "cmd": "ping" })));
        assert_eq!(absent["id"], Value::Null);
    }

    #[test]
    fn missing_cmd_is_an_unknown_empty_command() {
        let reg = builtins();
        let reply = with_ctx(|ctx| reg.dispatch(ctx, &json!({ "id": 3 })));
        assert_eq!(reply["ok"], json!(false));
        assert_eq!(reply["error"], json!("unknown command ''"));
    }

    #[test]
    fn typed_register_surfaces_a_params_deserialize_error() {
        let mut reg = CommandRegistry::new();
        reg.register::<Vec3Holder, Vec3Holder>("echo", "echo", |_ctx, p| Ok(p));
        // `x` must be a number; a string fails the typed deserialize and becomes
        // the envelope error rather than a panic.
        let reply =
            with_ctx(|ctx| reg.dispatch(ctx, &json!({ "cmd": "echo", "params": { "x": "nan" } })));
        assert_eq!(reply["ok"], json!(false));
        assert!(reply.get("error").is_some());
    }

    #[derive(serde::Serialize, serde::Deserialize, schemars::JsonSchema)]
    struct Vec3Holder {
        x: f32,
    }

    #[test]
    fn typed_register_folds_positional_args_onto_named_fields() {
        use saffron_protocol::CreateEntityParams;
        // `create-entity` with a bare positional arg (the `sa create-entity foo` / e2e
        // `{ args: ["foo"] }` form) folds `args[0]` onto the `name` field.
        let folded = fold_positional_args::<CreateEntityParams>(&json!({ "args": ["foo"] }));
        assert_eq!(folded, json!({ "args": ["foo"], "name": "foo" }));
        // A named key wins over its positional slot.
        let named =
            fold_positional_args::<CreateEntityParams>(&json!({ "name": "kept", "args": ["x"] }));
        assert_eq!(named["name"], json!("kept"));
        // No `args` array → untouched.
        let plain = fold_positional_args::<CreateEntityParams>(&json!({ "name": "n" }));
        assert_eq!(plain, json!({ "name": "n" }));
    }

    #[test]
    fn typed_register_rejects_an_invalid_positional_enum_value() {
        // `set-aa nonsense` (`{ args: ["nonsense"] }`) folds onto `mode`, then fails the
        // `AaModeDto` enum deserialize — the negative oracle the e2e asserts (bad input is
        // rejected, not silently accepted as an absent optional).
        let mut reg = CommandRegistry::new();
        register_builtin_commands(&mut reg);
        let reply = with_ctx(|ctx| {
            reg.dispatch(
                ctx,
                &json!({ "cmd": "set-aa", "params": { "args": ["nonsense"] } }),
            )
        });
        assert_eq!(reply["ok"], json!(false));
    }

    #[test]
    fn positional_or_prefers_named_then_args_then_null() {
        // Named key wins.
        let named = json!({ "entity": 5, "args": [9] });
        assert_eq!(positional_or(&named, "entity", 0), json!(5));
        // Falls back to the index-th positional arg.
        let positional = json!({ "args": [9, 10] });
        assert_eq!(positional_or(&positional, "entity", 1), json!(10));
        // Neither present → null.
        let empty = json!({});
        assert_eq!(positional_or(&empty, "entity", 0), Value::Null);
        // Out-of-range positional → null.
        assert_eq!(positional_or(&positional, "entity", 5), Value::Null);
    }
}
