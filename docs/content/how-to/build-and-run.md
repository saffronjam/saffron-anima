+++
title = 'Build and run'
weight = 1
math = false
+++

# Build and run

Configure, build, and run `SaffronEditor` from the `saffron-build` toolbox. (The Silverblue host has no C++ toolchain; the home directory is shared into the container.)

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
3. Run the editor:
   ```sh
   toolbox run -c saffron-build bash -lc '
     cd /var/home/saffronjam/repos/SaffronEngine
     ./build/debug/bin/SaffronEditor'
   ```

## Verify

- The window opens with the docked Hierarchy / Inspector / Assets / Viewport layout.
- For a headless check, bound the run and dump the offscreen image:
  ```sh
  SAFFRON_EXIT_AFTER_FRAMES=5 SAFFRON_CAPTURE=/tmp/frame.png ./build/debug/bin/SaffronEditor
  ```
  `SAFFRON_EXIT_AFTER_FRAMES=N` exits after `N` frames; `SAFFRON_CAPTURE=path` writes the viewport image at exit.

## In the code

| What | File | Symbols |
|---|---|---|
| Toolbox + preset + run | `AGENTS.md` | the `saffron-build` recipe, `cmake --preset debug` |
| The loop + frame limit | `app.cppm` | `run`, `detail::frameLimitFromEnv` |
| Capture on exit | `app.cppm` | `SAFFRON_CAPTURE` → `captureViewport` |
| Debug preset | `CMakePresets.json` | `debug` (clang++, libc++, lld, Ninja) |

## Related

- [Main loop](../../explanations/app-lifecycle-and-window/main-loop-and-run/)
- [Headless runs and capture](../../explanations/app-lifecycle-and-window/headless-and-capture/)
