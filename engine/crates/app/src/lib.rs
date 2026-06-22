//! The run loop and the `Layer` lifecycle — the bare runnable spine of a Saffron
//! Anima host.
//!
//! Provides the [`Layer`] trait (a set of optional lifecycle hooks), the [`App`] /
//! [`AppConfig`] types, and [`run`] — the
//! `poll → on_update → begin_frame → on_render → on_ui → begin_frame_graph →
//! on_render_graph → end_frame` loop with the `SAFFRON_EXIT_AFTER_FRAMES` /
//! `SAFFRON_MAX_FPS` env knobs and the `wait_gpu_idle`-before-teardown ordering.
//!
//! Two modes share one [`run`]: a **windowed** standalone host (a winit window +
//! a surface-bound renderer that presents through a real swapchain) and a
//! **headless** editor host (no window, a feature-selected device), decided from
//! `SAFFRON_EDITOR_NATIVE_VIEWPORT`. They share the per-frame body ([`step_frame`])
//! and the `start` / `finish` bring-up/teardown; only the loop driver differs — a
//! plain `while` ([`drive`]) headless vs a winit [`ApplicationHandler`]
//! ([`run_windowed`]) windowed, since winit 0.30 owns its own loop. There is no shm
//! publish, no overlay, and no control plane here — those belong to `saffron-host`,
//! which is a single [`Layer`] plus the lifecycle closures this loop invokes.
//!
//! ## Why a [`FrameHost`] trait
//!
//! The loop drives `begin_frame`/`end_frame`/`wait_gpu_idle` through the
//! [`FrameHost`] trait so the loop's hook-dispatch order and frame-limit/fps logic
//! are testable without a GPU (the verification gate runs on software llvmpipe, or
//! on no device at all in the unit tests). The real [`saffron_rendering::Renderer`]
//! implements it; the loop holds it as `Box<dyn FrameHost>` so [`App`] stays a
//! concrete type and [`Layer`] stays object-safe (`Box<dyn Layer>`). This is the
//! same "device selection is a parameter, not a fork" principle the renderer's
//! [`SurfaceSource`] follows — one [`run`], the host swapped behind a trait.
//!
//! DAG: depends on `saffron-core`, `saffron-window`, `saffron-rendering`.

#![deny(unsafe_code)]

use std::time::{Duration, Instant};

use saffron_core::TimeSpan;
use saffron_rendering::{RenderGraph, Renderer, SurfaceSource};
use saffron_window::{
    ActiveEventLoop, ApplicationHandler, ControlFlow, EventLoop, Window, WindowConfig, WindowEvent,
    WindowId,
};

