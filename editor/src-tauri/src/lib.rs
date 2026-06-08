use gtk::prelude::WidgetExt;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::fs;
use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;
use tauri::{AppHandle, Emitter, LogicalSize, Manager, RunEvent, State};

mod wayland_viewport;

/// The NVIDIA Vulkan ICD the toolbox lacks but the host provides; also the marker that
/// this is an NVIDIA box (the WebKit env workarounds below key off it).
const NVIDIA_ICD: &str = "/run/host/usr/share/vulkan/icd.d/nvidia_icd.x86_64.json";

fn nvidia_present() -> bool {
    std::path::Path::new(NVIDIA_ICD).exists() || std::path::Path::new("/sys/module/nvidia").exists()
}

struct EditorState {
    engine: Mutex<Option<Child>>,
    socket_path: String,
    viewport: Arc<wayland_viewport::ViewportShared>,
}

const MAIN_WINDOW_WIDTH: f64 = 1600.0;
const MAIN_WINDOW_HEIGHT: f64 = 900.0;
const MAIN_WINDOW_MIN_WIDTH: f64 = 1200.0;
const MAIN_WINDOW_MIN_HEIGHT: f64 = 720.0;

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct AppDataInfo {
    app_data_dir: String,
    userdata_dir: String,
    env_project: bool,
    auto_empty_project: bool,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct RecentProject {
    path: String,
    name: String,
    display_name: String,
    last_opened_at: String,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
struct RecentProjects {
    projects: Vec<RecentProject>,
}

/// Editor-wide settings persisted as deltas in appdata/settings.json: `key_bindings`
/// holds only the commands the user changed (command id → key-string); defaults live
/// in the frontend registry. An untyped map so adding commands never touches Rust.
#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct EditorSettings {
    #[serde(default)]
    key_bindings: std::collections::HashMap<String, String>,
}

impl Default for EditorState {
    fn default() -> Self {
        Self {
            engine: Mutex::new(None),
            socket_path: socket_path(),
            viewport: Arc::default(),
        }
    }
}

// Per-PID socket in XDG_RUNTIME_DIR so two editor instances get distinct engines/sockets.
fn socket_path() -> String {
    let dir = std::env::var("XDG_RUNTIME_DIR").unwrap_or_else(|_| "/tmp".to_string());
    format!("{dir}/saffron-editor-{}.sock", std::process::id())
}

// Per-PID shm segment the engine publishes viewport frames into; the presenter maps it.
fn viewport_shm_name() -> String {
    format!("/saffron-viewport-{}", std::process::id())
}

fn engine_binary() -> String {
    std::env::var("SAFFRON_ENGINE_BIN").unwrap_or_else(|_| {
        repo_root()
            .join("build/debug/bin/SaffronEngine")
            .to_string_lossy()
            .into_owned()
    })
}

fn repo_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(|path| path.parent())
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."))
}

fn app_data_dir() -> PathBuf {
    repo_root().join("appdata")
}

fn userdata_dir() -> PathBuf {
    app_data_dir().join("userdata")
}

fn recents_path() -> PathBuf {
    app_data_dir().join("recent-projects.json")
}

fn ensure_app_dirs() -> Result<(), String> {
    fs::create_dir_all(userdata_dir()).map_err(|err| format!("create app data dirs: {err}"))
}

fn read_recent_projects_file() -> RecentProjects {
    let path = recents_path();
    let Ok(text) = fs::read_to_string(path) else {
        return RecentProjects::default();
    };
    serde_json::from_str(&text).unwrap_or_default()
}

fn write_recent_projects_file(recents: &RecentProjects) -> Result<(), String> {
    ensure_app_dirs()?;
    let text = serde_json::to_string_pretty(recents)
        .map_err(|err| format!("encode recent projects: {err}"))?;
    fs::write(recents_path(), text).map_err(|err| format!("write recent projects: {err}"))
}

fn settings_path() -> PathBuf {
    app_data_dir().join("settings.json")
}

