# Phase 1 — windowed present foundation

**Status:** IN PROGRESS

## What was already here (the plan's premise was too pessimistic)

The windowed present path is **not** missing — it is fully wired and covered by tests:

- `engine/crates/app/src/lib.rs` drives the standalone host through `run_windowed` /
  `WindowedApp` (a winit `ApplicationHandler`): `Window::new` → `SurfaceSource::Window` →
  `Renderer::begin_present_frame` (acquire) → `render_scene_offscreen` →
  `present_active_view_to_swapchain` (blit offscreen → swapchain, present). `HostMode::from_env`
  selects windowed when `SAFFRON_EDITOR_NATIVE_VIEWPORT` is unset.
- `engine/crates/rendering/tests/swapchain_present.rs` proves it on a real Wayland surface:
  validation-clean clear+present, a window-capture PNG, and a present-only offscreen→swapchain
  blit asserting a non-blank scene.
- `just run-engine` already runs the host **windowed** by default and can load a project directly
  via `SAFFRON_PROJECT` — so direct, socket-free project loading already exists (de-risks Phase 3).

So the foundation exists. The one genuine gap is resize.

## The gap this phase closes: swapchain recreation on resize

`Swapchain` is built once in `Renderer::new` and never rebuilt. On resize it goes out of date;
`begin_present_frame` returns `Ok(false)` (doc: "a resize the caller should handle by rebuilding")
but nothing rebuilt it — frames were skipped indefinitely, and the offscreen blit source never
tracked the window. Implemented:

- `Renderer::recreate_swapchain(width, height)` (`engine/crates/rendering/src/renderer.rs`):
  guard offscreen/zero-extent → `wait_idle` → drop any stale acquired image → destroy + rebuild
  `Swapchain` as a unit. `PresentSync` is frame-ring-indexed (extent-independent), so it is kept.
- `FrameHost::resized(width, height)` (`engine/crates/app/src/lib.rs`): default no-op (headless
  has no swapchain); the `Renderer` impl calls `recreate_swapchain` then
  `set_viewport_desired_size(active_view, …)` so the present blit renders at native resolution.
- `WindowedApp::window_event` dispatches `WindowEvent::Resized(size)` →
  `app.frame_host.resized(size.width, size.height)` (zero sizes are skipped as minimize).

## Verification note (environment)

The `swapchain_present.rs` tests **deadlock under a nested headless `weston`**: FIFO present blocks
on a frame callback the nested compositor never delivers, so `vkQueuePresentKHR` hangs and the
test's own 30 s timeout (checked in `about_to_wait`) never runs. This is an environment artifact,
not a code regression — the test reached full renderer bring-up + swapchain creation before
stalling in present. The resize change is verified by a clean `cargo build` of `saffron-app` +
`saffron-rendering`; the live present/resize behavior is validated on a real Wayland session
(`just run-engine`, then resize the window) rather than under nested weston.

## Gate

Done: `cargo build -p saffron-app` (pulls rendering) clean; `cargo clippy -p saffron-app
-p saffron-rendering -- -D warnings` clean; `cargo fmt --check` clean — all exit 0.

Remaining (user-side, needs a real Wayland session — nested weston deadlocks on FIFO present):
`just run-engine` shows a window presenting a scene and survives a resize (the `swapchain rebuilt
WxH` log line fires, the scene keeps rendering, no validation errors).