/// Errors from host bring-up and the run loop.
///
/// The failures are typed so a caller can `match`; [`run`] collapses them to a
/// process exit code at the top of the stack.
#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// The OS window could not be created (windowed mode only).
    #[error("failed to create window: {0}")]
    Window(#[from] saffron_window::Error),

    /// The Vulkan renderer could not be brought up.
    #[error("failed to create renderer: {0}")]
    Renderer(#[from] saffron_rendering::Error),

    /// The winit event loop (windowed mode only) failed to build or run.
    #[error("event loop failed: {0}")]
    EventLoop(String),
}

/// A `Result` whose error is this crate's [`Error`].
pub type Result<T> = std::result::Result<T, Error>;

/// The per-frame GPU host the loop drives.
///
/// Abstracting the renderer's frame entry points behind a trait keeps [`run`]
/// GPU-free-testable: the real [`Renderer`] implements it, and a no-op host drives
/// the loop in unit tests. There is exactly one loop; only the host behind it
/// differs.
pub trait FrameHost {
    /// Begins a frame: acquires the swapchain image (or, in publish mode, the
    /// staging slot) and prepares per-frame state. Returns `false` when the frame
    /// must be skipped (e.g. the swapchain went out of date).
    ///
    /// # Errors
    ///
    /// Propagates any host failure that should abort the loop.
    fn begin_frame(&mut self) -> Result<bool>;

    /// Builds the frame graph (cull + scene passes) into `graph` so layers may add
    /// passes against it in [`Layer::on_render_graph`]. The loop owns the graph for
    /// the hook pass and hands it to [`Self::end_frame`].
    fn begin_frame_graph(&mut self, graph: &mut RenderGraph);

    /// Finishes the frame: executes the graph (deriving every barrier) and
    /// presents or publishes the result.
    ///
    /// # Errors
    ///
    /// Propagates any host failure that should abort the loop.
    fn end_frame(&mut self, graph: RenderGraph) -> Result<()>;

    /// The current viewport size in pixels. A zero in either axis means the host
    /// is minimized and the frame is skipped.
    fn viewport_size(&self) -> (u32, u32);

    /// Blocks until the GPU is idle. Called once before any teardown so no
    /// in-flight command buffer references a resource about to drop — the single
    /// most load-bearing teardown ordering fact.
    ///
    /// # Errors
    ///
    /// Propagates a device-lost or wait failure.
    fn wait_gpu_idle(&self) -> Result<()>;

    /// Handles a window resize to `(width, height)` pixels: rebuilds the swapchain
    /// and the active offscreen view so the present path matches the new surface.
    /// The default is a no-op — the headless editor host has no swapchain and never
    /// receives a resize (it has no window).
    fn resized(&mut self, _width: u32, _height: u32) {}

    /// Exposes the concrete [`Renderer`] when this host is the real GPU renderer,
    /// `None` for the GPU-free test host.
    ///
    /// The host (`saffron-host`) reaches it to drive `render_scene` /
    /// `submit_overlay` / `set_viewport_size` and to build the per-frame control +
    /// scene-render seams, which live above this crate in the DAG (so they cannot
    /// be folded into [`FrameHost`]). The provided default is `None`, so a
    /// non-renderer host (the unit-test stubs) opts out for free.
    fn renderer_mut(&mut self) -> Option<&mut Renderer> {
        None
    }

    /// Records this frame's wall-clock delta (seconds) so the host's `render-stats` reports
    /// live frame timing + fps. The default is a no-op for the GPU-free test host; the real
    /// [`Renderer`] folds it into its smoothed frame time.
    fn record_frame_timing(&mut self, _dt_seconds: f32) {}

    /// Finalizes this frame's telemetry once the frame is rendered: folds the CPU busy/wait
    /// split (seconds; the loop's update+render window minus the GPU fence-wait) into the
    /// smoothed `cpuFrameMs`/`cpuWaitMs`, pushes the raw frame into the history ring, runs the
    /// perf-alarm detectors, and advances the profiler-capture state machine. `dt_seconds` is
    /// the wall-clock delta since the prior frame (drives the alarm EMA). The default is a
    /// no-op for the GPU-free test host; the real [`Renderer`] folds it all in.
    fn finalize_frame_telemetry(
        &mut self,
        _busy_seconds: f32,
        _wait_seconds: f32,
        _dt_seconds: f32,
    ) {
    }
}

impl FrameHost for Renderer {
    fn begin_frame(&mut self) -> Result<bool> {
        // Two modes share one loop. The standalone windowed host acquires the swapchain image
        // here, lets the host render the scene offscreen in `on_ui`, and blits that offscreen
        // onto the acquired image at `end_frame`. The editor / headless host has no swapchain —
        // it renders offscreen and publishes the read-back to shared memory — so `begin_frame`
        // only waits the slot fence and signals the loop to run its hooks.
        if self.swapchain().is_some() {
            // Wait the slot fence + acquire the swapchain image; the offscreen render in the
            // host's `on_ui` then records + submits without re-waiting, and `end_frame` blits +
            // presents.
            Ok(self.begin_present_frame()?)
        } else {
            // Wait the frame slot's fence now, before the host's `on_render`/`on_ui` hooks
            // reset any per-frame GPU state (the skinning descriptor pool), so the slot is
            // idle first. `render_scene_offscreen` then records + submits without re-waiting.
            self.begin_offscreen_frame()?;
            Ok(true)
        }
    }

    fn begin_frame_graph(&mut self, _graph: &mut RenderGraph) {}

    fn end_frame(&mut self, _graph: RenderGraph) -> Result<()> {
        // The windowed standalone host blits the offscreen the host rendered in `on_ui` onto
        // the swapchain image acquired in `begin_frame`, then presents. The editor / headless
        // host published its frame to shared memory in `on_ui` and has no swapchain to present.
        if self.swapchain().is_some() {
            self.present_active_view_to_swapchain()?;
        }
        Ok(())
    }

    fn viewport_size(&self) -> (u32, u32) {
        // The windowed host's viewport is the swapchain extent; the editor/headless host's
        // is the active offscreen view (the editor owns the render size over the control
        // plane). Either way a non-zero size keeps the loop's minimized guard happy.
        (self.viewport_width(), self.viewport_height())
    }

    fn wait_gpu_idle(&self) -> Result<()> {
        Ok(self.device().wait_idle()?)
    }

    fn resized(&mut self, width: u32, height: u32) {
        // Only the windowed standalone host has a swapchain to rebuild; a zero extent
        // is a minimize, which the next frame's `viewport_size` guard skips anyway.
        if self.swapchain().is_none() || width == 0 || height == 0 {
            return;
        }
        if let Err(err) = self.recreate_swapchain(width, height) {
            tracing::error!("swapchain recreate failed: {err}");
        }
        // The present blit's source is the active offscreen view; track the window so
        // the presented image is rendered at native resolution, not scaled.
        let view = self.active_view_id();
        if let Err(err) = self.set_viewport_desired_size(view, width, height) {
            tracing::error!("viewport resize failed: {err}");
        }
    }

    fn renderer_mut(&mut self) -> Option<&mut Renderer> {
        Some(self)
    }

    fn record_frame_timing(&mut self, dt_seconds: f32) {
        self.observe_frame_delta(dt_seconds);
    }

    fn finalize_frame_telemetry(&mut self, busy_seconds: f32, wait_seconds: f32, dt_seconds: f32) {
        self.finalize_frame_telemetry(busy_seconds * 1000.0, wait_seconds * 1000.0, dt_seconds);
    }
}

/// The running application state the loop and the [`Layer`] hooks share.
///
/// Field order encodes teardown: [`Self::frame_host`] (the renderer) drops before
/// [`Self::window`]. The layer vec rides on `App` but the loop `mem::take`s it for
/// the duration of each hook pass, so a hook borrows `&mut App` (window + frame
/// host) without aliasing the list being iterated.
pub struct App {
    /// The per-frame GPU host (the renderer), behind a trait so the loop is
    /// GPU-free-testable.
    pub frame_host: Box<dyn FrameHost>,
    /// The OS window, present only in the standalone windowed mode. Editor /
    /// headless mode runs windowless (`None`).
    pub window: Option<Window>,
    /// The loop's run latch; a layer or signal handler sets it `false` to exit.
    pub running: bool,
    /// The attached layers. `attach_layer` pushes here; the loop `mem::take`s the
    /// vec out for each hook pass and restores it, so it is empty *during* a hook.
    layers: Vec<Box<dyn Layer>>,
}

impl App {
    /// Builds an app over a frame host and an optional window, with no layers
    /// attached yet (`on_create` attaches them).
    fn new(frame_host: Box<dyn FrameHost>, window: Option<Window>) -> Self {
        Self {
            frame_host,
            window,
            running: false,
            layers: Vec::new(),
        }
    }
}

/// A set of lifecycle hooks — the runtime "interface" a client implements as a
/// trait with provided (default-empty) methods.
///
/// A client implements only the hooks it needs; the empties cost nothing. Stored
/// as `Box<dyn Layer>` because the layer set is open and client-extensible (the
/// host is itself one [`Layer`]). Each hook takes `&mut App` rather than capturing
/// it, so a layer never aliases the app it runs inside.
pub trait Layer {
    /// The layer's name, for logs. Defaults to `"Layer"`.
    fn name(&self) -> &str {
        "Layer"
    }

    /// Runs once after the window + renderer exist and the layer is attached.
    fn on_attach(&mut self, _app: &mut App) {}

    /// Runs once per frame before rendering, with the frame delta.
    fn on_update(&mut self, _app: &mut App, _dt: TimeSpan) {}

    /// Submits GPU work for the frame (recorded into the current frame's command
    /// buffer via the renderer's submit seam).
    fn on_render(&mut self, _app: &mut App) {}

    /// Builds UI / overlay geometry for the frame.
    fn on_ui(&mut self, _app: &mut App) {}

    /// Adds passes to the frame's render graph (e.g. post-process).
    fn on_render_graph(&mut self, _app: &mut App, _graph: &mut RenderGraph) {}

    /// Runs once during teardown, before the renderer is dropped.
    fn on_detach(&mut self, _app: &mut App) {}
}

/// Client-provided configuration handed to [`run`].
///
/// `on_create` runs once the window + renderer exist (attach layers, wire signals
/// there); `on_exit` runs during teardown after `wait_gpu_idle`. The closures are
/// boxed so the config is a plain owned value.
pub struct AppConfig {
    /// The window parameters for the standalone windowed mode (ignored in
    /// headless editor mode).
    pub window: WindowConfig,
    /// Runs once after bring-up; the host attaches its [`Layer`] and wires signals
    /// here.
    pub on_create: Box<dyn FnOnce(&mut App)>,
    /// Runs once during teardown, after `wait_gpu_idle`.
    pub on_exit: Box<dyn FnOnce(&mut App)>,
}

impl Default for AppConfig {
    fn default() -> Self {
        Self {
            window: WindowConfig::default(),
            on_create: Box::new(|_| {}),
            on_exit: Box::new(|_| {}),
        }
    }
}

/// Whether the host runs windowed (standalone) or headless (the editor host).
///
/// Decided from `SAFFRON_EDITOR_NATIVE_VIEWPORT`: set → headless (no window, a
/// no-surface offscreen device — [`SurfaceSource::Offscreen`] — the frame path is
/// shm-publish). Unset → windowed standalone present-only host. This is a [`run`]-time
/// branch on the env var, not two `run` functions.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum HostMode {
    /// A winit window + a surface-bound renderer (the standalone present-only
    /// host).
    Windowed,
    /// No window + a no-surface offscreen device (the editor native-viewport host).
    Headless,
}

