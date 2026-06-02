+++
title = 'Tauri editor and the X11 viewport bridge'
weight = 1
+++

# Tauri editor and the X11 viewport bridge

The editor is a Tauri desktop app: a React/TypeScript front-end in a webview, a thin Rust backend, and the engine running as a separate process. The webview never renders the 3D scene. Instead the engine's own SDL/Vulkan window is reparented as a native child *over* a placeholder div, so the scene you see in the viewport is the engine presenting directly to its swapchain — the webview just owns the surrounding chrome. Every editor operation that touches the scene goes over the same JSON-over-unix-socket [control protocol](../../tooling-and-control/control-plane-architecture/) the `se` CLI speaks.

This replaces the old Dear ImGui editor. The engine project builds the `SaffronEngine` host executable — a headless viewport host that boots the engine, opens its window, and drains the control socket, with no ImGui panels of its own.

## Two processes, one socket

The Rust backend spawns `SaffronEngine` with `SAFFRON_EDITOR_NATIVE_VIEWPORT=1`, a per-instance `SAFFRON_CONTROL_SOCK` (pid-scoped so two editor windows don't collide), and `SDL_VIDEODRIVER=x11`. From then on the engine is the renderer and the webview is the UI, talking only over that socket.

The TypeScript side is a typed client over **one** generic Rust passthrough. Every scene/asset/render command is `invoke('control', { cmd, params })`; the Rust layer forwards it verbatim, turns an engine `ok:false` into a rejected promise, and otherwise resolves the result JSON. Adding a new `se` command needs zero Rust change — the typed wrapper in `client.ts` and a schema entry are all that move.

```ts
async function call<C extends keyof CommandResultMap>(
  cmd: C,
  params?: object,
): Promise<CommandResultMap[C]> {
  return invoke<CommandResultMap[C]>("control", { cmd, params: params ?? {} });
}
```

The only commands Rust handles directly are the window-handle lifecycle ones — `start_engine`, `attach_native_viewport`, `resize_native_viewport`, `quit_engine`, `engine_alive` — because those manipulate the native X11 window, not the scene.

## The present-only bridge

When `SAFFRON_EDITOR_NATIVE_VIEWPORT` is set the engine runs in **present-only** mode: it renders the scene (and the editor overlays — the [gizmo](../gizmo/) and light/camera billboards) into its offscreen target and blits that straight to the swapchain. ImGui is skipped entirely; there are no engine-side panels. `presentViewportToSwapchain` does the offscreen→swapchain blit (`transferSrc`→`transferDst`, then `PresentSrcKHR`).

Attaching is a reparent, not a copy. `attach-native-viewport` reads the Tauri window's X11 display + window number, then `XReparentWindow`s the engine's SDL window under it, `XMoveResizeWindow`s it to the viewport rect, and `XMapRaised`s it. There is no per-frame frame transfer — the engine owns its surface and the compositor stacks it over the webview. (An earlier fd-export/DMA-BUF path was explored and deliberately *not* adopted; X11-reparent is the sole bridge.)

> [!NOTE]
> The reparented child hard-requires Xlib, so the editor is X11/XWayland only. Native Wayland (which cannot reparent a foreign window) is a non-goal.

## Auto-start, auto-attach, loading overlay

On boot the Rust `.setup()` hook spawns the engine, then polls `viewport-native-info` with a child-liveness-aware bounded retry (it distinguishes "socket not bound yet" from "process crashed"). React drives an `engineStatus.phase` state machine — `idle → starting → attaching → ready` — and the [Viewport panel](../viewport-panel/) waits for a non-zero rect before sending the first `attach-native-viewport`. A `<LoadingOverlay/>` covers the viewport region until `phase === 'ready'`.

The overlay has to be a plain absolutely-positioned sibling inside the viewport panel, never a Radix portal: the engine only `XMapRaised`s the native window on a *successful* attach, so before that the webview can still paint over the (unmapped) rect.

## Crash recovery

The reconcile poll doubles as a liveness watchdog: each tick it calls `engineAlive()` (which uses `child.try_wait()`, not a stale handle, so a dead engine reads as dead). If the child has exited, the store flips `phase` back to `error`, the overlay reappears, and it offers **Retry** (re-attach) and **Restart** (quit + re-spawn + re-attach).

## In the code

| What | File | Symbols |
|---|---|---|
| Typed passthrough client | `editor/src/control/client.ts` | `call`, `callRaw`, `client` |
| Window-handle lifecycle | `editor/src/control/client.ts` | `startEngine`, `attachViewport`, `resizeViewport`, `quitEngine`, `engineAlive` |
| App shell + lifecycle events | `editor/src/app/App.tsx` | `App`, `engine-phase` / `viewport-error` listeners |
| Phase state machine | `editor/src/state/store.ts` | `EngineStatus`, `setPhase` |
| Loading + crash overlay | `editor/src/app/LoadingOverlay.tsx` | `LoadingOverlay`, Retry / Restart |
| Present-only blit (engine) | `renderer.cppm` | `setPresentViewportOnly`, `presentViewportToSwapchain` |
| Reparent (engine) | `control_commands_render.cpp` | `attach-native-viewport`, `viewport-native-info` |

## Related

- [Control plane](../../tooling-and-control/control-plane-architecture/) — the socket every command rides on
- [Viewport panel](../viewport-panel/) — the host div the native window is glued to
- [Theme and fonts](../theme-and-fonts/) — the shadcn/Tailwind chrome around the viewport
- [Shared types](../../tooling-and-control/shared-types/) — the schema-first wire contract the typed client consumes
