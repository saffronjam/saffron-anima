use raw_window_handle::{HasWindowHandle, RawWindowHandle};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::fs;
use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::Mutex;
use std::thread;
use std::time::Duration;
use tauri::{AppHandle, Emitter, Manager, RunEvent, State};

#[derive(Clone, Copy, Debug, Deserialize, Serialize)]
struct Bounds {
    x: i32,
    y: i32,
    width: i32,
    height: i32,
}

struct EditorState {
    engine: Mutex<Option<Child>>,
    socket_path: String,
}

impl Default for EditorState {
    fn default() -> Self {
        Self {
            engine: Mutex::new(None),
            socket_path: socket_path(),
        }
    }
}

// Per-PID socket in XDG_RUNTIME_DIR so two editor instances get distinct engines/sockets.
fn socket_path() -> String {
    let dir = std::env::var("XDG_RUNTIME_DIR").unwrap_or_else(|_| "/tmp".to_string());
    format!("{dir}/saffron-editor-{}.sock", std::process::id())
}

fn engine_binary() -> String {
    std::env::var("SAFFRON_ENGINE_BIN").unwrap_or_else(|_| {
        repo_root()
            .join("build/debug/bin/SaffronEditor")
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

fn parent_xid(window: &tauri::WebviewWindow) -> Result<u64, String> {
    let handle = window
        .window_handle()
        .map_err(|err| format!("get editor window handle: {err}"))?;
    match handle.as_raw() {
        RawWindowHandle::Xlib(handle) => Ok(handle.window),
        other => Err(format!("unsupported editor window handle: {other:?}")),
    }
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
    Command::new(engine_binary())
        .env("SAFFRON_EDITOR_NATIVE_VIEWPORT", "1")
        .env("SAFFRON_CONTROL_SOCK", socket_path)
        .env("SDL_VIDEODRIVER", "x11")
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
}

// ONE generic passthrough: any `se` command reaches the engine with zero Rust changes.
#[tauri::command]
fn control(
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

#[tauri::command]
async fn attach_native_viewport(
    window: tauri::WebviewWindow,
    state: State<'_, EditorState>,
    bounds: Bounds,
) -> Result<(), String> {
    let parent = parent_xid(&window)?;
    // parentXid as a string so the u64 XID survives JSON without precision loss.
    control_request_with_params(
        &state.socket_path,
        "attach-native-viewport",
        json!({
            "parentXid": parent.to_string(),
            "x": bounds.x, "y": bounds.y, "width": bounds.width, "height": bounds.height
        }),
    )
    .map(|_| ())
}

#[tauri::command]
fn resize_native_viewport(state: State<'_, EditorState>, bounds: Bounds) -> Result<(), String> {
    // The phase-1 engine command does XMoveResize only (no reparent) → no flicker.
    control_request_with_params(
        &state.socket_path,
        "resize-native-viewport",
        json!({ "x": bounds.x, "y": bounds.y, "width": bounds.width, "height": bounds.height }),
    )
    .map(|_| ())
}

#[tauri::command]
fn quit_engine(state: State<'_, EditorState>) -> Result<(), String> {
    teardown(&state);
    Ok(())
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

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(EditorState::default())
        .invoke_handler(tauri::generate_handler![
            control,
            start_engine,
            attach_native_viewport,
            resize_native_viewport,
            quit_engine,
            engine_alive
        ])
        .setup(|app| {
            if let Some(window) = app.get_webview_window("main") {
                let _ = window.set_title("Saffron Editor");
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
