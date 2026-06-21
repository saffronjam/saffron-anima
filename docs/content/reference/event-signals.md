+++
title = 'Signals'
weight = 2
math = false
+++

# Signals

A signal is a list of subscribers that a publisher invokes. This page covers the signal/slot primitive in `saffron-signal` and the typed signals `saffron-window`'s `Window` exposes.

## `SubscriberList<Args>`

The engine-wide event list. A handler is any `FnMut(Args) -> bool`; returning `true` stops propagation to later subscribers. `publish` iterates a *snapshot* of the subscriber set, so a handler may subscribe or unsubscribe (itself included) mid-dispatch without disturbing the in-flight iteration. The list is single-thread (`!Send`) — every consumer dispatches on the main thread — and every method takes `&self` through interior mutability.

| What | File | Symbols |
|---|---|---|
| The signal/slot list | `lib.rs` | `SubscriberList`, `SubscriptionId` |

| Member | Effect |
|---|---|
| `subscribe(handler: impl FnMut(Args) -> bool + 'static) -> SubscriptionId` | append a handler, return its token |
| `unsubscribe(id: SubscriptionId)` | remove the handler with that id (no-op if gone) |
| `publish(args: Args)` | dispatch over a snapshot until a handler returns `true` (`Args: Clone`) |
| `len() -> usize` / `is_empty() -> bool` | the live handler count |

`SubscriptionId(pub u64)` is the subscription token; ids are monotonic and never reused for the life of the list. `Args` is the payload — a single value, or a tuple for several (`SubscriberList<(u32, u32)>`).

## `Window` typed signals

`Window` is a thin facade over `winit` 0.30 that translates each `WindowEvent` into typed signals. It holds the current pixel size, a `should_close` latch, and (when windowed) the underlying winit window. Two construction modes exist: `Window::new(event_loop, config)` builds a real OS window (the standalone present-only host), and `Window::headless()` builds the windowless facade the editor host takes — the signals are fully usable with no OS window behind them.

| What | File | Symbols |
|---|---|---|
| The window facade and its signals | `lib.rs` | `Window`, `WindowConfig`, `Window::new`, `Window::headless`, `Window::dispatch_window_event` |

| Signal | Type | Args | Fired on |
|---|---|---|---|
| `on_close` | `SubscriberList<()>` | — | `WindowEvent::CloseRequested` (also latches `should_close`) |
| `on_resize` | `SubscriberList<(u32, u32)>` | width, height (pixels) | `WindowEvent::Resized` |
| `on_key_pressed` | `SubscriberList<(KeyCode, bool)>` | keycode, is_repeat | `WindowEvent::KeyboardInput` pressed |
| `on_key_released` | `SubscriberList<KeyCode>` | keycode | `WindowEvent::KeyboardInput` released |
| `on_file_dropped` | `SubscriberList<std::path::PathBuf>` | dropped file path | `WindowEvent::DroppedFile` |
| `on_raw_event` | `SubscriberList<WindowEvent>` | the raw winit event | every event, before typed dispatch (the gizmo/camera input feeds off this) |

`KeyCode` is `winit::keyboard::PhysicalKey` — the location-stable physical key, exhaustively matchable downstream (`PhysicalKey::Code(KeyCode::Escape)`). `dispatch_window_event` publishes to `on_raw_event` first, then maps the event to the typed signals; it runs without a live event loop, so it is testable headless.

## Window accessors and config

| Symbol | Effect |
|---|---|
| `width() -> u32` / `height() -> u32` | current pixel size |
| `should_close() -> bool` / `request_close()` | the close latch (read / set) |
| `is_windowed() -> bool` | `true` on the standalone host path |
| `winit_window() -> Option<&Window>` | the underlying winit window, when windowed |

`WindowConfig` is `{ title: String, width: u32, height: u32, hidden: bool }`, defaulting to `"Saffron"`, 1600×900, not hidden. The `winit` event loop that drives `poll → on_update → … → present` lives in `saffron-host`; this crate provides the signals it publishes into.

## Related

- [Window and events](../../explanations/app-lifecycle-and-window/window-and-events/) — how winit events become typed signals
