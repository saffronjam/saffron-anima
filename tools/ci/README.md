# CI / reproducible gate

`tools/ci/check.sh` is the single reproducible gate for the engine + Tauri editor.
It runs four steps and fails if any one fails:

1. **engine build** — `cmake --preset debug` + `cmake --build build/debug -j1`
   (`-j1` avoids an intermittent clang module-BMI ICE).
2. **present-only host smoke** — launches `SaffronEditor` bounded to 5 frames
   (`SAFFRON_EXIT_AFTER_FRAMES=5`); opens a real Vulkan swapchain.
3. **control schema contract test** — `tools/check-control-schema/check.ts`
   diffs live `se` control output against `schemas/control`.
4. **frontend build** — `editor/` `bun run build` (gen `@saffron/protocol` →
   `tsc` → `vite build`).

## Prerequisites

This builds **only** on the local Fedora Silverblue host inside the
`saffron-build` toolbox. You need, all at once:

- the toolbox (clang 21 + libc++ `import std`, Vulkan 1.4 headers/loader/
  validation/tools, SDL3, slang) — see `AGENTS.md`;
- the **host bun** on `PATH` (the frontend build);
- a **display** — steps 2 and 3 open a Vulkan swapchain, so run a headless
  weston compositor and point SDL at it.

## Local one-liner (the everyday gate)

```sh
toolbox run -c saffron-build bash -lc '
  export PATH="/var/home/saffronjam/.bun/bin:$PATH" XDG_RUNTIME_DIR=/run/user/$(id -u)
  weston --backend=headless --width=1280 --height=720 --socket=wl-ci --idle-time=0 &
  sleep 2; export WAYLAND_DISPLAY=wl-ci SDL_VIDEODRIVER=wayland
  tools/ci/check.sh'
```

Or, from inside an already-prepared toolbox shell (display + bun set up), use the
root `Makefile`: `make check` (also `make engine`, `make editor`, `make schema`,
`make help`). The Makefile targets are thin wrappers — they assume the
environment is already prepared (toolbox, bun, display); they do not set it up.

## Honest CI story

There is **no GitHub-hosted pipeline**, on purpose. A stock hosted runner
(`ubuntu-latest`) cannot build SaffronEngine: the toolchain lives in an
immutable-OS toolbox (clang 21 + libc++ `import std` C++26 modules), it needs the
Vulkan SDK + SDL3 + slang, and the smoke/schema steps need a headless GPU display.
Reproducing that with `apt install` is not feasible, and faking a green hosted run
would be dishonest.

`.github/workflows/ci.yml` is therefore configured `runs-on: [self-hosted,
saffron-build]` — it only runs on a **self-hosted runner** provisioned with the
`saffron-build` toolbox (or a container image that replicates it) plus a headless
weston. Until such a runner is registered, the workflow simply queues; the
Makefile / `check.sh` run locally is the gate that actually protects `main`.