impl HostMode {
    /// Reads the mode from `SAFFRON_EDITOR_NATIVE_VIEWPORT` (the env var the
    /// editor sets when it spawns the host as its viewport producer).
    #[must_use]
    pub fn from_env() -> Self {
        Self::from_present(std::env::var_os("SAFFRON_EDITOR_NATIVE_VIEWPORT").is_some())
    }

    /// Maps the presence of `SAFFRON_EDITOR_NATIVE_VIEWPORT` to a mode: present
    /// (any value, including empty) → headless, absent → windowed. Split out pure
    /// so it is testable without env mutation.
    #[must_use]
    fn from_present(present: bool) -> Self {
        if present {
            Self::Headless
        } else {
            Self::Windowed
        }
    }
}

/// Owns the main loop. Returns a process exit code (`0` on a clean exit, `1` on a
/// bring-up failure).
///
/// The mode (windowed vs headless) is read from the environment. A creation
/// failure logs via `saffron-core` and returns `1`; everything else runs the loop
/// to completion and returns `0`.
#[must_use]
pub fn run(config: AppConfig) -> i32 {
    match run_inner(config, HostMode::from_env()) {
        Ok(()) => 0,
        Err(err) => {
            tracing::error!("{err}");
            1
        }
    }
}

/// Brings up the host for `mode`, runs the loop, tears down. Split from [`run`] so
/// the bring-up failures stay typed (`?`) and [`run`] only collapses them to an
/// exit code.
///
/// Headless mode (the editor host) builds a no-surface offscreen device
/// ([`SurfaceSource::Offscreen`]) with no window and runs the plain [`drive`] loop.
/// Windowed mode (the standalone present-only host) needs a winit window, which winit
/// 0.30 only creates from inside an active event loop, so it hands off to
/// [`run_windowed`] — the renderer is then surface-bound and presents through a real
/// swapchain on the same feature-selected GPU. Both modes share the per-frame body and
/// the `wait_gpu_idle`-before-teardown ordering.
fn run_inner(config: AppConfig, mode: HostMode) -> Result<()> {
    match mode {
        HostMode::Headless => {
            let renderer = Renderer::new(
                &SurfaceSource::Offscreen,
                config.window.width,
                config.window.height,
            )?;
            let app = App::new(Box::new(renderer), None);
            drive(app, config, LoopLimits::from_env());
            Ok(())
        }
        HostMode::Windowed => run_windowed(config, LoopLimits::from_env()),
    }
}

