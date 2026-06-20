# Phase 1 — saffron-app: the run loop, the Layer trait, and the headless/windowed mode split

**Status:** COMPLETED

**Depends on:** 00-foundations:phase-1-workspace-scaffold, 00-foundations:phase-2-core-crate, 00-foundations:phase-5-window-crate (saffron-window), 06-rendering:phase-1-device-swapchain-bringup

## Goal

Port `app.cppm` into the `saffron-app` lib: the `Layer` trait (PP-1 locked struct-of-closures →
trait-with-default-methods), the `App`/`AppConfig` types, and `run(config)` — the poll → update →
begin_frame → render → ui → begin_frame_graph → render_graph → end_frame loop with the
`SAFFRON_EXIT_AFTER_FRAMES` / `SAFFRON_MAX_FPS` env knobs and the `wait_gpu_idle`-before-teardown
ordering. The loop must support both modes: a **windowed** standalone host (winit window + surface-bound
renderer) and a **headless** editor host (no window, feature-selected device), decided from
`SAFFRON_EDITOR_NATIVE_VIEWPORT`. No shm publish, no overlay, no control yet — this phase is the bare
runnable spine that a trivial test `Layer` drives for N frames and exits.

## Why this shape (NO LEGACY)

- **`Layer` = `trait Layer` with provided methods, `Box<dyn Layer>`** (PP-1). The C++ six-`std::function`
  struct becomes a trait whose default methods are empty; a layer implements only what it needs. Chosen
  trait-object (not an enum of layer kinds) because the layer set is open/client-extensible. The
  `std::string name` field becomes `fn name(&self) -> &str { "Layer" }`.
