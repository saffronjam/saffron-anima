# editor — Tauri/React editor

The editor is a **Tauri 2 / React 19 / TypeScript** app. It spawns the `SaffronEngine`
host, reparents the host's X11 window as a native child inside the viewport panel, and
drives every operation over the JSON-over-unix-socket control plane. The engine renders;
this app is the UI shell around a native viewport.

## Layout

```
src/
  app/         shell (App.tsx), docking layout, menu/topbar, lifecycle wiring
  panels/      Hierarchy, Inspector, Assets, Environment, RenderStats, Viewport, Topbar
  components/  shadcn/ui + field renderers (NumberDrag, ColorField, VectorEditor, …)
  control/     typed control client over the Tauri bridge (client.ts)
  state/       Zustand store + the reconcile poll (store.ts)
  protocol/    GENERATED TypeScript types — do not edit by hand
  lib/         utilities
scripts/gen-protocol.ts   schemas/control/*.json → src/protocol/index.ts
src-tauri/     Rust bridge (lib.rs): engine spawn, control passthrough, X11 reparent
```

Stack (see `package.json`): React 19, Tauri 2, Zustand 5, Vite 7, Tailwind v4
(`@tailwindcss/vite`), shadcn/ui (Radix), `react-resizable-panels`. Lint/format via
**oxc** (`oxlint` + `oxfmt`, configs in `.oxlintrc.json` / `.oxfmtrc.json`) and prettier.

## Workflow

```sh
bun install
bun run check    # gen:protocol + tsc --noEmit
bun run build    # gen:protocol + tsc + vite build
bun run tauri:dev  # launches the app; needs an X11/XWayland display for the reparent
```

`bun run gen:protocol` regenerates `src/protocol/index.ts` from `schemas/control/`.

## Rules that are easy to break

- **`src/protocol/index.ts` is generated.** Never edit it. Edit the schemas in
  `schemas/control/`, run `bun run gen:protocol`, and commit the result. The hand-kept
  `CommandResultMap` in `src/control/client.ts` maps commands to their result types.
- **Entity IDs are strings end-to-end.** They are u64 in the engine; treat them as opaque
  strings in JS and **never `Number()` them** — that silently corrupts large IDs.
- **The native viewport is an X11 child painted on top of the webview.** Menus, popovers,
  and dialogs must anchor to non-viewport regions (sidebars, topbar, menubar) or they get
  covered. When a modal needs the space, the viewport parks off-screen rather than fighting
  the z-order.
- **The control client is one generic passthrough.** Rust exposes a single
  `control(cmd, params)` command (it rejects on `ok:false`); the typed wrappers in
  `client.ts` layer on top. Dedicated lifecycle commands (spawn/attach/resize/quit/alive)
  are separate from the passthrough. Use `.raw()` only for not-yet-typed commands.
- **State sync is a focus-gated poll, not push.** `store.ts` reconciles at ~6 Hz, gated on
  `document.hasFocus()` and `phase === 'ready'`, keyed on the engine's `sceneVersion` /
  `selectionVersion` stamps. High-frequency edits (field scrubs, gizmo drags) use coalescers
  and set `dragActive` to block the poll from clobbering optimistic local state.

The Rust bridge sets `SDL_VIDEODRIVER=x11` and a per-PID socket under `$XDG_RUNTIME_DIR`,
spawns `$SAFFRON_ENGINE_BIN` (default `build/debug/bin/SaffronEngine`), and reparents via
`raw-window-handle`. A watchdog flips the UI to an error overlay if the engine dies.