/// The loop's frame-limit and pacing knobs, read once from the environment.
///
/// Passed into [`drive`] so the loop body never reads the environment itself,
/// which keeps the loop tests free of process-global env mutation (the crate
/// denies `unsafe`, so `std::env::set_var` is unavailable here).
#[derive(Clone, Copy, Default)]
struct LoopLimits {
    /// `SAFFRON_EXIT_AFTER_FRAMES`: exit after this many frames; `0` = no limit.
    frame_limit: u64,
    /// `SAFFRON_MAX_FPS`: cap the loop rate; `0` = uncapped.
    max_fps: u64,
}

impl LoopLimits {
    /// Reads both knobs from the environment.
    fn from_env() -> Self {
        Self {
            frame_limit: frame_limit_from_env(),
            max_fps: max_fps_from_env(),
        }
    }
}

/// The frame-clock state the per-frame step threads between iterations: the loop's
/// frame count, the previous-frame instant (for `dt`), and the next-frame deadline
/// (for `SAFFRON_MAX_FPS` pacing). Shared by the plain headless loop and the
/// winit-driven windowed loop so both pace and count frames identically.
struct FrameClock {
    frame_count: u64,
    last: Instant,
    next_frame: Instant,
}

impl FrameClock {
    /// Starts the clock at `now`.
    fn new() -> Self {
        let now = Instant::now();
        Self {
            frame_count: 0,
            last: now,
            next_frame: now,
        }
    }
}

/// Runs `on_create` then `on_attach` and latches the loop running. The shared
/// bring-up half both drivers call before their first frame.
fn start(app: &mut App, config: &mut AppConfig) {
    let on_create = std::mem::replace(&mut config.on_create, Box::new(|_| {}));
    on_create(app);
    run_hook(app, |layer, app| layer.on_attach(app));
    app.running = true;
}

/// `wait_gpu_idle`, then `on_detach`, then `on_exit` — the shared teardown half both
/// drivers call once the loop ends. `wait_gpu_idle` runs first so no in-flight
/// command buffer references a resource a handler is about to drop.
fn finish(app: &mut App, config: &mut AppConfig) {
    if let Err(err) = app.frame_host.wait_gpu_idle() {
        tracing::error!("wait_gpu_idle failed: {err}");
    }
    run_hook(app, |layer, app| layer.on_detach(app));
    let on_exit = std::mem::replace(&mut config.on_exit, Box::new(|_| {}));
    on_exit(app);
}

/// Runs one frame: the timing record, the `on_update` pass, the (non-minimized)
/// `begin_frame` + render passes, the telemetry finalize, the frame-limit check, and
/// the FPS pacing. Sets `app.running = false` when the frame limit is hit or a frame
/// fails. Shared by both drivers so the hook order, minimized guard, telemetry
/// split, and pacing are one implementation.
fn step_frame(app: &mut App, limits: LoopLimits, clock: &mut FrameClock) {
    let now = Instant::now();
    let dt = TimeSpan::from_seconds((now - clock.last).as_secs_f32());
    clock.last = now;
    // Feed the frame delta to the GPU host so `render-stats` reports live frame timing.
    // The first frame's delta is the time since loop start, which the EMA seeds on.
    app.frame_host.record_frame_timing(dt.seconds);

    // The CPU busy window opens here (update + render) and closes after `run_frame`; the
    // GPU fence-wait inside `begin_frame` is the wait split. Both feed the renderer's
    // `cpuFrameMs`/`cpuWaitMs` EMA so `render-stats` reports a real CPU-side cost.
    let busy_start = Instant::now();
    run_hook(app, |layer, app| layer.on_update(app, dt));

    let (width, height) = app.frame_host.viewport_size();
    let minimized = width == 0 || height == 0;
    let mut wait_seconds = 0.0;
    if !minimized {
        let before_begin = Instant::now();
        match app.frame_host.begin_frame() {
            Ok(true) => {
                // `begin_frame` blocks on the slot's in-flight fence (the GPU-bound wait).
                wait_seconds = before_begin.elapsed().as_secs_f32();
                run_frame(app);
            }
            Ok(false) => {}
            Err(err) => {
                tracing::error!("begin_frame failed: {err}");
                app.running = false;
            }
        }
    }
    // Busy = the whole update+render span minus the fence-wait; never negative.
    let busy_seconds = (busy_start.elapsed().as_secs_f32() - wait_seconds).max(0.0);
    // Finalize the frame's telemetry (CPU EMA + history ring + alarms + capture advance).
    // Skipped on a minimized frame (no render happened), so the history + capture only see
    // real frames.
    if !minimized {
        app.frame_host
            .finalize_frame_telemetry(busy_seconds, wait_seconds, dt.seconds);
    }

    clock.frame_count += 1;
    if limits.frame_limit != 0 && clock.frame_count >= limits.frame_limit {
        tracing::info!("frame limit reached ({}), exiting", limits.frame_limit);
        app.running = false;
    }

    pace_loop(limits.max_fps, &mut clock.next_frame);
}