- **The `shared_ptr<HostState>`-by-value capture pattern is gone.** In C++ the closures capture state by
  value because closures cannot otherwise share mutable state across frames; a Rust `Layer` impl *is*
  the state (the host's fields live on its `HostLayer`), so there is no `Arc`/`Rc` here. The hooks take
  `&mut App` instead of capturing `App&`.
- **The borrow conflict is resolved by `mem::take`-ing the layer vec for a hook pass**, not by `RefCell`.
  `run` cannot lend a `&mut Layer` a `&mut App` while `App` owns the `Vec<Box<dyn Layer>>` being
  iterated. The locked resolution: `App { renderer, window, running, .. }` holds no layers; the layers
  live in a `Vec` `run` owns, and each hook pass iterates that vec by index, passing `&mut app`. Because
  the layers are not a field of `app`, no aliasing exists. (Equivalently `std::mem::take(&mut app.layers)`
  then restore — both avoid the alias; the plan picks the layers-outside-App form for clarity.)
- **`Result<Window, String>`/`Result<Renderer, String>` → typed `Error`.** `run` returns `i32` (the
  process exit code) exactly as C++; a creation failure logs via `saffron-core` and returns `1`.
- **Env parsing strictness preserved.** `from_chars` rejecting trailing garbage and `MAX_FPS==0` → ignore
  port to `str::parse::<u64>()` with `Result` + the same reject-and-log-and-ignore behavior. `anyhow` is
  allowed in this crate only if it is a `bin`; `saffron-app` is a `lib`, so it uses its own `thiserror`
  `Error` (foundations rule).
- **No window in editor mode.** Per feasibility 4.5 the editor frame path never presents a swapchain, so
  the headless device path (06-rendering phase-1's by-feature selection) is used and `saffron-window` is
  not instantiated. The mode is a `run`-time branch on the env var, not two `run` functions.

## The standalone windowed driver (WIRED, 2026-06)

Initial bring-up stubbed the windowed path: `bring_up` ignored the mode and always built a
no-surface offscreen (`SurfaceSource::Offscreen`) renderer with no window, and `poll_events` was a no-op —
so `make run-engine` (no `SAFFRON_EDITOR_NATIVE_VIEWPORT`) opened no window and created no swapchain. The
plan always called for a real standalone present-only host (README §3: "neither set → standalone
present-only host with a winit window"); the renderer side (`SurfaceSource::Window` + swapchain present)
was complete and covered by `tests/swapchain_present.rs`, only the run-loop driver was missing. It is now
wired (one path, no legacy):

- `run_inner` branches on `HostMode`. **Headless** builds the no-surface offscreen
  (`SurfaceSource::Offscreen`) renderer with no window and runs the plain `while` `drive` loop. **Windowed**
  hands off to `run_windowed`, which owns a
  winit `EventLoop` and drives an `ApplicationHandler` (`WindowedApp`): it creates the `Window` +
  `SurfaceSource::Window` renderer in `resumed` (winit 0.30 only makes windows from an active loop), runs
  one frame per `about_to_wait`, feeds close/resize through `Window::dispatch_window_event` in
  `window_event`, and runs teardown in `exiting`.
- The per-frame body (`step_frame`) and the bring-up/teardown halves (`start` = `on_create` + `on_attach`;
  `finish` = `wait_gpu_idle` → `on_detach` → `on_exit`) are shared by both drivers, so the hook order, the
  minimized guard, the telemetry split, the FPS pacing, and the `wait_gpu_idle`-before-teardown ordering
  are one implementation regardless of mode. The dead `poll_events` stub is removed; the windowed loop
  pumps winit events through the handler instead.
- The winit machinery (`EventLoop`, `ControlFlow`, `ApplicationHandler`, `WindowId`) is re-exported from
  `saffron-window` (the engine's single winit facade), so `saffron-app` stays winit-free and
  `#![deny(unsafe_code)]`.

Verified on the real machine: `saffron-host` with no `SAFFRON_EDITOR_NATIVE_VIEWPORT` opens a window and
logs `N swapchain images` on the selected GPU (the 3070 Ti with the NVIDIA ICD, llvmpipe without it), runs
its frames, and exits 0 validation-clean.

## Grounding (real files/symbols)

- `engine-old/source/saffron/app/app.cppm`: `run` (the full loop, lines 86-236), `Layer` (six optional
  `std::function`s), `App` (`window`, `renderer`, `layers`, `running`), `AppConfig` (`window`,
  `onCreate`, `onExit`), `attachLayer`, `detail::frameLimitFromEnv` (`SAFFRON_EXIT_AFTER_FRAMES`,
  strict `from_chars`), `detail::maxFpsFromEnv` (`SAFFRON_MAX_FPS`, reject 0), the `waitGpuIdle` call at
  line 209 (before `on_detach`/`on_exit`), the `SAFFRON_CAPTURE` PPM dump at 224.
- `engine-old/source/saffron/window/window.cppm`: `WindowConfig.hidden` (set from
  `SAFFRON_EDITOR_NATIVE_VIEWPORT` by the host) — in the Rust port the editor mode skips the window
  entirely rather than hiding it.
- 06-rendering phase-1 gate: "the device-selection code here is written so [the headless] path is a
  parameter, not a fork" — this phase exercises that parameter.
- `editor/src-tauri/src/lib.rs:331`: the editor sets `SAFFRON_MAX_FPS=500` (the pacing the loop honors).

## Acceptance gate

- Cargo workspace compiles; `cargo build -p saffron-app` and `cargo clippy -p saffron-app` clean;
  `saffron-app` is `#![deny(unsafe_code)]` (no FFI here).
- Unit `#[test]`s:
  - `frame_limit_from_env` parses a valid count, rejects trailing garbage (`"10x"` → ignored/0), and
    treats unset as 0 — matching the C++ `from_chars` strictness.
  - `max_fps_from_env` rejects `0` and garbage, accepts a valid value.
  - A `CountingLayer` (implements only `on_update`, increments a counter) driven by `run` with
    `SAFFRON_EXIT_AFTER_FRAMES=3` and a headless/no-op renderer stub exits after exactly 3 frames and
    the counter reads 3 — proving the hook-dispatch order and the frame-limit exit without a GPU.
  - The `Layer` default methods are all no-ops (a `struct Empty;` impl with zero overrides compiles and
    its hooks do nothing).
- A headless N-frame smoke (gated behind the renderer being available, else skipped+logged): `run` with
  `SAFFRON_EDITOR_NATIVE_VIEWPORT=1` + a real headless renderer + `SAFFRON_EXIT_AFTER_FRAMES=2` boots,
  runs `begin_frame`/`end_frame` twice, calls `wait_gpu_idle` before teardown, and exits 0 with a
  validation-clean log (no window created).
