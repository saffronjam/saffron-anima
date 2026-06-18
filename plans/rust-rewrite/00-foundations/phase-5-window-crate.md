# Phase 5 — `saffron-window`: the winit wrapper behind a typed-signal facade

**Status:** COMPLETED

**Depends on:** 00-foundations:phase-2-core-crate, 00-foundations:phase-3-signal-crate

## Goal

Port `Saffron.Window` into `saffron-window`: the thin window wrapper that owns the OS window and
exposes the engine's input/lifecycle events as the typed `SubscriberList` signals downstream code
subscribes to (`on_resize`, `on_key_pressed`, `on_key_released`, `on_close`, `on_file_dropped`). The
C++ wraps SDL3; the Rust wrap is `winit` 0.30 + `raw-window-handle` 0.6 (dependency-adoption §5). The
crate also exposes the `raw-window-handle` / `raw-display-handle` pair `ash-window` consumes for
surface creation (§2b), and it must support a **windowless** construction: the editor host runs
headless and creates no window at all (§2b, §5), so the signal facade and the public type exist
without an OS window behind them.

This phase ports only the window crate's surface — its typed signals and the handle accessor. The
`winit` `ApplicationHandler` event loop that *drives* `poll → on_update → … → present` is PP-10's
design (`08-host-and-viewport/`); this crate provides the signals that loop publishes into, not the
loop.

## Why this shape (NO LEGACY)

- **`winit` 0.30, not the `sdl3` crate.** The SDL surface the C++ actually used is tiny — instance
  extensions, surface creation, and the five typed event signals — and the editor path is headless,
  so `winit`'s event-loop model is a clean fit and the `sdl3` crate is still pre-stable WIP
  (dependency-adoption §5). The C++ `SDL_INIT_VIDEO` + `SDL_CreateWindow` + `SDL_PollEvent` triad
  (`window.cppm:42,52,79`) is replaced wholesale; there is no SDL anywhere in the Rust tree.
- **`raw-window-handle` 0.6 is the surface seam, and it lives here.** `Window` exposes a raw window +
  display handle pair via `raw-window-handle`'s `HasWindowHandle`/`HasDisplayHandle`; `ash-window`
  (in `saffron-rendering`, the FFI crate) consumes that pair for `ash_window::create_surface` and
  `enumerate_required_extensions`. The C++ surface path (`SDL_Vulkan_CreateSurface`,
  `SDL_Vulkan_GetInstanceExtensions`) crosses the SDL↔Vulkan boundary the same way; the handle is the
  decoupling point so `saffron-window` never depends on `ash`.
- **Typed signals over `saffron-signal`, not winit callbacks raw.** The crate holds one
  `SubscriberList<Args...>` per event (phase-3) and translates winit `WindowEvent`s into `publish`
  calls — `WindowEvent::Resized` → `on_resize.publish(w, h)`, `KeyboardInput` (pressed) →
  `on_key_pressed.publish(keycode, is_repeat)`, `KeyboardInput` (released) → `on_key_released`,
  `CloseRequested` → `on_close`, `DroppedFile` → `on_file_dropped`. This preserves the C++ contract 1:1
  (`window.cppm:96-113`): downstream code subscribes to a typed signal, never parses a raw event. The
  `eventSinks` raw-`SDL_Event` forwarding (`window.cppm:37,81`) the C++ host used to feed the gizmo +
  fly-camera is re-expressed as a typed raw-winit-event signal (or a host-owned dispatch in PP-10) —
  **not** a `Vec<Box<dyn Fn(SDL_Event)>>` transliteration, because there is no SDL event type to
  forward.
- **Windowless construction is a first-class state, not a `null` handle.** The C++ `Window` carries a
  `handle = nullptr` sentinel (`window.cppm:24`); in Rust the windowed/headless split is a real
  distinction, not a nullable pointer. The headless editor host constructs the signal facade with no
  winit window and no raw-window-handle to hand out (any surface accessor returns `None` /
  `Result::Err`), so `ash-window` is never invoked on that path and no `VK_KHR_surface` extension is
  required (dependency-adoption §2b). The present-only standalone host is the only path that builds a
  real winit window.
- **`thiserror` Result, no `Result<T, String>`.** Window creation is fallible (winit event-loop /
  window-builder errors); the crate defines its own `saffron_window::Error` enum and `Result<T>`
  alias (conventions §2), with winit's error types lifted via `#[from]`. The C++
  `Err("SDL_CreateWindow failed: …")` string becomes a typed variant.