/// Runs the loop over an already-built [`App`]: `on_create`, `on_attach`, the
/// per-frame hook passes, then `wait_gpu_idle` before `on_detach` / `on_exit`.
///
/// The headless driver: a plain `while` loop, no window. Drives whatever
/// [`FrameHost`] the [`App`] holds, so the no-op host of the unit tests and the real
/// headless renderer share this body. The windowed standalone host uses
/// [`run_windowed`] instead (winit owns its loop), but both reuse [`start`],
/// [`step_frame`], and [`finish`].
fn drive(mut app: App, mut config: AppConfig, limits: LoopLimits) {
    start(&mut app, &mut config);

    let mut clock = FrameClock::new();
    while app.running {
        step_frame(&mut app, limits, &mut clock);
    }

    finish(&mut app, &mut config);
}

/// The standalone present-only host: a winit window + a surface-bound renderer that
/// presents through a real swapchain on the feature-selected GPU.
///
/// winit 0.30 only creates a window from inside an active event loop and owns the
/// loop itself, so this drives the frame body through an [`ApplicationHandler`]
/// ([`WindowedApp`]) rather than the plain `while` of [`drive`]. The window + the
/// [`SurfaceSource::Window`] renderer are built in `resumed` (the first time the
/// loop is ready), each frame runs in `about_to_wait`, and window events
/// (close / resize) feed the [`Window`]'s typed signals. The same [`start`] /
/// [`step_frame`] / [`finish`] the headless loop uses run here, so the hook order
/// and the `wait_gpu_idle`-before-teardown ordering are identical across modes.
fn run_windowed(config: AppConfig, limits: LoopLimits) -> Result<()> {
    let event_loop = EventLoop::new().map_err(|err| Error::EventLoop(err.to_string()))?;
    event_loop.set_control_flow(ControlFlow::Poll);

    let mut windowed = WindowedApp {
        config: Some(config),
        limits,
        app: None,
        clock: FrameClock::new(),
        bring_up_error: None,
    };
    event_loop
        .run_app(&mut windowed)
        .map_err(|err| Error::EventLoop(err.to_string()))?;

    // A bring-up failure inside `resumed` (window or renderer creation) exits the
    // loop; surface it as the typed error so `run` collapses it to exit code 1.
    if let Some(err) = windowed.bring_up_error {
        return Err(err);
    }
    Ok(())
}

/// The winit [`ApplicationHandler`] that drives the windowed standalone host.
///
/// Holds the config until `resumed` consumes it to build the window + renderer, the
/// running [`App`] once built, the shared [`FrameClock`], and a deferred bring-up
/// error (a window/renderer failure in `resumed` cannot return through the winit
/// callback, so it is stashed and re-raised by [`run_windowed`]).
struct WindowedApp {
    config: Option<AppConfig>,
    limits: LoopLimits,
    app: Option<App>,
    clock: FrameClock,
    bring_up_error: Option<Error>,
}

impl ApplicationHandler for WindowedApp {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        // `resumed` can fire more than once (a real desktop suspend/resume); build the
        // window + renderer only on the first, when the config is still present.
        let Some(mut config) = self.config.take() else {
            return;
        };

        let window = match Window::new(event_loop, &config.window) {
            Ok(window) => window,
            Err(err) => {
                self.bring_up_error = Some(err.into());
                event_loop.exit();
                return;
            }
        };
        let (width, height) = (window.width(), window.height());
        let renderer = match Renderer::new(&SurfaceSource::Window(&window), width, height) {
            Ok(renderer) => renderer,
            Err(err) => {
                self.bring_up_error = Some(err.into());
                event_loop.exit();
                return;
            }
        };

