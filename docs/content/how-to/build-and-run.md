+++
title = 'Build and run'
weight = 1
math = false
+++

# Build and run

The editor is a Tauri/React app driving the engine over the control socket; the engine itself is the C++ `SaffronEditor` target, now a headless viewport host under `editor-old/`. Both build in the `saffron-build` toolbox. (The Silverblue host has no C++ toolchain; the home directory is shared into the container, so the host's `bun` runs *inside* the toolbox by PATH.)

## Build the engine host

Build `SaffronEditor` first — the Tauri app spawns it on launch.

## Steps

1. Configure once, or after any CMake change:
   ```sh
   toolbox run -c saffron-build bash -lc '
     cd /var/home/saffronjam/repos/SaffronEngine
     cmake --preset debug'
   ```
2. Build with `-j1` (parallel builds intermittently hit a Clang module-BMI ICE):
   ```sh
   toolbox run -c saffron-build bash -lc '
     cd /var/home/saffronjam/repos/SaffronEngine
     cmake --build build/debug -j1'
   ```
3. Run the host on its own (it opens its own window — useful for a headless check or driving it from the `se` CLI without the Tauri app):
   ```sh
   toolbox run -c saffron-build bash -lc '
     cd /var/home/saffronjam/repos/SaffronEngine
     ./build/debug/bin/SaffronEditor'
   ```

## Run the Tauri editor

The Tauri app builds and runs in the *same* toolbox, with the host `bun` on the PATH (the home dir is shared in). `bun run tauri dev` spawns the `SaffronEditor` host (built above) and reparents its viewport into the webview, so build the host first.

```sh
toolbox run -c saffron-build bash -lc '
  export PATH="/var/home/saffronjam/.bun/bin:$PATH"
  cd /var/home/saffronjam/repos/SaffronEngine/editor
  bun install        # first time / after dependency changes
  bun run check      # generate protocol types + typecheck
  bun run tauri dev  # spawns the engine host + opens the editor'
```

`bun run check` regenerates `editor/src/protocol/` from the [control schemas](../../explanations/tooling-and-control/shared-types/) and typechecks. The dev launch needs an X11/XWayland display — the reparented child is Xlib-only — so it can't run under the toolbox's headless Wayland compositor; use a real desktop session.

## Verify

- **Engine host alone**: the window opens and presents the scene; drive it with the `se` CLI over its control socket.
- **Tauri editor**: the shell opens with the Hierarchy / tabbed Inspector·Environment·Stats / Assets / Viewport dock; a "Preparing renderer…" overlay clears once the embedded scene attaches.
- For a headless engine check, bound the host run and dump the offscreen image:
  ```sh
  SAFFRON_EXIT_AFTER_FRAMES=5 SAFFRON_CAPTURE=/tmp/frame.png ./build/debug/bin/SaffronEditor
  ```
  `SAFFRON_EXIT_AFTER_FRAMES=N` exits after `N` frames; `SAFFRON_CAPTURE=path` writes the viewport image at exit.

> [!NOTE]
> The Tauri editor is at parity with the old ImGui editor, not beyond it. Undo/redo, scene-graph parenting, multi-viewport, and native Wayland are non-goals — the old editor lacked them too.

## In the code

| What | File | Symbols |
|---|---|---|
| Toolbox + preset + run | `AGENTS.md` | the `saffron-build` recipe, `cmake --preset debug` |
| The loop + frame limit | `app.cppm` | `run`, `detail::frameLimitFromEnv` |
| Capture on exit | `app.cppm` | `SAFFRON_CAPTURE` → `captureViewport` |
| Debug preset | `CMakePresets.json` | `debug` (clang++, libc++, lld, Ninja) |
| Frontend scripts | `editor/package.json` | `dev`, `check`, `tauri:dev`, `gen:protocol` |

## Related

- [Tauri editor and the X11 bridge](../../explanations/ui-and-editor/tauri-editor-and-x11-bridge/) — how the editor drives the host
- [Main loop](../../explanations/app-lifecycle-and-window/main-loop-and-run/)
- [Headless runs and capture](../../explanations/app-lifecycle-and-window/headless-and-capture/)
