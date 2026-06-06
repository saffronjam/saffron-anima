+++
title = 'Tauri editor and the viewport bridge'
weight = 1
+++

# Tauri editor and the viewport bridge

The editor is a Tauri desktop application: a React/TypeScript front-end in a webview, a
thin Rust backend, and the engine running as a separate process. The webview never renders
the 3D scene. The engine renders headless and the editor composites its frames below the
transparent UI ([viewport compositing](../viewport-compositing/)), so the viewport shows
the live render while the webview owns all chrome — including chrome blended over the
scene itself.

Every editor operation that touches the scene rides the same JSON-over-unix-socket
[control protocol](../../tooling-and-control/control-plane-architecture/) the `se` CLI
speaks. The engine project builds the `SaffronEngine` host executable — a headless host
that boots the engine, publishes frames, and drains the control socket, with no panels of
its own.

## Two processes, one socket

The Rust backend spawns `SaffronEngine` with `SAFFRON_EDITOR_NATIVE_VIEWPORT=1` (hidden
window), a per-instance `SAFFRON_CONTROL_SOCK` (pid-scoped, so two editor windows do not
collide), `SAFFRON_VIEWPORT_SHM` naming the shared-memory segment frames publish into, and
a `SAFFRON_MAX_FPS` cap. The engine is then the renderer and the webview is the UI,
talking only over that socket.

The TypeScript side is a typed client over one generic Rust passthrough. Every scene,
asset, and render command is `invoke('control', { cmd, params })`; the Rust layer forwards
it verbatim, turns an engine `ok:false` into a rejected promise, and otherwise resolves
the result JSON. Adding a new `se` command needs no Rust change — the typed wrapper in
`client.ts` and a DTO entry are all that move.

```ts
async function call<C extends keyof CommandResultMap>(
  cmd: C,
  params?: object,
): Promise<CommandResultMap[C]> {
  return invoke<CommandResultMap[C]>("control", { cmd, params: params ?? {} });
}
```

Rust handles only the lifecycle and presenter commands directly — `start_engine`,
`set_viewport_bounds`, `set_viewport_hidden`, `quit_engine`, `engine_alive` — because
those manage the child process and the compositor-side subsurface rather than the scene.

> [!NOTE]
> The presenter is a Wayland subsurface, so the editor requires a Wayland session.

## Auto-start and the loading overlay

On boot the Rust `.setup()` hook installs the presenter on the GTK window, spawns the
engine, then polls `viewport-native-info` with a child-liveness-aware bounded retry that
distinguishes "socket not bound yet" from "process crashed". React drives an
`engineStatus.phase` state machine — `idle → starting → attaching → ready` — and the
[viewport panel](../viewport-panel/) probes the same command before flipping to `ready`.
A `<LoadingOverlay/>` covers the viewport region until then; it paints an opaque
background, which also covers the transparent hole before the first frame arrives.

## Crash recovery

The reconcile poll doubles as a liveness watchdog: each tick it calls `engineAlive()`,
which uses `child.try_wait()` rather than a stale handle, so a dead engine reads as dead.
If the child has exited, the store flips `phase` back to `error`, the overlay reappears,
and it offers **Retry** (re-probe) and **Restart** (quit, re-spawn, re-probe).

## In the code

| What | File | Symbols |
|---|---|---|
| Typed passthrough client | `editor/src/control/client.ts` | `call`, `callRaw`, `client` |
| Lifecycle + presenter commands | `editor/src/control/client.ts` | `startEngine`, `setViewportBounds`, `setViewportHidden`, `quitEngine`, `engineAlive` |
| Engine spawn + env | `editor/src-tauri/src/lib.rs` | `spawn_engine`, `auto_start`, `nvidia_present` |
| App shell + lifecycle events | `editor/src/app/App.tsx` | `App`, `engine-phase` / `viewport-error` listeners |
| Phase state machine | `editor/src/state/store.ts` | `EngineStatus`, `setPhase` |
| Loading + crash overlay | `editor/src/app/LoadingOverlay.tsx` | `LoadingOverlay`, Retry / Restart |

## Related

- [Viewport compositing](../viewport-compositing/) — how the engine's frames reach the screen
- [Viewport panel](../viewport-panel/) — the host div the subsurface is glued to
- [Theme and fonts](../theme-and-fonts/) — the shadcn/Tailwind chrome around the viewport
- [Shared types](../../tooling-and-control/shared-types/) — the DTO-first wire contract the typed client consumes
