// Presents the engine's shared-memory frames on a raw Wayland subsurface placed BELOW
// the GTK window's surface. The compositor blends the (transparent) GTK/WebKit UI surface
// over it and presents the viewport at the monitor's refresh rate — completely outside
// GTK3's ~60Hz paint loop. Uses GTK's own wl_display connection (system libwayland
// backend) with a private event queue, so it coexists with GDK's dispatching.

use std::ffi::{c_void, CString};
use std::os::fd::{AsFd, FromRawFd, OwnedFd};
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use glib::translate::ToGlibPtr;
use gtk::glib;
use gtk::prelude::*;
use webkit2gtk::WebViewExt;
use wayland_backend::client::{Backend, ObjectId};
use wayland_client::protocol::{
    wl_buffer::{self, WlBuffer},
    wl_callback::{self, WlCallback},
    wl_compositor::WlCompositor,
    wl_registry::{self, WlRegistry},
    wl_shm::{self, WlShm},
    wl_shm_pool::WlShmPool,
    wl_subcompositor::WlSubcompositor,
    wl_subsurface::WlSubsurface,
    wl_surface::WlSurface,
};
use wayland_client::{Connection, Dispatch, Proxy, QueueHandle, WEnum};
use wayland_protocols::wp::presentation_time::client::{
    wp_presentation::{self, WpPresentation},
    wp_presentation_feedback::{self, Kind as FeedbackKind, WpPresentationFeedback},
};
use wayland_protocols::wp::viewporter::client::{wp_viewport::WpViewport, wp_viewporter::WpViewporter};

const SHM_MAGIC: u32 = 0x5346_5632; // "SFV2"
const SHM_HEADER_BYTES: usize = 32;

unsafe extern "C" {
    fn gdk_wayland_display_get_wl_display(display: *mut c_void) -> *mut c_void;
    fn gdk_wayland_window_get_wl_surface(window: *mut c_void) -> *mut c_void;
}

/// The two views the editor presents, each glued to its own pane (one shm segment +
/// one subsurface apiece). The wire tokens MUST match the engine's `viewIdFromWire`.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum View {
    Scene,
    AssetPreview,
}

impl View {
    /// The control-plane / shm wire token. Exactly "scene" / "assetPreview" end-to-end.
    pub fn wire(self) -> &'static str {
        match self {
            View::Scene => "scene",
            View::AssetPreview => "assetPreview",
        }
    }

    pub fn from_wire(wire: &str) -> Option<View> {
        match wire {
            "scene" => Some(View::Scene),
            "assetPreview" => Some(View::AssetPreview),
            _ => None,
        }
    }

    fn index(self) -> usize {
        match self {
            View::Scene => 0,
            View::AssetPreview => 1,
        }
    }
}

/// Per-view viewport state the presenter applies, fed from two sides: the webview reports
/// the pane's logical CSS rect (a Tauri command writes `pos`/`size`), and the GTK side
/// tracks the webview widget's origin within the toplevel surface (`offset`, CSD-aware).
/// `parked` detaches this view's subsurface (its tab is inactive, or a modal owns the
/// region) — the inactive pane is parked while the other view renders.
#[derive(Default)]
pub struct ViewportShared {
    pos: AtomicU64,    // packed logical (x << 32 | y), relative to the webview
    size: AtomicU64,   // packed logical (w << 32 | h)
    offset: AtomicU64, // packed logical origin of the webview within the toplevel surface
    window: AtomicU64, // packed logical toplevel size (the backdrop stretches to it)
    parked: AtomicBool,
}

impl ViewportShared {
    pub fn set_bounds(&self, x: i32, y: i32, width: i32, height: i32) {
        self.pos.store(pack_pair(x, y), Ordering::Relaxed);
        self.size.store(pack_pair(width, height), Ordering::Relaxed);
    }

    pub fn set_parked(&self, parked: bool) {
        self.parked.store(parked, Ordering::Relaxed);
    }
}

/// The two per-view shared handles, indexed by `View`. Both panes get their own state so a
/// tab switch parks/unparks rather than re-binding a shared surface.
#[derive(Default)]
pub struct Viewports {
    views: [Arc<ViewportShared>; 2],
}

impl Viewports {
    pub fn view(&self, view: View) -> &Arc<ViewportShared> {
        &self.views[view.index()]
    }
}

fn pack_pair(a: i32, b: i32) -> u64 {
    ((a.max(0) as u64) << 32) | (b.max(0) as u64)
}