        let mut app = App::new(Box::new(renderer), Some(window));
        start(&mut app, &mut config);
        self.clock = FrameClock::new();
        self.config = Some(config);
        self.app = Some(app);
    }

    fn window_event(&mut self, event_loop: &ActiveEventLoop, _id: WindowId, event: WindowEvent) {
        let Some(app) = self.app.as_mut() else {
            return;
        };
        if let Some(window) = app.window.as_mut() {
            window.dispatch_window_event(&event);
        }
        match event {
            WindowEvent::CloseRequested => {
                app.running = false;
                event_loop.exit();
            }
            // A resize makes the swapchain out of date; rebuild it (and the offscreen
            // the present blits from) at the new surface size before the next frame.
            WindowEvent::Resized(size) => {
                app.frame_host.resized(size.width, size.height);
            }
            _ => {}
        }
    }

    fn about_to_wait(&mut self, event_loop: &ActiveEventLoop) {
        let Some(app) = self.app.as_mut() else {
            return;
        };
        if app.window.as_ref().is_some_and(Window::should_close) {
            app.running = false;
        }
        if !app.running {
            event_loop.exit();
            return;
        }

        step_frame(app, self.limits, &mut self.clock);

        // A frame limit / failure inside `step_frame` clears `running`; exit promptly.
        if app.running {
            if let Some(window) = app.window.as_ref().and_then(Window::winit_window) {
                window.request_redraw();
            }
        } else {
            event_loop.exit();
        }
    }

    fn exiting(&mut self, _event_loop: &ActiveEventLoop) {
        // The loop is ending: run the shared teardown (`wait_gpu_idle` → `on_detach` →
        // `on_exit`) exactly once, before the `App` (renderer) drops.
        if let (Some(mut app), Some(mut config)) = (self.app.take(), self.config.take()) {
            finish(&mut app, &mut config);
        }
    }
}

/// The render/ui/graph hook pass for one accepted frame: `on_render`, `on_ui`,
/// then `begin_frame_graph` + per-layer `on_render_graph`, then `end_frame`.
///
/// The render graph is owned here for the pass and moved into `end_frame`: the loop
/// hands it to each layer, then `end_frame` executes it.
fn run_frame(app: &mut App) {
    run_hook(app, |layer, app| layer.on_render(app));
    run_hook(app, |layer, app| layer.on_ui(app));

    let mut graph = RenderGraph::new();
    app.frame_host.begin_frame_graph(&mut graph);
    run_hook(app, |layer, app| layer.on_render_graph(app, &mut graph));
    if let Err(err) = app.frame_host.end_frame(graph) {
        tracing::error!("end_frame failed: {err}");
        app.running = false;
    }
}

/// Dispatches one hook across every attached layer.
///
/// Moves the layer vec out of `app` for the pass so `f` can borrow `&mut App`
/// (window + frame host + `running`) without aliasing the list, then restores it. A
/// hook that itself attaches a layer (via `attach_layer` on the
/// moved-out-then-restored vec) is picked up next pass, not mid-pass.
fn run_hook(app: &mut App, mut f: impl FnMut(&mut Box<dyn Layer>, &mut App)) {
    let mut layers = std::mem::take(&mut app.layers);
    for layer in &mut layers {
        f(layer, app);
    }
    // A hook may have attached new layers onto the (empty) `app.layers`; keep both
    // — the originals first, then any freshly attached.
    layers.append(&mut app.layers);
    app.layers = layers;
}

/// Sleeps to honor `SAFFRON_MAX_FPS`, catching up without accumulating debt after
/// a slow frame. A zero `max_fps` disables pacing.
fn pace_loop(max_fps: u64, next_frame: &mut Instant) {
    if max_fps == 0 {
        return;
    }
    *next_frame += Duration::from_nanos(1_000_000_000 / max_fps);
    let now = Instant::now();
    if *next_frame < now {
        *next_frame = now;
    } else {
        std::thread::sleep(*next_frame - now);
    }
}

/// Attaches a layer to the app.
///
/// Called from `AppConfig::on_create` (and legitimately from inside a hook). A
/// layer attached during a hook pass first runs its `on_attach`-less self on the
/// *next* pass (the running loop never replays `on_attach` mid-loop), so it only
/// joins the per-frame iteration after the attach phase.
pub fn attach_layer(app: &mut App, layer: Box<dyn Layer>) {
    app.layers.push(layer);
}

/// Parses `SAFFRON_EXIT_AFTER_FRAMES` strictly: a valid `u64` count, else `0`.
///
/// Trailing garbage (`"10x"`) is rejected (logged + ignored as `0`), and an unset
/// var is `0`. `0` means "no frame limit".
#[must_use]
pub fn frame_limit_from_env() -> u64 {
    parse_u64_env("SAFFRON_EXIT_AFTER_FRAMES", false)
}

/// Parses `SAFFRON_MAX_FPS` strictly: a valid non-zero `u64`, else `0` (no cap).
///
/// Trailing garbage and a literal `0` are both rejected (logged + ignored).
#[must_use]
pub fn max_fps_from_env() -> u64 {
    parse_u64_env("SAFFRON_MAX_FPS", true)
}

/// Reads `name` and parses it as a strict `u64`. An unset var returns `0`
/// silently; a present-but-invalid var logs `name` and returns `0`. The parse
/// strictness is the pure [`parse_strict_u64`], split out so it is testable
/// without mutating the process environment (the crate denies `unsafe`, so
/// `std::env::set_var` is unavailable).
fn parse_u64_env(name: &str, reject_zero: bool) -> u64 {
    let Some(raw) = std::env::var_os(name) else {
        return 0;
    };
    let text = raw.to_string_lossy();
    match parse_strict_u64(&text, reject_zero) {
        Some(value) => value,
        None => {
            tracing::error!("invalid {name}='{text}', ignoring");
            0
        }
    }
}