// A missing or corrupt settings file falls back to defaults (an empty delta map).
fn read_settings_file() -> EditorSettings {
    let Ok(text) = fs::read_to_string(settings_path()) else {
        return EditorSettings::default();
    };
    serde_json::from_str(&text).unwrap_or_default()
}

fn write_settings_file(settings: &EditorSettings) -> Result<(), String> {
    ensure_app_dirs()?;
    let text = serde_json::to_string_pretty(settings)
        .map_err(|err| format!("encode editor settings: {err}"))?;
    fs::write(settings_path(), text).map_err(|err| format!("write editor settings: {err}"))
}

fn configure_main_window(window: &tauri::WebviewWindow) {
    let _ = window.set_title("Saffron Editor");
    let _ = window.set_min_size(Some(LogicalSize::new(
        MAIN_WINDOW_MIN_WIDTH,
        MAIN_WINDOW_MIN_HEIGHT,
    )));
    let _ = window.set_size(LogicalSize::new(MAIN_WINDOW_WIDTH, MAIN_WINDOW_HEIGHT));
}

// The one socket round-trip helper the whole bridge is built on. Newline-delimited JSON;
// surfaces the engine's `ok:false` error string as a typed Err (→ a rejected JS promise).
fn control_request_with_params(
    socket_path: &str,
    command: &str,
    params: Value,
) -> Result<Value, String> {
    let mut stream = UnixStream::connect(socket_path)
        .map_err(|err| format!("control socket unavailable: {err}"))?;
    stream
        .set_read_timeout(Some(Duration::from_millis(1000)))
        .map_err(|err| format!("set read timeout: {err}"))?;
    let mut request = json!({ "id": 1, "cmd": command, "params": params }).to_string();
    request.push('\n');
    stream
        .write_all(request.as_bytes())
        .map_err(|err| format!("send control request: {err}"))?;

    let mut reply = String::new();
    let mut buffer = [0_u8; 4096];
    while !reply.contains('\n') {
        let read = stream
            .read(&mut buffer)
            .map_err(|err| format!("read control reply: {err}"))?;
        if read == 0 {
            break;
        }
        reply.push_str(&String::from_utf8_lossy(&buffer[..read]));
    }
    let value: Value =
        serde_json::from_str(reply.trim()).map_err(|err| format!("decode control reply: {err}"))?;
    if value.get("ok").and_then(|v| v.as_bool()).unwrap_or(false) {
        return Ok(value.get("result").cloned().unwrap_or_default());
    }
    Err(value
        .get("error")
        .and_then(|v| v.as_str())
        .unwrap_or("control command failed")
        .to_string())
}

fn control_request(socket_path: &str, command: &str) -> Result<Value, String> {
    control_request_with_params(socket_path, command, json!({}))
}

fn spawn_engine(socket_path: &str) -> Result<Child, String> {
    let _ = fs::remove_file(socket_path);
    ensure_app_dirs()?;
    let mut command = Command::new(engine_binary());
    command
        .env("SAFFRON_EDITOR_NATIVE_VIEWPORT", "1")
        .env("SAFFRON_CONTROL_SOCK", socket_path)
        .env("SAFFRON_APPDATA_DIR", app_data_dir())
        // The engine publishes frames into shared memory for the subsurface presenter
        // instead of presenting to its (hidden) swapchain.
        .env("SAFFRON_VIEWPORT_SHM", viewport_shm_name());
    // Unthrottled headless publish renders thousands of fps for nothing; cap well above
    // any display rate. An explicit SAFFRON_MAX_FPS in the environment wins.
    if std::env::var_os("SAFFRON_MAX_FPS").is_none() {
        command.env("SAFFRON_MAX_FPS", "500");
    }
    // The toolbox ships only Mesa ICD manifests; point Vulkan at the host's NVIDIA ICD
    // so the engine renders on hardware instead of llvmpipe.
    if std::env::var_os("VK_ICD_FILENAMES").is_none() && std::path::Path::new(NVIDIA_ICD).exists() {
        command.env("VK_ICD_FILENAMES", NVIDIA_ICD);
    }
    command
        .current_dir(repo_root())
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit())
        .spawn()
        .map_err(|err| format!("failed to start engine host: {err}"))
}

