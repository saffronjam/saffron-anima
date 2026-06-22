//! `saffron-player`: the standalone runtime that runs an exported Saffron app.
//!
//! It loads a project from the directory beside its executable (an `app.json` manifest +
//! `project.json` + `assets/` + `src/` + `shaders/`, the layout the editor's Export produces),
//! opens a real window, and runs the scene as a live simulation — animation, physics, and Luau
//! scripts — through the shared [`saffron_runtime::RuntimeSession`]. It links none of the editor
//! stack: no control plane, no shared-memory frame publishing, no gizmo overlay. Material shaders
//! are loaded pre-baked (`.spv`); the player never invokes `slangc`.

#![deny(unsafe_code)]

use std::cell::RefCell;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::rc::Rc;

use saffron_app::{App, AppConfig, Layer, attach_layer, run};
use saffron_assets::{
    AssetServer, ProjectHost, ProjectInfo, RenderSceneOptions, RendererScene, render_scene,
};
use saffron_core::TimeSpan;
use saffron_protocol::AppManifest;
use saffron_rendering::{GpuQueue, Renderer, Uploader};
use saffron_runtime::RuntimeSession;
use saffron_scene::{ComponentRegistry, Scene, ScriptInputState, register_builtin_components};
use saffron_window::keyboard::{KeyCode, PhysicalKey};
use saffron_window::{
    ElementState, MouseButton, MouseScrollDelta, Window, WindowConfig, WindowEvent,
};

fn main() -> ExitCode {
    saffron_log::init_logging();

    let project_dir = resolve_project_dir();
    let manifest = load_manifest(&project_dir);
    tracing::info!(
        "saffron-player: '{}' ({}x{}) from {}",
        manifest.title,
        manifest.width,
        manifest.height,
        project_dir.display()
    );
    // v1 limitation: the window backend presents FIFO (vsync on) and has no fullscreen path yet,
    // so these manifest fields are read but not yet applied — surfaced rather than silently dropped.
    if manifest.fullscreen {
        tracing::warn!("saffron-player: fullscreen requested but not yet applied (v1)");
    }
    if !manifest.vsync {
        tracing::warn!("saffron-player: vsync=false requested but present mode is FIFO (v1)");
    }

    let window = WindowConfig {
        title: manifest.title.clone(),
        width: manifest.width,
        height: manifest.height,
        hidden: false,
    };
    let config = AppConfig {
        window,
        on_create: Box::new(move |app: &mut App| {
            attach_layer(app, Box::new(PlayerLayer::new(project_dir, manifest)));
        }),
        on_exit: Box::new(|_app| {}),
    };
    let code = run(config);
    ExitCode::from(u8::try_from(code).unwrap_or(1))
}

/// Resolves the project directory: an explicit CLI argument, else `SAFFRON_PROJECT`, else the
/// directory containing the executable (the staged-folder default).
fn resolve_project_dir() -> PathBuf {
    if let Some(arg) = std::env::args().nth(1) {
        return PathBuf::from(arg);
    }
    if let Ok(env) = std::env::var("SAFFRON_PROJECT")
        && !env.is_empty()
    {
        return PathBuf::from(env);
    }
    std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(Path::to_path_buf))
        .unwrap_or_else(|| PathBuf::from("."))
}

/// Reads `app.json` from the project directory, falling back to defaults (field-by-field via
/// `#[serde(default)]`) when it is absent or unparseable.
fn load_manifest(dir: &Path) -> AppManifest {
    let path = dir.join("app.json");
    match std::fs::read_to_string(&path) {
        Ok(text) => serde_json::from_str(&text).unwrap_or_else(|err| {
            tracing::warn!("saffron-player: app.json parse failed ({err}); using defaults");
            AppManifest::default()
        }),
        Err(_) => {
            tracing::info!(
                "saffron-player: no app.json at {}; using defaults",
                path.display()
            );
            AppManifest::default()
        }
    }
}

/// The [`ProjectHost`] adapter over the renderer, for `AssetServer::load_project` (the GPU-idle +
/// render-settings serde it needs). Wraps `&mut Renderer` directly — the player has no control
/// plane to route through.
struct PlayerProjectHost<'a> {
    renderer: &'a mut Renderer,
}

impl ProjectHost for PlayerProjectHost<'_> {
    fn wait_gpu_idle(&mut self) {
        if let Err(err) = self.renderer.device().wait_idle() {
            tracing::error!("saffron-player: wait_gpu_idle: {err}");
        }
    }

    fn render_settings_to_json(&self) -> serde_json::Value {
        self.renderer.render_settings_to_json()
    }

    fn apply_render_settings(&mut self, settings: &serde_json::Value) {
        self.renderer.apply_render_settings(settings);
    }
}

