# editor — Tauri/React editor

The editor is a **Tauri 2 / React 19 / TypeScript** app. It spawns the `SaffronEngine`
host headless, presents the host's shared-memory frames on a Wayland subsurface below its
transparent window (the viewport panel is a hole the render shows through), and drives
every operation over the JSON-over-unix-socket control plane. The engine renders; this
app is the UI shell composited over the live viewport.

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
scripts/gen-protocol.ts   re-runs tools/gen-control-dto → src/protocol/se-types.ts
src-tauri/     Rust bridge (lib.rs + wayland_viewport.rs): engine spawn, control passthrough, subsurface presenter
```

Stack (see `package.json`): React 19, Tauri 2, Zustand 5, Vite 7, Tailwind v4
(`@tailwindcss/vite`), shadcn/ui (Radix), `react-resizable-panels`. Lint/format via
**oxc** (`oxlint` + `oxfmt`, configs in `.oxlintrc.json` / `.oxfmtrc.json`) and prettier.

## Workflow

```sh
bun install
bun run check    # gen:protocol + tsc --noEmit
bun run build    # gen:protocol + tsc + vite build
bun run tauri:dev  # launches the app; needs a Wayland session for the subsurface presenter
```

`bun run gen:protocol` regenerates `src/protocol/index.ts` from `schemas/control/`.

## Rules that are easy to break

- **`src/protocol/se-types.ts` is generated.** Never edit it. Edit the DTOs in
  `control_dto.cppm` + `tools/gen-control-dto/gen.ts`, run `bun run gen:protocol`, and
  commit the result. `src/protocol/index.ts` is the hand-kept re-export shim (compat
  overrides live there), and `client.ts` layers the typed wrappers on top.
- **Entity IDs are strings end-to-end.** They are u64 in the engine; treat them as opaque
  strings in JS and **never `Number()` them** — that silently corrupts large IDs.
- **The viewport is a transparent hole down to the engine's subsurface.** The page-level
  backgrounds (index.html, body) stay transparent; every visible region paints its own
  opaque background. DOM freely composites over the viewport. When a modal or another tab
  owns the region, `viewportHidden` parks the subsurface and the panel paints opaque so the
  desktop never shows through.
- **The control client is one generic passthrough.** Rust exposes a single
  `control(cmd, params)` command (it rejects on `ok:false`); the typed wrappers in
  `client.ts` layer on top. Dedicated lifecycle/presenter commands (spawn/bounds/hidden/
  quit/alive) are separate from the passthrough. Use `.raw()` only for not-yet-typed
  commands.
- **State sync is a focus-gated poll, not push.** `store.ts` reconciles at ~6 Hz, gated on
  `document.hasFocus()` and `phase === 'ready'`, keyed on the engine's `sceneVersion` /
  `selectionVersion` stamps. High-frequency edits (field scrubs, gizmo drags) use coalescers
  and set `dragActive` to block the poll from clobbering optimistic local state.

The Rust bridge sets a per-PID socket under `$XDG_RUNTIME_DIR` and a per-PID shm segment,
spawns `$SAFFRON_ENGINE_BIN` (default `build/debug/bin/SaffronEngine`) with
`SAFFRON_VIEWPORT_SHM` + `SAFFRON_MAX_FPS` (and the NVIDIA `VK_ICD_FILENAMES` guard), and
presents via `wayland_viewport.rs`. A watchdog flips the UI to an error overlay if the
engine dies.