// True only if the child is spawned AND has not exited — via try_wait (reaping), never
// Option::is_some (which reports a crashed engine as still running).
fn child_alive(engine: &Mutex<Option<Child>>) -> bool {
    let Ok(mut guard) = engine.lock() else {
        return false;
    };
    match guard.as_mut() {
        Some(child) => matches!(child.try_wait(), Ok(None)),
        None => false,
    }
}

fn teardown(state: &EditorState) {
    let _ = control_request(&state.socket_path, "quit");
    if let Ok(mut guard) = state.engine.lock() {
        if let Some(mut child) = guard.take() {
            let _ = child.kill();
            let _ = child.wait();
        }
    }
    let _ = fs::remove_file(&state.socket_path);
    // The engine unlinks its shm segment on clean exit; cover the killed case too.
    let _ = fs::remove_file(format!("/dev/shm{}", viewport_shm_name()));
}

// ONE generic passthrough: any `se` command reaches the engine with zero Rust changes.
// Async so the blocking socket round trip runs on a worker, never the main thread
// driving the webview event loop (a sync command would stall the UI during edit streams).
#[tauri::command]
async fn control(
    state: State<'_, EditorState>,
    cmd: String,
    params: Option<Value>,
) -> Result<Value, String> {
    control_request_with_params(&state.socket_path, &cmd, params.unwrap_or_else(|| json!({})))
}

#[tauri::command]
fn engine_alive(state: State<'_, EditorState>) -> bool {
    child_alive(&state.engine)
}

#[tauri::command]
fn start_engine(state: State<'_, EditorState>) -> Result<(), String> {
    if child_alive(&state.engine) {
        return Ok(());
    }
    let child = spawn_engine(&state.socket_path)?;
    state
        .engine
        .lock()
        .map_err(|_| "engine lock poisoned".to_string())?
        .replace(child);
    Ok(())
}

/// The viewport panel's logical CSS rect within the webview, plus the window scale
/// factor. The presenter positions the subsurface in logical coordinates; the engine
/// renders at device pixels.
#[derive(Clone, Copy, Debug, Deserialize, Serialize)]
struct ViewportBounds {
    x: f64,
    y: f64,
    width: f64,
    height: f64,
    scale: f64,
}

#[tauri::command]
async fn set_viewport_bounds(
    window: tauri::WebviewWindow,
    state: State<'_, EditorState>,
    bounds: ViewportBounds,
    resize_engine: bool,
) -> Result<(), String> {
    state.viewport.set_bounds(
        bounds.x.round() as i32,
        bounds.y.round() as i32,
        bounds.width.round() as i32,
        bounds.height.round() as i32,
    );
    // The subsurface position is double-buffered on the parent; nudge a parent commit.
    let nudge = window.clone();
    let _ = window.run_on_main_thread(move || {
        if let Ok(gtk_window) = nudge.gtk_window() {
            gtk_window.queue_draw();
        }
    });
    // Resizing the engine's render target recreates the offscreen chain (expensive), so
    // only the settled (debounced) bounds do it — live drag ticks stretch the current
    // frame via the subsurface instead. Ignore failures while the engine boots.
    if resize_engine {
        let width = ((bounds.width * bounds.scale).round() as i64).max(1);
        let height = ((bounds.height * bounds.scale).round() as i64).max(1);
        let _ = control_request_with_params(
            &state.socket_path,
            "set-viewport-size",
            json!({ "width": width, "height": height }),
        );
    }
    Ok(())
}

