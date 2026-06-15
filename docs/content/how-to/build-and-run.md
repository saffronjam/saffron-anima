+++
title = 'Build and run'
weight = 1
math = false
+++

# Build and run

Build the C++ engine host and the Tauri editor, then run both.

The editor is a Tauri/React app that drives the engine over the control socket. The engine is the C++ `SaffronAnima` host executable, built from the `engine/` project as a headless viewport host. Both build in the `saffron-build` toolbox. The Silverblue host has no C++ toolchain; the home directory is shared into the container, so the host's `bun` runs inside the toolbox by PATH.

## Build the engine host

Build `SaffronAnima` first; the Tauri app spawns it on launch.

1. Configure once, or after any CMake change:
   ```sh
   toolbox run -c saffron-build bash -lc '
     cd /var/home/saffronjam/repos/SaffronAnima
     cmake --preset debug'
   ```
2. Build with `-j1` (parallel builds intermittently hit a Clang module-BMI ICE):
   ```sh
   toolbox run -c saffron-build bash -lc '
     cd /var/home/saffronjam/repos/SaffronAnima
     cmake --build build/debug -j1'
   ```
3. Run the host on its own. It opens its own window, useful for a headless check or for driving it from the `sa` CLI without the Tauri app:
   ```sh
   toolbox run -c saffron-build bash -lc '
     cd /var/home/saffronjam/repos/SaffronAnima
     ./build/debug/bin/SaffronAnima'
   ```

## Run the Tauri editor

The Tauri app builds and runs in the same toolbox, with the host `bun` on the PATH (the home directory is shared in). `bun run tauri dev` spawns the `SaffronAnima` host built above and composites its frames under the webview, so build the host first.

```sh
toolbox run -c saffron-build bash -lc '
  export PATH="/var/home/saffronjam/.bun/bin:$PATH"
  cd /var/home/saffronjam/repos/SaffronAnima/editor
  bun install        # first time / after dependency changes
  bun run check      # generate protocol types + typecheck
  bun run tauri dev  # spawns the engine host + opens the editor'
```

`bun run check` regenerates `editor/src/protocol/` from the [control schemas](../../explanations/tooling-and-control/shared-types/) and typechecks. The dev launch needs a Wayland session because the viewport presents on a wl_subsurface; use a real desktop session.

## Verify

- **Engine host alone**: the window opens and presents the scene; drive it with the `sa` CLI over its control socket.
- **Tauri editor**: the shell opens with the Hierarchy / tabbed Inspector·Environment·Stats / Assets / Viewport dock; a "Preparing renderer…" overlay clears once the embedded scene attaches.
- For a headless engine check, bound the host run and dump the offscreen image:
  ```sh
  SAFFRON_EXIT_AFTER_FRAMES=5 SAFFRON_CAPTURE=/tmp/frame.png ./build/debug/bin/SaffronAnima
  ```
  `SAFFRON_EXIT_AFTER_FRAMES=N` exits after `N` frames; `SAFFRON_CAPTURE=path` writes the viewport image at exit.

> [!NOTE]
> The Tauri editor is the only editor. Undo/redo, multi-viewport, and native Wayland are non-goals for now.

## In the code

| What | File | Symbols |
|---|---|---|
| Toolbox + preset + run | `AGENTS.md` | the `saffron-build` recipe, `cmake --preset debug` |
| The loop + frame limit | `app.cppm` | `run`, `detail::frameLimitFromEnv` |
| Capture on exit | `app.cppm` | `SAFFRON_CAPTURE` → `captureViewport` |
| Debug preset | `CMakePresets.json` | `debug` (clang++, libc++, lld, Ninja) |
| Frontend scripts | `editor/package.json` | `dev`, `check`, `tauri:dev`, `gen:protocol` |

## Related

- [Tauri editor and the viewport bridge](../../explanations/ui-and-editor/tauri-editor-and-viewport-bridge/) — how the editor drives the host
- [Main loop](../../explanations/app-lifecycle-and-window/main-loop-and-run/)
- [Headless runs and capture](../../explanations/app-lifecycle-and-window/headless-and-capture/)