fn unpack_pair(packed: u64) -> (i32, i32) {
    ((packed >> 32) as i32, (packed & 0xffff_ffff) as i32)
}

/// Ground truth for "is the viewport really updating": frame callbacks only pace commits,
/// while wp_presentation says what the compositor DID with each one — displayed (presented,
/// with the vblank seq it hit) or superseded by a later commit (discarded).
#[derive(Default)]
struct PresentationStats {
    presented: u32,
    discarded: u32,
    refresh_ns: u32,
    flags: u32,
    last_seq: Option<u64>,
    seq_delta_sum: u64,
    seq_delta_count: u32,
}

impl PresentationStats {
    fn flags_label(&self) -> String {
        let mut names = Vec::new();
        for (bit, name) in [
            (FeedbackKind::Vsync, "vsync"),
            (FeedbackKind::HwClock, "hw-clock"),
            (FeedbackKind::HwCompletion, "hw-completion"),
            (FeedbackKind::ZeroCopy, "zero-copy"),
        ] {
            if self.flags & bit.bits() != 0 {
                names.push(name);
            }
        }
        if names.is_empty() { "none".to_string() } else { names.join("+") }
    }

    fn reset_window(&mut self) {
        self.presented = 0;
        self.discarded = 0;
        self.flags = 0;
        self.seq_delta_sum = 0;
        self.seq_delta_count = 0;
        // last_seq survives so the first delta of the next window stays meaningful.
    }
}

#[derive(Default)]
struct State {
    globals: Vec<(u32, String, u32)>,
    stats: PresentationStats,
    // Per-view frame-callback pending flags, indexed by View. A surface's frame callback
    // clears its own slot, so the two panes pace independently on the compositor's refresh.
    frame_pending: [bool; 2],
}

impl Dispatch<WlRegistry, ()> for State {
    fn event(
        state: &mut Self, _: &WlRegistry, event: wl_registry::Event, _: &(), _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wl_registry::Event::Global { name, interface, version } = event {
            state.globals.push((name, interface, version));
        }
    }
}

impl Dispatch<WlCallback, usize> for State {
    fn event(
        state: &mut Self, _: &WlCallback, event: wl_callback::Event, view: &usize, _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wl_callback::Event::Done { .. } = event {
            if let Some(slot) = state.frame_pending.get_mut(*view) {
                *slot = false;
            }
        }
    }
}

impl Dispatch<WlBuffer, ()> for State {
    fn event(
        _: &mut Self, _: &WlBuffer, _: wl_buffer::Event, _: &(), _: &Connection, _: &QueueHandle<Self>,
    ) {
        // Release events: the 4-slot ring makes writer/reader collisions unlikely; v1
        // accepts the (cosmetic) risk instead of cross-process backpressure.
    }
}

impl Dispatch<WlShm, ()> for State {
    fn event(_: &mut Self, _: &WlShm, _: wl_shm::Event, _: &(), _: &Connection, _: &QueueHandle<Self>) {}
}

impl Dispatch<WpPresentation, ()> for State {
    fn event(
        _: &mut Self, _: &WpPresentation, event: wp_presentation::Event, _: &(), _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wp_presentation::Event::ClockId { clk_id } = event {
            tracing::info!(target: "viewport", "presentation clock id {clk_id}");
        }
    }
}

impl Dispatch<WpPresentationFeedback, ()> for State {
    fn event(
        state: &mut Self, _: &WpPresentationFeedback, event: wp_presentation_feedback::Event,
        _: &(), _: &Connection, _: &QueueHandle<Self>,
    ) {
        match event {
            wp_presentation_feedback::Event::Presented {
                refresh, seq_hi, seq_lo, flags, ..
            } => {
                let stats = &mut state.stats;
                let seq = ((seq_hi as u64) << 32) | seq_lo as u64;
                stats.presented += 1;
                stats.refresh_ns = refresh;
                if let Some(last) = stats.last_seq {
                    stats.seq_delta_sum += seq.saturating_sub(last);
                    stats.seq_delta_count += 1;
                }
                stats.last_seq = Some(seq);
                if let WEnum::Value(kind) = flags {
                    stats.flags |= kind.bits();
                }
            }
            wp_presentation_feedback::Event::Discarded => state.stats.discarded += 1,
            _ => {}
        }
    }
}