#[tauri::command]
fn set_viewport_hidden(
    window: tauri::WebviewWindow,
    state: State<'_, EditorState>,
    hidden: bool,
) -> Result<(), String> {
    state.viewport.set_hidden(hidden);
    // The opaque underlay punches its hole per the hidden flag; repaint it.
    let nudge = window.clone();
    let _ = window.run_on_main_thread(move || {
        if let Ok(gtk_window) = nudge.gtk_window() {
            gtk_window.queue_draw();
        }
    });
    Ok(())
}

#[tauri::command]
fn quit_engine(state: State<'_, EditorState>) -> Result<(), String> {
    teardown(&state);
    Ok(())
}

#[tauri::command]
fn app_data_info() -> Result<AppDataInfo, String> {
    ensure_app_dirs()?;
    Ok(AppDataInfo {
        app_data_dir: app_data_dir().to_string_lossy().into_owned(),
        userdata_dir: userdata_dir().to_string_lossy().into_owned(),
        env_project: std::env::var_os("SAFFRON_PROJECT").is_some(),
        auto_empty_project: std::env::var_os("SAFFRON_AUTO_EMPTY_PROJECT").is_some(),
    })
}

#[tauri::command]
fn list_recent_projects() -> Result<RecentProjects, String> {
    ensure_app_dirs()?;
    let mut recents = read_recent_projects_file();
    recents.projects.retain(|project| PathBuf::from(&project.path).exists());
    recents.projects.truncate(12);
    write_recent_projects_file(&recents)?;
    Ok(recents)
}

#[tauri::command]
fn load_editor_settings() -> Result<EditorSettings, String> {
    ensure_app_dirs()?;
    Ok(read_settings_file())
}

#[tauri::command]
fn save_editor_settings(settings: EditorSettings) -> Result<(), String> {
    write_settings_file(&settings)
}

#[tauri::command]
fn remember_recent_project(project: RecentProject) -> Result<RecentProjects, String> {
    ensure_app_dirs()?;
    let mut recents = read_recent_projects_file();
    recents.projects.retain(|recent| recent.path != project.path);
    recents.projects.insert(0, project);
    recents.projects.truncate(12);
    write_recent_projects_file(&recents)?;
    Ok(recents)
}

// Spawn the engine + poll readiness on a background thread, emitting engine-phase /
// viewport-error events. React drives the actual attach (it owns the viewport rect).
fn auto_start(handle: &AppHandle) -> Result<(), String> {
    let state = handle.state::<EditorState>();
    let child = spawn_engine(&state.socket_path)?;
    state
        .engine
        .lock()
        .map_err(|_| "engine lock poisoned".to_string())?
        .replace(child);
    let _ = handle.emit("engine-phase", "starting");

    let monitor = handle.clone();
    let socket = state.socket_path.clone();
    thread::spawn(move || {
        let mut delay = 50u64;
        for _ in 0..40 {
            thread::sleep(Duration::from_millis(delay));
            delay = (delay * 2).min(800);
            let st = monitor.state::<EditorState>();
            if !child_alive(&st.engine) {
                let _ = monitor.emit("viewport-error", "engine exited during startup");
                return;
            }
            if control_request(&socket, "viewport-native-info").is_ok() {
                let _ = monitor.emit("engine-phase", "attaching");
                return;
            }
        }
        let _ = monitor.emit("viewport-error", "engine control socket did not come up");
    });
    Ok(())
}

const STDERR_NOISE: [&str; 2] = ["pci id for fd ", "libEGL warning:"];

