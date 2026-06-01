+++
title = 'Signals'
weight = 2
math = false
+++

# Signals

The signal/slot primitive from `Saffron.Signal` and the typed signals the `Window` exposes.

## `SubscriberList<Args...>`
The engine-wide event list. A handler returns `true` to stop propagation to later subscribers.

| Member | Signature | Effect |
|---|---|---|
| `subscribe` | `auto subscribe(std::function<bool(Args...)> handler) -> SubscriptionId` | append handler, return token |
| `unsubscribe` | `void unsubscribe(SubscriptionId id)` | remove the handler with that id |
| `publish` | `void publish(Args... args) const` | dispatch over a snapshot until a handler returns `true` |
| `entries` | `std::vector<Entry>` | live handlers |
| `nextId` | `u64` (starts at 1) | next id to hand out |

`SubscriptionId` is a `struct { u64 value = 0; }`. `Entry` is `struct { u64 id; std::function<bool(Args...)> handler; }`.

## `Window` typed signals
Members of `struct Window` (also `SDL_Window* handle`, `u32 width`, `u32 height`, `bool shouldClose`).

| Signal | Type | Args | Fired on |
|---|---|---|---|
| `onClose` | `SubscriberList<>` | — | `SDL_EVENT_QUIT` / `WINDOW_CLOSE_REQUESTED` |
| `onResize` | `SubscriberList<u32, u32>` | width, height (pixels) | `WINDOW_PIXEL_SIZE_CHANGED` |
| `onKeyPressed` | `SubscriberList<i32, bool>` | keycode, isRepeat | `KEY_DOWN` |
| `onKeyReleased` | `SubscriberList<i32>` | keycode | `KEY_UP` |
| `onFileDropped` | `SubscriberList<std::string>` | dropped file path | `DROP_FILE` |
| `eventSinks` | `std::vector<std::function<void(const SDL_Event&)>>` | raw `SDL_Event` | every event, before typed dispatch (ImGui feeds off this) |

## Window functions
| Symbol | Signature |
|---|---|
| `newWindow` | `auto newWindow(const WindowConfig&) -> Result<Window>` |
| `destroyWindow` | `void destroyWindow(Window&)` |
| `pollEvents` | `void pollEvents(Window&)` |

`WindowConfig` is `struct { std::string title = "Saffron"; u32 width = 1600; u32 height = 900; }`.

## Related
- [Window and events](../../explanations/app-lifecycle-and-window/window-and-events/) — how SDL events become typed signals