wayland_client::delegate_noop!(State: ignore WlCompositor);
wayland_client::delegate_noop!(State: ignore WlSubcompositor);
wayland_client::delegate_noop!(State: ignore WlSubsurface);
wayland_client::delegate_noop!(State: ignore WlSurface);
wayland_client::delegate_noop!(State: ignore WlShmPool);
wayland_client::delegate_noop!(State: ignore WpViewporter);
wayland_client::delegate_noop!(State: ignore WpViewport);

/// Prepares the GTK window for compositor-side blending (transparent toplevel + webview)
/// and spawns the worker thread that owns the two view subsurfaces + commit loop once the
/// toplevel's wl_surface exists. Must run on the GTK main thread; requires a Wayland session.
/// Each view binds its own shm segment (`scene_shm` / `asset_shm`) and shared state.
pub fn install(
    window: &tauri::WebviewWindow, scene_shm: String, asset_shm: String, viewports: &Viewports,
) -> Result<(), String> {
    let scene_shared = Arc::clone(viewports.view(View::Scene));
    let asset_shared = Arc::clone(viewports.view(View::AssetPreview));
    let is_wayland = gdk::Display::default()
        .map(|display| display.type_().name().starts_with("GdkWayland"))
        .unwrap_or(false);
    if !is_wayland {
        return Err("the viewport presenter requires a Wayland session".to_string());
    }

    let gtk_window = window.gtk_window().map_err(|err| format!("gtk_window: {err}"))?;
    let vbox = window.default_vbox().map_err(|err| format!("default_vbox: {err}"))?;
    let webview = vbox
        .children()
        .into_iter()
        .find(|child| child.type_().name() == "WebKitWebView")
        .ok_or_else(|| "WebKitWebView widget not found in default_vbox".to_string())?;

    // An app-paintable host is what lets the webview be transparent and reveal the
    // subsurface behind it.
    gtk_window.set_app_paintable(true);
    match webview.clone().downcast::<webkit2gtk::WebView>() {
        Ok(view) => view.set_background_color(&gdk::RGBA::new(0.0, 0.0, 0.0, 0.0)),
        Err(_) => tracing::warn!(target: "viewport", "webview is not a webkit2gtk::WebView; transparency may not apply"),
    }

    // The opaque backdrop lives BELOW the toplevel as its own subsurface (see run()) —
    // painting under the webview from GTK does not survive WebKit's GL blit, which
    // replaces rather than blends the pixels beneath its allocation. The toplevel still
    // paints one near-invisible dot so the compositor never culls it as fully
    // transparent (which would starve GTK of frame callbacks and freeze the paint loop
    // the subsurface adoption depends on).
    gtk_window.connect_draw(|_, context| {
        context.set_source_rgba(0.0, 0.0, 0.0, 0.02);
        context.rectangle(0.0, 0.0, 2.0, 2.0);
        let _ = context.fill();
        glib::Propagation::Proceed
    });

    // The webview widget's origin within the toplevel surface: the window allocation
    // origin covers any CSD margins, translate_coordinates the widget's place in it.
    // The DOM rect the webview reports is relative to this origin. Both panes share the
    // same webview origin + toplevel size, so write the offset/window to both views.
    let update_offset = {
        let scene_shared = Arc::clone(&scene_shared);
        let asset_shared = Arc::clone(&asset_shared);
        let gtk_window = gtk_window.clone();
        std::rc::Rc::new(move |webview: &gtk::Widget| {
            let window_alloc = gtk_window.allocation();
            let (mut x, mut y) = (0, 0);
            if let Some((tx, ty)) = webview.translate_coordinates(&gtk_window, 0, 0) {
                x = window_alloc.x() + tx;
                y = window_alloc.y() + ty;
            }
            let offset = pack_pair(x, y);
            let window = pack_pair(
                window_alloc.x() + window_alloc.width(),
                window_alloc.y() + window_alloc.height(),
            );
            for shared in [&scene_shared, &asset_shared] {
                shared.offset.store(offset, Ordering::Relaxed);
                shared.window.store(window, Ordering::Relaxed);
            }
            // Subsurface position is double-buffered on the parent: nudge a parent commit.
            gtk_window.queue_draw();
        })
    };
    update_offset(&webview);
    {
        let update_offset = std::rc::Rc::clone(&update_offset);
        webview.connect_size_allocate(move |widget, _| update_offset(widget));
    }

    glib::timeout_add_local(Duration::from_millis(50), move || {
        let Some(gdk_window) = gtk_window.window() else {
            return glib::ControlFlow::Continue;
        };
        let display = gtk_window.display();
        let display_ptr: *mut c_void = {
            let raw: *mut gdk::ffi::GdkDisplay = display.to_glib_none().0;
            unsafe { gdk_wayland_display_get_wl_display(raw.cast()) }
        };
        let surface_ptr: *mut c_void = {
            let raw: *mut gdk::ffi::GdkWindow = gdk_window.to_glib_none().0;
            unsafe { gdk_wayland_window_get_wl_surface(raw.cast()) }
        };
        if display_ptr.is_null() || surface_ptr.is_null() {
            return glib::ControlFlow::Continue;
        }

        // The compositor culls everything below a surface that advertises itself opaque;
        // make sure the toplevel doesn't (the window is transparent by design).
        gdk_window.set_opaque_region(None);

        let scene_shm = scene_shm.clone();
        let asset_shm = asset_shm.clone();
        let scene_shared = Arc::clone(&scene_shared);
        let asset_shared = Arc::clone(&asset_shared);
        let display_addr = display_ptr as usize;
        let surface_addr = surface_ptr as usize;
        thread::spawn(move || {
            let views = [(View::Scene, scene_shm, scene_shared), (View::AssetPreview, asset_shm, asset_shared)];
            if let Err(err) = run(display_addr, surface_addr, views) {
                tracing::warn!(target: "viewport", "wayland presenter failed: {err}");
            }
        });

        // Subsurface creation/position is double-buffered in the PARENT's state: it only
        // enters the surface tree when the parent commits. A static transparent window may
        // not be committing at all, so force a few redraws while the worker comes up.
        let redraw_window = gtk_window.clone();
        let mut redraws = 0u32;
        glib::timeout_add_local(Duration::from_millis(250), move || {
            redraw_window.queue_draw();
            redraws += 1;
            if redraws >= 20 { glib::ControlFlow::Break } else { glib::ControlFlow::Continue }
        });
        glib::ControlFlow::Break
    });
    Ok(())
}