- **`WindowConfig` is `Default`, not a free `newWindow`.** The C++ `WindowConfig` aggregate
  (`window.cppm:13`: title, width, height, hidden) becomes a struct deriving `Default` with the same
  field defaults (`"Saffron"`, 1600×900, `hidden = false`), and `newWindow(config)` becomes
  `Window::new(config)` / `Window::headless()` associated functions (conventions §1, the
  `newThing` → `Thing::new` rule).

## Grounding (real files/symbols)

- `engine-old/source/saffron/window/window.cppm`
  - `WindowConfig` (`:13`) — title / width / height / hidden; `Default`-equivalent field values.
  - `Window` (`:22`) — the plain-data carrier: native handle, current `width`/`height`,
    `shouldClose`, and the five typed signals: `onClose` (`:29`), `onResize` `<u32,u32>` (`:30`),
    `onKeyPressed` `<i32,bool>` (`:31`), `onKeyReleased` `<i32>` (`:32`), `onFileDropped`
    `<std::string>` (`:33`). The `eventSinks` raw-event forwarding (`:37`).
  - `newWindow` (`:40`) — `SDL_Init(SDL_INIT_VIDEO)` + `SDL_CreateWindow` with
    `SDL_WINDOW_VULKAN | RESIZABLE | HIGH_PIXEL_DENSITY`, the `hidden → SDL_WINDOW_HIDDEN` flag, and
    the two `Err(...)`-on-failure returns (`:43`, `:55`).
  - `destroyWindow` (`:66`) — `SDL_DestroyWindow` + `SDL_Quit`; in Rust this is `Drop`, not a free
    function (conventions §5).
  - `pollEvents` (`:76`) — the `SDL_PollEvent` loop and the event→signal translation table:
    quit/close-requested → `onClose` (`:86-95`), pixel-size-changed → `onResize` (`:96-101`),
    key-down → `onKeyPressed` (`:102-105`), key-up → `onKeyReleased` (`:106-109`), drop-file →
    `onFileDropped` (`:110-113`). This is the table the winit `WindowEvent` match reproduces.
- `00-foundations/dependency-adoption.md`
  - §5 (`winit` 0.30 + `raw-window-handle` 0.6 pick; the `ApplicationHandler` model is PP-10's; the
    headless-has-no-window subtlety; the X11 child-window embedding is *gone*, a subtraction not a gap).
  - §2b (`ash-window` 0.13 consumes the `raw-window-handle` 0.6 pair; the headless path never builds a
    surface and assembles its instance-extension list by hand — so `saffron-window` must not require a
    window for the editor path).
- `00-foundations/conventions.md` §2 (typed `thiserror` Result), §5 (`Drop` for `destroyWindow`), §6
  (the `SubscriberList` contract the typed signals reuse — handler returns `bool` to stop
  propagation, snapshot-iterate), §1 (`newWindow` → associated function; `WindowConfig` → `Default`).
- `00-foundations/README.md` §2.1 (the `saffron-window → {core, signal}` edge: the crate depends on
  `signal` *because* `Window` exposes typed `SubscriberList` signals — that edge is preserved).

## Acceptance gate

- `cargo build -p saffron-window` and the full workspace build are green; `saffron-window` depends
  only on `saffron-core` and `saffron-signal` (plus `winit` + `raw-window-handle`), matching the
  README §2 graph edge.
- `cargo test -p saffron-window` covers the typed-signal translation: a unit test drives each event
  into the signal facade and asserts the matching `SubscriberList` fires with the right args —
  `on_resize` gets `(w, h)`, `on_key_pressed` gets `(keycode, is_repeat)`, `on_key_released` gets the
  keycode, `on_close` fires on a close request, `on_file_dropped` gets the path. These exercise the
  facade without a live winit event loop (the translation is a pure function over a synthesized
  event), so they run headless on the gate.
- The crate exposes a `raw-window-handle` / `raw-display-handle` accessor (`HasWindowHandle` /
  `HasDisplayHandle` or an explicit getter) that `ash-window` can consume for surface creation; a test
  asserts a windowed instance yields a handle and a headless instance yields `None` / an error.
- **The headless path is exercised:** `Window::headless()` (or the equivalent windowless construction)
  builds the signal facade with no winit window, returns no surface handle, and is the path the editor
  host takes — a test constructs it, confirms the signals are still usable, and confirms no surface
  handle is produced.
- `cargo clippy -p saffron-window` and `cargo fmt --check` clean; crate root `#![deny(unsafe_code)]`
  (the `raw-window-handle` accessor stays on the safe `HasWindowHandle` trait surface; any `unsafe`
  needed for a raw FFI handle belongs in `saffron-rendering`'s ash seam, not here).