/// The player's single [`Layer`]: loads the project + starts the runtime on attach, advances the
/// simulation each update, renders the scene through its primary camera each frame, and tears the
/// runtime down before the renderer drops.
struct PlayerLayer {
    project_dir: PathBuf,
    manifest: AppManifest,
    scene: Scene,
    assets: AssetServer,
    runtime: RuntimeSession,
    registry: ComponentRegistry,
    project: ProjectInfo,
    uploader: Option<Uploader>,
    /// The gameplay input, shared with the window-signal closures that mutate it.
    input: Rc<RefCell<ScriptInputState>>,
    started: bool,
    warned_no_camera: bool,
}

impl PlayerLayer {
    fn new(project_dir: PathBuf, manifest: AppManifest) -> Self {
        let assets = AssetServer::new(project_dir.join("assets"));
        Self {
            project_dir,
            manifest,
            scene: Scene::new(),
            assets,
            runtime: RuntimeSession::new(),
            registry: register_builtin_components(),
            project: ProjectInfo::default(),
            uploader: None,
            input: Rc::new(RefCell::new(ScriptInputState::default())),
            started: false,
            warned_no_camera: false,
        }
    }

    /// Lazily builds the one-off uploader from the renderer's device + queue (asset GPU uploads).
    fn ensure_uploader(&mut self, renderer: &Renderer) {
        if self.uploader.is_some() {
            return;
        }
        let queue = GpuQueue::new(renderer.device().graphics_queue);
        match Uploader::new(renderer.device(), &queue) {
            Ok(uploader) => self.uploader = Some(uploader),
            Err(err) => tracing::error!("saffron-player: uploader create failed: {err}"),
        }
    }

    /// Routes the runtime's buffered script logs/errors to the console.
    fn drain_logs(&mut self) {
        for line in self.runtime.take_logs() {
            tracing::info!("[script] {}", line.message);
        }
        for err in self.runtime.take_errors() {
            tracing::error!("[script error] {}: {}", err.script, err.message);
        }
    }

    /// Subscribes the window input signals into the shared [`ScriptInputState`]: held keys via the
    /// typed key signals, mouse position/buttons/scroll via the raw-event signal.
    fn wire_input(&self, window: &Window) {
        let input = Rc::clone(&self.input);
        window.on_key_pressed.subscribe(move |(key, _repeat)| {
            if let Some(name) = key_name(key) {
                input.borrow_mut().held.insert(name);
            }
            false
        });
        let input = Rc::clone(&self.input);
        window.on_key_released.subscribe(move |key| {
            if let Some(name) = key_name(key) {
                input.borrow_mut().held.remove(&name);
            }
            false
        });
        let input = Rc::clone(&self.input);
        window.on_raw_event.subscribe(move |event| {
            apply_mouse_event(&mut input.borrow_mut(), &event);
            false
        });
    }
}

impl Layer for PlayerLayer {
    fn name(&self) -> &str {
        "PlayerLayer"
    }

    fn on_attach(&mut self, app: &mut App) {
        let Some(renderer) = app.frame_host.renderer_mut() else {
            tracing::error!("saffron-player: no renderer; cannot start");
            return;
        };
        renderer.set_present_viewport_only(true);
        self.ensure_uploader(renderer);

        let selection = self.project_dir.join("project.json");
        let selection = selection.to_string_lossy().into_owned();
        {
            let mut host = PlayerProjectHost { renderer };
            match self.assets.load_project(
                &mut host,
                &self.registry,
                &mut self.scene,
                &mut self.project,
                &selection,
                "",
            ) {
                Ok(_sidecar) => tracing::info!(
                    "saffron-player: loaded project '{}'",
                    self.project.display_name
                ),
                Err(err) => {
                    tracing::error!("saffron-player: load project failed: {err}");
                    return;
                }
            }
        }

        // Start the live simulation (build the Jolt world + script VM from the loaded scene).
        let project_dir = self.project_dir.clone();
        self.runtime
            .start(&mut self.scene, &mut self.assets, &project_dir);
        self.drain_logs();
        self.started = true;

        if let Some(window) = app.window.as_ref() {
            self.wire_input(window);
        }
        tracing::info!("saffron-player: running '{}'", self.manifest.title);
    }

    fn on_update(&mut self, _app: &mut App, dt: TimeSpan) {
        if !self.started {
            return;
        }
        {
            let mut input = self.input.borrow_mut();
            self.runtime
                .advance(&mut self.scene, &mut self.assets, dt.seconds, &mut input);
        }
        self.drain_logs();
    }