/// One view's compositor objects + the per-view state the loop carries between ticks. Each
/// view owns a subsurface permanently glued to its pane (set from its `ViewportShared`
/// geometry), its own shm mapping, and its own buffer ring — so a tab switch parks/unparks
/// rather than re-binding a shared surface.
struct ViewSurface {
    view: View,
    shared: Arc<ViewportShared>,
    surface: WlSurface,
    subsurface: WlSubsurface,
    viewport: WpViewport,
    cname: CString,
    pool_fd: OwnedFd,
    base: *const u8,
    total: usize,
    seg_ino: u64,
    header: *const u32,
    pool: WlShmPool,
    buffers: Vec<WlBuffer>,
    buffer_dims: (u32, u32),
    last_seq: u32,
    applied_size: u64,
    applied_pos: u64,
    buffer_attached: bool,
    parked: bool,
    first_commit: bool,
    commits: u32,
    last_segment_check: std::time::Instant,
    frame_sent_at: std::time::Instant,
    last_commit_at: std::time::Instant,
}

fn run(
    display_addr: usize, parent_addr: usize, views: [(View, String, Arc<ViewportShared>); 2],
) -> Result<(), String> {
    let stats_enabled = std::env::var_os("SAFFRON_VIEWPORT_STATS").is_some();
    let backend = unsafe { Backend::from_foreign_display(display_addr as *mut _) };
    let conn = Connection::from_backend(backend);
    let mut queue = conn.new_event_queue::<State>();
    let qh = queue.handle();

    let registry = conn.display().get_registry(&qh, ());
    let mut state = State::default();
    queue.roundtrip(&mut state).map_err(|err| format!("registry roundtrip: {err}"))?;

    let find = |wanted: &str| -> Option<(u32, u32)> {
        state.globals.iter().find(|(_, name, _)| name == wanted).map(|(id, _, ver)| (*id, *ver))
    };
    let (compositor_id, compositor_ver) = find("wl_compositor").ok_or("no wl_compositor global")?;
    let (subcompositor_id, _) = find("wl_subcompositor").ok_or("no wl_subcompositor global")?;
    let (shm_id, _) = find("wl_shm").ok_or("no wl_shm global")?;
    let (viewporter_id, _) = find("wp_viewporter").ok_or("no wp_viewporter global")?;

    let compositor: WlCompositor = registry.bind(compositor_id, compositor_ver.min(4), &qh, ());
    let subcompositor: WlSubcompositor = registry.bind(subcompositor_id, 1, &qh, ());
    let wl_shm: WlShm = registry.bind(shm_id, 1, &qh, ());
    let viewporter: WpViewporter = registry.bind(viewporter_id, 1, &qh, ());
    let presentation: Option<WpPresentation> =
        find("wp_presentation").map(|(id, ver)| registry.bind(id, ver.min(1), &qh, ()));
    if presentation.is_none() {
        tracing::warn!(target: "viewport", "wp_presentation not advertised; presentation stats unavailable");
    }

    let parent: WlSurface = unsafe {
        let id = ObjectId::from_ptr(WlSurface::interface(), parent_addr as *mut _)
            .map_err(|err| format!("foreign wl_surface: {err}"))?;
        WlSurface::from_id(&conn, id).map_err(|err| format!("wl_surface from_id: {err}"))?
    };

    // One view subsurface per pane. Both go below the toplevel (the transparent webview
    // composites over them); the asset surface sits just below the scene surface so the
    // backdrop (placed below the asset surface next) ends up under BOTH viewport surfaces.
    let mut surfaces: Vec<ViewSurface> = Vec::with_capacity(views.len());
    let mut below: Option<WlSurface> = None;
    for (view, shm_name, shared) in views {
        let surface = compositor.create_surface(&qh, ());
        let subsurface = subcompositor.get_subsurface(&surface, &parent, &qh, ());
        subsurface.set_desync();
        match &below {
            Some(prev) => subsurface.place_below(prev),
            None => subsurface.place_below(&parent),
        }
        subsurface.set_position(0, 0);
        let viewport = viewporter.get_viewport(&surface, &qh, ());

        // Map this view's segment (retry until the engine creates it) + keep an fd for the pool.
        let cname = CString::new(shm_name.clone()).map_err(|_| "bad shm name".to_string())?;
        let mut attempts = 0u32;
        let (pool_fd, base, total, seg_ino) = loop {
            if let Some(mapping) = open_shm(&cname) {
                break mapping;
            }
            attempts += 1;
            if attempts % 50 == 0 {
                let errno = std::io::Error::last_os_error();
                tracing::info!(target: "viewport", "still waiting for shm '{shm_name}': {errno}");
            }
            thread::sleep(Duration::from_millis(100));
        };
        let pool: WlShmPool = wl_shm.create_pool(pool_fd.as_fd(), total as i32, &qh, ());
        tracing::info!(target: "viewport", "'{}' subsurface up ({total} byte pool)", view.wire());

        below = Some(surface.clone());
        let now = std::time::Instant::now();
        surfaces.push(ViewSurface {
            view,
            shared,
            surface,
            subsurface,
            viewport,
            cname,
            base,
            total,
            seg_ino,
            header: base as *const u32,
            pool_fd,
            pool,
            buffers: Vec::new(),
            buffer_dims: (0, 0),
            last_seq: 0,
            applied_size: 0,
            applied_pos: u64::MAX, // (0,0) is a legitimate position, so start impossible
            buffer_attached: false,
            parked: false,
            first_commit: true,
            commits: 0,
            last_segment_check: now,
            frame_sent_at: now,
            last_commit_at: now - Duration::from_secs(1),
        });
    }

    // Opaque theme backdrop below BOTH viewport subsurfaces: one dark pixel stretched to
    // the whole window. Every translucent or unpainted pixel of the (transparent) page —
    // including a parked view's hole — resolves against it instead of the desktop, so
    // hairline panel seams and the repaint-lag strip during interactive resizes render as
    // they would in an opaque app. Painting this from GTK does not work: WebKit's GL blit
    // replaces the pixels beneath its allocation rather than blending over them.
    let backdrop_surface = compositor.create_surface(&qh, ());
    let backdrop_sub = subcompositor.get_subsurface(&backdrop_surface, &parent, &qh, ());
    backdrop_sub.set_desync();
    // `below` is the last (lowest) viewport surface; placing the backdrop below it lands it
    // under every viewport surface in the parent's stack.
    if let Some(lowest) = &below {
        backdrop_sub.place_below(lowest);
    } else {
        backdrop_sub.place_below(&parent);
    }
    backdrop_sub.set_position(0, 0);
    let backdrop_viewport = viewporter.get_viewport(&backdrop_surface, &qh, ());
    let _backdrop_fd;
    let _backdrop_buffer;
    match backdrop_pixel_fd() {
        Some(fd) => {
            let pool: WlShmPool = wl_shm.create_pool(fd.as_fd(), 4, &qh, ());
            let buffer = pool.create_buffer(0, 1, 1, 4, wl_shm::Format::Argb8888, &qh, ());
            backdrop_surface.attach(Some(&buffer), 0, 0);
            backdrop_surface.damage(0, 0, i32::MAX, i32::MAX);
            _backdrop_fd = Some(fd);
            _backdrop_buffer = Some(buffer);
        }
        None => {
            tracing::warn!(target: "viewport", "backdrop memfd failed; seams may show the desktop");
            _backdrop_fd = None;
            _backdrop_buffer = None;
        }
    }
    let _ = conn.flush();

    let mut applied_window = 0u64;
    let mut last_report = std::time::Instant::now();

    loop {
        let _ = queue.dispatch_pending(&mut state);

        // Stretch the backdrop to the toplevel size (applies the initial attach too). Both
        // views carry the same window size, so either view's value drives it.
        let packed_window = surfaces[0].shared.window.load(Ordering::Relaxed);
        if packed_window != applied_window && packed_window != 0 {
            let (w, h) = unpack_pair(packed_window);
            if w > 0 && h > 0 {
                backdrop_viewport.set_destination(w, h);
                backdrop_surface.commit();
                let _ = conn.flush();
                applied_window = packed_window;
            }
        }

        let mut committed = false;
        let mut all_parked = true;
        for vs in &mut surfaces {
            if step_view(vs, &mut state, &conn, &mut queue, &qh, &wl_shm, &presentation) {
                committed = true;
            }
            if !vs.parked {
                all_parked = false;
            }
        }

        if last_report.elapsed() >= Duration::from_secs(1) {
            if stats_enabled {
                let stats = &state.stats;
                let mean_delta = if stats.seq_delta_count > 0 {
                    stats.seq_delta_sum as f64 / stats.seq_delta_count as f64
                } else {
                    0.0
                };
                let refresh_hz = if stats.refresh_ns > 0 {
                    1.0e9 / stats.refresh_ns as f64
                } else {
                    0.0
                };
                let commits: u32 = surfaces.iter().map(|vs| vs.commits).sum();
                tracing::info!(
                    target: "viewport",
                    "commit {commits}/s · presented {}/s discarded {}/s · \
                     vblank Δ mean {mean_delta:.2} · refresh {refresh_hz:.0} Hz · flags {}",
                    stats.presented,
                    stats.discarded,
                    stats.flags_label(),
                );
            }
            state.stats.reset_window();
            for vs in &mut surfaces {
                vs.commits = 0;
            }
            last_report = std::time::Instant::now();
        }

        // An active view paces itself on the compositor frame callback (no sleep when one
        // committed). When idle, a short rest if a view is still live (waiting on the next
        // frame), a longer one when both panes are parked.
        if !committed {
            thread::sleep(if all_parked { Duration::from_millis(20) } else { Duration::from_micros(500) });
        }
    }
}