// Reroutes this process's stderr through a pipe and forwards every line except known
// Mesa GPU-probe chatter, which the loader prints unconditionally (no env gate) when
// WebKitGTK initializes EGL on an NVIDIA fd. The webview subprocesses and the engine
// inherit the filtered fd, so their stderr is covered too; engine logs go to stdout
// and are untouched. On any setup failure stderr is simply left as it was.
fn install_stderr_noise_filter() {
    use std::io::{BufRead, BufReader};
    use std::os::fd::FromRawFd;

    let mut fds = [0i32; 2];
    if unsafe { libc::pipe(fds.as_mut_ptr()) } != 0 {
        return;
    }
    let real = unsafe { libc::dup(libc::STDERR_FILENO) };
    if real < 0 || unsafe { libc::dup2(fds[1], libc::STDERR_FILENO) } < 0 {
        return;
    }
    unsafe { libc::close(fds[1]) };

    let reader = unsafe { fs::File::from_raw_fd(fds[0]) };
    let mut sink = unsafe { fs::File::from_raw_fd(real) };
    thread::spawn(move || {
        let mut buffered = BufReader::new(reader);
        let mut line = Vec::new();
        let mut dropped_previous = false;
        loop {
            line.clear();
            match buffered.read_until(b'\n', &mut line) {
                Ok(0) | Err(_) => return,
                Ok(_) => {}
            }
            let text = String::from_utf8_lossy(&line);
            let trimmed = text.trim();
            let noise = STDERR_NOISE.iter().any(|prefix| trimmed.starts_with(prefix))
                || (dropped_previous && trimmed.is_empty());
            if noise {
                dropped_previous = true;
                continue;
            }
            dropped_previous = false;
            if sink.write_all(&line).is_err() {
                return;
            }
        }
    });
}

pub fn run() {
    // WebKitGTK's default DMABUF renderer renders NOTHING on NVIDIA Wayland, and the
    // non-DMABUF fallback loses transparency (an opaque page would hide the subsurface).
    // Steering WebKit onto Mesa's software EGL keeps a transparent, working webview; the
    // engine still renders on the hardware ICD. Gated on NVIDIA so AMD/Intel keep the
    // fast DMABUF path. Explicit values in the environment win.
    #[cfg(target_os = "linux")]
    if nvidia_present() {
        if std::env::var_os("__EGL_VENDOR_LIBRARY_FILENAMES").is_none() {
            unsafe {
                std::env::set_var(
                    "__EGL_VENDOR_LIBRARY_FILENAMES",
                    "/usr/share/glvnd/egl_vendor.d/50_mesa.json",
                )
            };
        }
        if std::env::var_os("LIBGL_ALWAYS_SOFTWARE").is_none() {
            unsafe { std::env::set_var("LIBGL_ALWAYS_SOFTWARE", "1") };
        }
    }

    // WebKitGTK's EGL init on NVIDIA prints Mesa GPU-probe chatter to stderr before
    // falling back cleanly. EGL_LOG_LEVEL gates the "libEGL warning:" lines (respecting
    // an explicit value); the bare Mesa loader lines ("pci id for fd …") have no env
    // gate at all, so the stderr filter below drops them instead.
    #[cfg(target_os = "linux")]
    if std::env::var_os("EGL_LOG_LEVEL").is_none() {
        unsafe { std::env::set_var("EGL_LOG_LEVEL", "fatal") };
    }
    install_stderr_noise_filter();

    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(EditorState::default())
        .invoke_handler(tauri::generate_handler![
            control,
            start_engine,
            set_viewport_bounds,
            set_viewport_hidden,
            quit_engine,
            engine_alive,
            app_data_info,
            list_recent_projects,
            remember_recent_project,
            load_editor_settings,
            save_editor_settings
        ])
        .setup(|app| {
            if let Some(window) = app.get_webview_window("main") {
                configure_main_window(&window);
                let shared = Arc::clone(&app.state::<EditorState>().viewport);
                if let Err(err) = wayland_viewport::install(&window, viewport_shm_name(), shared) {
                    let _ = app.handle().emit("viewport-error", err);
                }
            }
            if let Err(err) = auto_start(app.handle()) {
                let _ = app.handle().emit("viewport-error", err);
            }
            Ok(())
        })
        .build(tauri::generate_context!())
        .expect("failed to build Saffron editor")
        .run(|handle, event| {
            if let RunEvent::ExitRequested { .. } = event {
                let state = handle.state::<EditorState>();
                teardown(&state);
            }
        });
}