    fn on_ui(&mut self, app: &mut App) {
        if !self.started {
            return;
        }
        let Some(renderer) = app.frame_host.renderer_mut() else {
            return;
        };
        // Track the window size so the offscreen the present blits from is native-resolution.
        if let Some(window) = app.window.as_ref() {
            let view = renderer.active_view_id();
            let _ = renderer.set_viewport_desired_size(view, window.width(), window.height());
        }
        self.ensure_uploader(renderer);
        let Some(uploader) = self.uploader.as_ref() else {
            return;
        };
        if renderer.viewport_width() == 0 || renderer.viewport_height() == 0 {
            return;
        }

        if let Some(cam) = self.scene.primary_camera() {
            let skinning = renderer.skinning_enabled();
            let mut driver = RendererScene::new(renderer, uploader, skinning);
            let options = RenderSceneOptions {
                show_editor_camera_models: false,
                show_grid: false,
            };
            render_scene(
                &mut driver,
                &mut self.scene,
                &mut self.assets,
                &cam,
                options,
            );
        } else if !self.warned_no_camera {
            tracing::warn!("saffron-player: scene has no primary camera; rendering sky only");
            self.warned_no_camera = true;
        }

        if let Err(err) = renderer.render_scene_offscreen() {
            tracing::error!("saffron-player: render_scene_offscreen: {err}");
        }
    }

    fn on_detach(&mut self, _app: &mut App) {
        // Teardown order mirrors the host's, before the renderer drops (the loop already idled the
        // GPU): stop scripts → drop the world → shut down the Jolt globals → release GPU caches so
        // the last `Arc<GpuMesh>`/`Arc<GpuTexture>` drops under a live-but-idle device.
        self.runtime.stop_scripts();
        self.runtime.drop_physics_world();
        self.runtime.shutdown_physics_globals();
        self.uploader = None;
        self.assets.clear_asset_caches();
    }
}

/// Maps a winit physical key to the lowercase name scripts read from `sa.input` (matching the
/// control plane's `to_ascii_lowercase` convention): letters → `"a"`..`"z"`, digits → `"0"`..`"9"`,
/// plus the common named/movement/modifier keys. Unmapped keys are ignored.
fn key_name(key: PhysicalKey) -> Option<String> {
    let PhysicalKey::Code(code) = key else {
        return None;
    };
    let name = match code {
        KeyCode::Space => "space",
        KeyCode::Enter => "enter",
        KeyCode::Escape => "escape",
        KeyCode::Tab => "tab",
        KeyCode::Backspace => "backspace",
        KeyCode::ArrowUp => "up",
        KeyCode::ArrowDown => "down",
        KeyCode::ArrowLeft => "left",
        KeyCode::ArrowRight => "right",
        KeyCode::ShiftLeft | KeyCode::ShiftRight => "shift",
        KeyCode::ControlLeft | KeyCode::ControlRight => "ctrl",
        KeyCode::AltLeft | KeyCode::AltRight => "alt",
        other => {
            let dbg = format!("{other:?}");
            if let Some(letter) = dbg.strip_prefix("Key") {
                return Some(letter.to_ascii_lowercase());
            }
            if let Some(digit) = dbg.strip_prefix("Digit") {
                return Some(digit.to_string());
            }
            return None;
        }
    };
    Some(name.to_string())
}

/// Folds a raw mouse [`WindowEvent`] into the gameplay input: cursor position, button held-set,
/// and accumulated scroll. Keyboard events are handled by the typed signals, so they are ignored
/// here.
fn apply_mouse_event(input: &mut ScriptInputState, event: &WindowEvent) {
    match event {
        WindowEvent::CursorMoved { position, .. } => {
            input.mouse_x = position.x as f32;
            input.mouse_y = position.y as f32;
        }
        WindowEvent::MouseInput { state, button, .. } => {
            if let Some(name) = mouse_button_name(*button) {
                match state {
                    ElementState::Pressed => {
                        input.mouse_buttons.insert(name);
                    }
                    ElementState::Released => {
                        input.mouse_buttons.remove(&name);
                    }
                }
            }
        }
        WindowEvent::MouseWheel { delta, .. } => {
            input.scroll += match delta {
                MouseScrollDelta::LineDelta(_, y) => *y,
                MouseScrollDelta::PixelDelta(p) => p.y as f32,
            };
        }
        _ => {}
    }
}

/// The lowercase name for a mouse button (`"left"`/`"right"`/`"middle"`); others are ignored.
fn mouse_button_name(button: MouseButton) -> Option<String> {
    Some(
        match button {
            MouseButton::Left => "left",
            MouseButton::Right => "right",
            MouseButton::Middle => "middle",
            _ => return None,
        }
        .to_string(),
    )
}