/// Advances one view by one tick: remaps a recreated segment, applies geometry, parks/unparks
/// per its shared flag, and attaches+commits the next seqlock frame if one arrived and the
/// view is unthrottled. Returns true when this view committed a new frame this tick.
fn step_view(
    vs: &mut ViewSurface, state: &mut State, conn: &Connection, queue: &mut wayland_client::EventQueue<State>,
    qh: &QueueHandle<State>, wl_shm: &WlShm, presentation: &Option<WpPresentation>,
) -> bool {
    let pending_slot = vs.view.index();

    // The engine recreates the segment when a frame outgrows the slot capacity (and a
    // restarted engine makes a fresh one): same name, new inode. Remap and rebuild the pool
    // + buffers, or this view keeps reading the orphaned old mapping forever.
    if vs.last_segment_check.elapsed() >= Duration::from_millis(250) {
        vs.last_segment_check = std::time::Instant::now();
        if let Some((ino, size)) = stat_shm(&vs.cname) {
            if ino != vs.seg_ino || size != vs.total {
                if let Some(mapping) = open_shm(&vs.cname) {
                    for buffer in vs.buffers.drain(..) {
                        buffer.destroy();
                    }
                    vs.pool.destroy();
                    unsafe { libc::munmap(vs.base as *mut _, vs.total) };
                    (vs.pool_fd, vs.base, vs.total, vs.seg_ino) = mapping;
                    vs.header = vs.base as *const u32;
                    vs.pool = wl_shm.create_pool(vs.pool_fd.as_fd(), vs.total as i32, qh, ());
                    vs.buffer_dims = (0, 0);
                    vs.last_seq = 0;
                    vs.buffer_attached = false;
                    tracing::info!(target: "viewport", "'{}' shm segment remapped ({} byte pool)", vs.view.wire(), vs.total);
                }
            }
        }
    }

    // Parked (this view's tab is inactive, or a modal owns the region): detach the buffer so
    // the subsurface vanishes and the webview DOM (resolving against the backdrop) paints the
    // hole. The ring retains the last frame, so an unpark re-attaches it instantly.
    if vs.shared.parked.load(Ordering::Relaxed) {
        if !vs.parked {
            vs.surface.attach(None, 0, 0);
            vs.surface.commit();
            let _ = conn.flush();
            vs.parked = true;
            vs.buffer_attached = false;
            state.frame_pending[pending_slot] = false;
        }
        return false;
    }
    if vs.parked {
        vs.parked = false;
        vs.last_seq = vs.last_seq.wrapping_sub(1); // force a re-attach showing the retained frame
    }

    let magic = unsafe { ptr::read_volatile(vs.header) };
    let seq = unsafe { ptr::read_volatile(vs.header.add(3)) };

    // Geometry first, decoupled from frame arrival: position/destination changes apply to the
    // already-attached buffer immediately (the old frame stretches to the new rect), so the
    // subsurface stays glued to the pane during a dock drag even while the engine is still
    // rendering at the old size.
    let mut geometry_changed = false;
    let packed = vs.shared.size.load(Ordering::Relaxed);
    if packed != vs.applied_size && packed != 0 {
        let (w, h) = unpack_pair(packed);
        if w > 0 && h > 0 {
            vs.viewport.set_destination(w, h);
            vs.applied_size = packed;
            geometry_changed = true;
        }
    }
    let packed_pos = vs.shared.pos.load(Ordering::Relaxed);
    let packed_offset = vs.shared.offset.load(Ordering::Relaxed);
    let combined = {
        let (x, y) = unpack_pair(packed_pos);
        let (ox, oy) = unpack_pair(packed_offset);
        pack_pair(x + ox, y + oy)
    };
    if combined != vs.applied_pos {
        let (x, y) = unpack_pair(combined);
        // Applied on the parent's next commit; the bounds command queue_draws one.
        vs.subsurface.set_position(x, y);
        vs.applied_pos = combined;
        geometry_changed = true;
    }
    // Commit geometry-only changes when a buffer is attached; otherwise the pending state
    // simply rides along with the next frame commit.
    if geometry_changed && vs.buffer_attached {
        vs.surface.commit();
        let _ = conn.flush();
    }

    // Pace on the compositor's frame callback when it flows (= the monitor's refresh). The
    // callback only flows once the subsurface is actually presented, which needs a PARENT
    // commit to adopt it first — so fall back to bounded self-paced commits when a callback
    // hasn't arrived in 50ms (e.g. before first visibility / when occluded).
    let throttled = if state.frame_pending[pending_slot] {
        if vs.frame_sent_at.elapsed() < Duration::from_millis(50) {
            // Force a read from the compositor: GTK may simply not be reading the socket while
            // idle, leaving our callback undelivered. Roundtrip both flushes and reads
            // (multi-reader safe via libwayland's prepare_read).
            if vs.frame_sent_at.elapsed() > Duration::from_millis(2) {
                let _ = queue.roundtrip(state);
            }
            state.frame_pending[pending_slot]
        } else {
            state.frame_pending[pending_slot] = false; // callback lost/withheld; self-pace at >=8ms
            vs.last_commit_at.elapsed() < Duration::from_millis(8)
        }
    } else {
        false
    };

    if magic != SHM_MAGIC || seq == vs.last_seq || throttled {
        return false;
    }
    let width = unsafe { ptr::read_volatile(vs.header.add(1)) };
    let height = unsafe { ptr::read_volatile(vs.header.add(2)) };
    let slots = unsafe { ptr::read_volatile(vs.header.add(4)) }.max(1);
    let capacity = unsafe { ptr::read_volatile(vs.header.add(5)) } as usize;
    let pixel_bytes = (width as usize) * (height as usize) * 4;
    if pixel_bytes == 0 || capacity == 0 ||
        SHM_HEADER_BYTES + (slots as usize) * capacity > vs.total || pixel_bytes > capacity
    {
        return false;
    }

    if vs.buffer_dims != (width, height) {
        for buffer in vs.buffers.drain(..) {
            buffer.destroy();
        }
        for slot in 0..slots {
            let offset = (SHM_HEADER_BYTES + (slot as usize) * capacity) as i32;
            vs.buffers.push(vs.pool.create_buffer(
                offset,
                width as i32,
                height as i32,
                (width * 4) as i32,
                wl_shm::Format::Xrgb8888,
                qh,
                (),
            ));
        }
        vs.buffer_dims = (width, height);
    }

    let buffer = &vs.buffers[(seq % slots) as usize];
    vs.surface.attach(Some(buffer), 0, 0);
    vs.surface.damage(0, 0, i32::MAX, i32::MAX);

    state.frame_pending[pending_slot] = true;
    vs.frame_sent_at = std::time::Instant::now();
    vs.surface.frame(qh, pending_slot);
    // One feedback per submission: the compositor answers presented (with the vblank it hit)
    // or discarded (a later commit superseded this one before any repaint).
    if let Some(presentation) = presentation {
        presentation.feedback(&vs.surface, qh, ());
    }
    vs.surface.commit();
    let _ = conn.flush();
    vs.last_commit_at = std::time::Instant::now();
    vs.last_seq = seq;
    vs.buffer_attached = true;
    if vs.first_commit {
        tracing::info!(target: "viewport", "'{}' first subsurface commit ({width}x{height} buffer)", vs.view.wire());
        vs.first_commit = false;
    }
    vs.commits += 1;
    true
}

