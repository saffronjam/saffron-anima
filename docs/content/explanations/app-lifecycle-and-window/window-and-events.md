+++
title = 'Window & events'
weight = 4
+++

# Window & events

A window is an on-screen surface that the renderer presents into and that delivers the operating
system's input events. In Anima it is a thin facade over a winit window plus a set of typed
event signals. Input reaches the rest of the program through those signals — a layer subscribes
in `on_attach` and gets called back when the matching event occurs.

```rust
pub struct Window {
    handle: Option<WinitWindow>,   // None in headless mode
    width: u32,
    height: u32,
    should_close: bool,

    pub on_close: SubscriberList<()>,
    pub on_resize: SubscriberList<(u32, u32)>,          // width, height (pixels)
    pub on_key_pressed: SubscriberList<(KeyCode, bool)>, // keycode, is_repeat
    pub on_key_released: SubscriberList<KeyCode>,        // keycode
    pub on_file_dropped: SubscriberList<PathBuf>,        // dropped file path
    pub on_raw_event: SubscriberList<WindowEvent>,       // every raw winit event
}
```

## Two construction modes

`Window::new` builds a real winit window from the host's active event loop and hands out a
`raw-window-handle` / `raw-display-handle` pair that `ash-window` (in `saffron-rendering`)
consumes for surface creation. That is the standalone present-only host path.

`Window::headless` is the windowless mode the editor host takes. The signal facade and the
public type exist with no OS window behind them; the handle accessors return `None` (or a
`HandleError`), never a sentinel, so no Vulkan surface is built on that path. `KeyCode` is
winit's `PhysicalKey` — a location-stable physical key identity, exhaustively matchable
downstream.

## Dispatch

The winit `ApplicationHandler` event loop that drives `poll → on_update → … → present` lives in
`saffron_app`; this crate provides the translation from a raw winit event to the typed signals.
`dispatch_window_event` is that pure translation table: it publishes the raw event to
`on_raw_event` first, then maps the events it recognizes into typed signal publishes. It runs
without a live event loop (a synthesized `WindowEvent` is enough), so it is testable headless.

## Typed signals

Each signal is a `SubscriberList<Args>`, the engine-wide signal/slot type. A subscriber is a
closure that returns `true` to stop propagation or `false` to let later subscribers also see the
event. winit events map in as:

| Signal | winit event | Payload |
|---|---|---|
| `on_close` | `WindowEvent::CloseRequested` | none; also latches `should_close` |
| `on_resize` | `WindowEvent::Resized` | new width, height in pixels |
| `on_key_pressed` | `WindowEvent::KeyboardInput` (pressed) | keycode, `is_repeat` |
| `on_key_released` | `WindowEvent::KeyboardInput` (released) | keycode |
| `on_file_dropped` | `WindowEvent::DroppedFile` | dropped file path |

`on_resize` publishes the **pixel** size: `dispatch_window_event` reads winit's `PhysicalSize`
and updates `width`/`height` from it. In windowed mode the host subscribes the Escape key to
`request_close`, and `WindowedApp::window_event` exits the loop on `CloseRequested`; the editor
subscribes `on_file_dropped` to import dropped models and textures.

## Raw event sink

Some consumers need the whole `WindowEvent`, not a typed slice. The viewport gizmo and the
editor fly-camera are the main ones: they read raw mouse motion, button state, and modifier
deltas that no typed signal carries. Rather than couple `Window` to those consumers, `Window`
exposes `on_raw_event`, a `SubscriberList<WindowEvent>`, and the host subscribes a handler that
forwards each event to the gizmo and camera input.

The raw sink fires *before* typed dispatch, so it sees an event even when a typed signal later
consumes it. This keeps `Window` ignorant of who is listening: it knows how to forward raw
events and how to publish typed ones, nothing about the consumers.

## In the code

| What | File | Symbols |
|---|---|---|
| Window data + signals | `window/src/lib.rs` | `Window`, `WindowConfig` |
| Create modes | `window/src/lib.rs` | `Window::new`, `Window::headless` |
| Event translation | `window/src/lib.rs` | `dispatch_window_event`, `dispatch_key` |
| Surface handles | `window/src/lib.rs` | `HasWindowHandle`, `HasDisplayHandle` impls |
| Signal primitive | `signal/src/lib.rs` | `SubscriberList`, `subscribe`, `publish` |

> [!TIP]
> `width`/`height` are 0 until the first resize event, and a headless window starts at `0×0`.
> The loop treats a 0 dimension as minimized and skips the frame, so don't divide by the window
> size in `on_update` without guarding against zero.

## Related

- [Main loop](../main-loop-and-run/) — where the event loop runs and `on_close` is wired
- [Layers as a trait of hooks](../layer-system/) — a layer subscribes to these signals in `on_attach`