/// Parses `text` as a strict `u64`: the whole string must be a base-10 `u64` (no
/// trailing garbage, no sign, no whitespace). Returns `None` on any rejection.
/// `reject_zero` additionally rejects a parsed `0` (the `SAFFRON_MAX_FPS` `== 0`
/// ignore rule); for `SAFFRON_EXIT_AFTER_FRAMES`, `0` is a valid value meaning "no
/// limit".
fn parse_strict_u64(text: &str, reject_zero: bool) -> Option<u64> {
    match text.parse::<u64>() {
        Ok(value) if !(reject_zero && value == 0) => Some(value),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::{Cell, RefCell};
    use std::rc::Rc;

    /// A `FrameHost` that does no GPU work — drives the loop on no device.
    struct NoopFrameHost;

    impl FrameHost for NoopFrameHost {
        fn begin_frame(&mut self) -> Result<bool> {
            Ok(true)
        }
        fn begin_frame_graph(&mut self, _graph: &mut RenderGraph) {}
        fn end_frame(&mut self, _graph: RenderGraph) -> Result<()> {
            Ok(())
        }
        fn viewport_size(&self) -> (u32, u32) {
            (1, 1)
        }
        fn wait_gpu_idle(&self) -> Result<()> {
            Ok(())
        }
    }

    /// Builds an `App` over a no-op host with no window (the headless test spine).
    fn headless_app() -> App {
        App::new(Box::new(NoopFrameHost), None)
    }

    /// A config that attaches `layer` in `on_create` and runs `on_exit` after
    /// teardown.
    fn config_with(layer: Box<dyn Layer>, on_exit: Box<dyn FnOnce(&mut App)>) -> AppConfig {
        let layer = RefCell::new(Some(layer));
        AppConfig {
            window: WindowConfig::default(),
            on_create: Box::new(move |app| {
                if let Some(layer) = layer.borrow_mut().take() {
                    attach_layer(app, layer);
                }
            }),
            on_exit,
        }
    }

    /// The frame limit cannot be reached via env in this `unsafe`-free crate, so
    /// the loop tests pass `LoopLimits` explicitly.
    fn limited(frame_limit: u64) -> LoopLimits {
        LoopLimits {
            frame_limit,
            max_fps: 0,
        }
    }

    #[test]
    fn empty_layer_defaults_are_noops() {
        struct Empty;
        impl Layer for Empty {}

        let mut layer = Empty;
        let mut app = headless_app();
        assert_eq!(layer.name(), "Layer");
        layer.on_attach(&mut app);
        layer.on_update(&mut app, TimeSpan::from_seconds(0.016));
        layer.on_render(&mut app);
        layer.on_ui(&mut app);
        let mut graph = RenderGraph::new();
        layer.on_render_graph(&mut app, &mut graph);
        layer.on_detach(&mut app);
    }

    #[test]
    fn frame_limit_parses_strictly() {
        // `SAFFRON_EXIT_AFTER_FRAMES`: `0` is a valid value (no limit).
        assert_eq!(
            parse_strict_u64("10", false),
            Some(10),
            "valid count parses"
        );
        assert_eq!(
            parse_strict_u64("0", false),
            Some(0),
            "literal 0 is allowed"
        );
        assert_eq!(
            parse_strict_u64("10x", false),
            None,
            "trailing garbage rejected"
        );
        assert_eq!(parse_strict_u64("", false), None, "empty rejected");
        assert_eq!(parse_strict_u64("-1", false), None, "sign rejected");
        assert_eq!(parse_strict_u64(" 5", false), None, "whitespace rejected");
    }

    #[test]
    fn max_fps_parses_strictly_and_rejects_zero() {
        // `SAFFRON_MAX_FPS`: a literal 0 is rejected (the `== 0` ignore rule).
        assert_eq!(
            parse_strict_u64("500", true),
            Some(500),
            "valid value parses"
        );
        assert_eq!(parse_strict_u64("0", true), None, "literal 0 rejected");
        assert_eq!(
            parse_strict_u64("60fps", true),
            None,
            "trailing garbage rejected"
        );
    }

    #[test]
    fn host_mode_maps_var_presence() {
        assert_eq!(
            HostMode::from_present(false),
            HostMode::Windowed,
            "absent → windowed"
        );
        // Presence (any value, including empty) selects headless.
        assert_eq!(
            HostMode::from_present(true),
            HostMode::Headless,
            "present → headless"
        );
    }

    #[test]
    fn loop_exits_after_frame_limit_with_correct_hook_order() {
        // A shared sink the layer records its hook order into. `Rc<RefCell>`
        // because the loop owns the layer by value, so the test reads its trace
        // out-of-band.
        let order: Rc<RefCell<Vec<&'static str>>> = Rc::new(RefCell::new(Vec::new()));
        let updates = Rc::new(Cell::new(0u32));

        struct ProbeLayer {
            order: Rc<RefCell<Vec<&'static str>>>,
            updates: Rc<Cell<u32>>,
        }
        impl Layer for ProbeLayer {
            fn on_attach(&mut self, _app: &mut App) {
                self.order.borrow_mut().push("attach");
            }
            fn on_update(&mut self, _app: &mut App, _dt: TimeSpan) {
                self.updates.set(self.updates.get() + 1);
                self.order.borrow_mut().push("update");
            }
            fn on_render(&mut self, _app: &mut App) {
                self.order.borrow_mut().push("render");
            }
            fn on_ui(&mut self, _app: &mut App) {
                self.order.borrow_mut().push("ui");
            }
            fn on_render_graph(&mut self, _app: &mut App, _graph: &mut RenderGraph) {
                self.order.borrow_mut().push("render_graph");
            }
            fn on_detach(&mut self, _app: &mut App) {
                self.order.borrow_mut().push("detach");
            }
        }

        let layer = Box::new(ProbeLayer {
            order: Rc::clone(&order),
            updates: Rc::clone(&updates),
        });
        drive(
            headless_app(),
            config_with(layer, Box::new(|_| {})),
            limited(3),
        );

        assert_eq!(
            updates.get(),
            3,
            "on_update fired exactly per the 3-frame limit"
        );
        let order = order.borrow();
        assert_eq!(order.first(), Some(&"attach"), "attach is first");
        assert_eq!(order.last(), Some(&"detach"), "detach is last");
        let frame: Vec<&'static str> = order[1..order.len() - 1].to_vec();
        assert_eq!(
            frame,
            vec![
                "update",
                "render",
                "ui",
                "render_graph", //
                "update",
                "render",
                "ui",
                "render_graph", //
                "update",
                "render",
                "ui",
                "render_graph",
            ],
            "per-frame hook order: update → render → ui → render_graph, three times"
        );
    }

    #[test]
    fn minimized_skips_frame_body_but_still_counts_to_the_limit() {
        struct ZeroSizeHost;
        impl FrameHost for ZeroSizeHost {
            fn begin_frame(&mut self) -> Result<bool> {
                panic!("begin_frame must not run while minimized");
            }
            fn begin_frame_graph(&mut self, _graph: &mut RenderGraph) {}
            fn end_frame(&mut self, _graph: RenderGraph) -> Result<()> {
                Ok(())
            }
            fn viewport_size(&self) -> (u32, u32) {
                (0, 720)
            }
            fn wait_gpu_idle(&self) -> Result<()> {
                Ok(())
            }
        }

        let renders = Rc::new(Cell::new(0u32));
        struct RenderCounter(Rc<Cell<u32>>);
        impl Layer for RenderCounter {
            fn on_render(&mut self, _app: &mut App) {
                self.0.set(self.0.get() + 1);
            }
        }

        let app = App::new(Box::new(ZeroSizeHost), None);
        let layer = Box::new(RenderCounter(Rc::clone(&renders)));
        drive(app, config_with(layer, Box::new(|_| {})), limited(2));

        assert_eq!(
            renders.get(),
            0,
            "a minimized host skips the frame body (no on_render) yet still exits via the frame limit"
        );
    }

    #[test]
    fn on_exit_runs_after_detach() {
        let trace: Rc<RefCell<Vec<&'static str>>> = Rc::new(RefCell::new(Vec::new()));

        struct DetachProbe(Rc<RefCell<Vec<&'static str>>>);
        impl Layer for DetachProbe {
            fn on_detach(&mut self, _app: &mut App) {
                self.0.borrow_mut().push("detach");
            }
        }

        let layer = Box::new(DetachProbe(Rc::clone(&trace)));
        let on_exit: Box<dyn FnOnce(&mut App)> = {
            let trace = Rc::clone(&trace);
            Box::new(move |_| trace.borrow_mut().push("exit"))
        };
        drive(headless_app(), config_with(layer, on_exit), limited(1));

        assert_eq!(
            &*trace.borrow(),
            &["detach", "exit"],
            "on_detach runs before on_exit (after wait_gpu_idle)"
        );
    }

    #[test]
    fn a_layer_attached_during_a_hook_joins_the_next_pass() {
        // The host attaches sub-layers from inside `on_attach`/`on_update`; verify
        // the `mem::take`-and-restore in `run_hook` preserves both the originals
        // and the freshly-attached without dropping either.
        let attached_count = Rc::new(Cell::new(0u32));

        struct Spawner {
            count: Rc<Cell<u32>>,
            done: bool,
        }
        impl Layer for Spawner {
            fn on_update(&mut self, app: &mut App, _dt: TimeSpan) {
                if !self.done {
                    self.done = true;
                    let count = Rc::clone(&self.count);
                    attach_layer(app, Box::new(Spawned(count)));
                }
            }
        }
        struct Spawned(Rc<Cell<u32>>);
        impl Layer for Spawned {
            fn on_update(&mut self, _app: &mut App, _dt: TimeSpan) {
                self.0.set(self.0.get() + 1);
            }
        }

        let layer = Box::new(Spawner {
            count: Rc::clone(&attached_count),
            done: false,
        });
        drive(
            headless_app(),
            config_with(layer, Box::new(|_| {})),
            limited(3),
        );

        // Frame 1: spawner attaches Spawned (which does not run that pass). Frames
        // 2 and 3: Spawned's on_update fires → 2.
        assert_eq!(
            attached_count.get(),
            2,
            "a mid-hook-attached layer joins the next pass, not the current one"
        );
    }

    // The full headless N-frame smoke (drive a real renderer for two frames over
    // `drive` on a present swapchain) needs hardware: the windowed swapchain present
    // path is covered by `tests/swapchain_present.rs` on a real Wayland surface, and
    // the offscreen path (the editor host) creates no swapchain at all. The renderer
    // crate's own offscreen tests validate the device / allocator / sync2 path via an
    // offscreen clear+readback. The loop's hook order, frame-limit exit, minimized
    // skip, and teardown ordering are fully proven above on the GPU-free `FrameHost`
    // stub.
}