/// One ARGB pixel of the theme background (`--background`, oklch(0.145 0 0) = #0a0a0a)
/// in an anonymous shm fd — the backdrop buffer wp_viewport stretches over the window.
fn backdrop_pixel_fd() -> Option<OwnedFd> {
    unsafe {
        let fd = libc::memfd_create(c"saffron-backdrop".as_ptr(), libc::MFD_CLOEXEC);
        if fd < 0 {
            return None;
        }
        if libc::ftruncate(fd, 4) != 0 {
            libc::close(fd);
            return None;
        }
        let base = libc::mmap(ptr::null_mut(), 4, libc::PROT_WRITE, libc::MAP_SHARED, fd, 0);
        if base == libc::MAP_FAILED {
            libc::close(fd);
            return None;
        }
        // ARGB8888 little-endian byte order: B, G, R, A.
        (base as *mut u8).copy_from([0x0a, 0x0a, 0x0a, 0xff].as_ptr(), 4);
        libc::munmap(base, 4);
        Some(OwnedFd::from_raw_fd(fd))
    }
}

fn open_shm(name: &std::ffi::CStr) -> Option<(OwnedFd, *const u8, usize, u64)> {
    unsafe {
        let fd = libc::shm_open(name.as_ptr(), libc::O_RDWR, 0);
        if fd < 0 {
            return None;
        }
        let mut st: libc::stat = std::mem::zeroed();
        if libc::fstat(fd, &mut st) != 0 || (st.st_size as usize) < SHM_HEADER_BYTES {
            libc::close(fd);
            return None;
        }
        let size = st.st_size as usize;
        let base = libc::mmap(ptr::null_mut(), size, libc::PROT_READ, libc::MAP_SHARED, fd, 0);
        if base == libc::MAP_FAILED {
            libc::close(fd);
            return None;
        }
        Some((OwnedFd::from_raw_fd(fd), base as *const u8, size, st.st_ino as u64))
    }
}

/// Inode + size of the segment currently behind `name` — cheap probe to detect the
/// engine recreating it (bigger frames, engine restart).
fn stat_shm(name: &std::ffi::CStr) -> Option<(u64, usize)> {
    unsafe {
        let fd = libc::shm_open(name.as_ptr(), libc::O_RDONLY, 0);
        if fd < 0 {
            return None;
        }
        let mut st: libc::stat = std::mem::zeroed();
        let ok = libc::fstat(fd, &mut st) == 0;
        libc::close(fd);
        if ok { Some((st.st_ino as u64, st.st_size as usize)) } else { None }
    }
}
