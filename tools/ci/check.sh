#!/usr/bin/env bash
# Reproducible verification gates for the SaffronEngine + Tauri editor.
#
# Run inside the saffron-build toolbox with the host bun on PATH, under a display
# (the engine smoke + schema contract test open a Vulkan swapchain → need one):
#
#   toolbox run -c saffron-build bash -lc '
#     export PATH="/var/home/saffronjam/.bun/bin:$PATH" XDG_RUNTIME_DIR=/run/user/$(id -u)
#     weston --backend=headless --width=1280 --height=720 --socket=wl-ci --idle-time=0 &
#     sleep 2; export WAYLAND_DISPLAY=wl-ci SDL_VIDEODRIVER=wayland
#     tools/ci/check.sh
#   '
#
# Gates: engine build (-j1), present-only host smoke, schema contract test, frontend build.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"
fail=0
step() { echo; echo "=== $* ==="; }

step "engine build (toolbox, -j1)"
cmake --preset debug && cmake --build build/debug -j1 || fail=1

step "engine present-only smoke (bounded, headless)"
(
  export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
  cd /tmp && rm -f project.json
  SAFFRON_EXIT_AFTER_FRAMES=5 SAFFRON_CONTROL_SOCK=/tmp/se-ci.sock "$REPO/build/debug/bin/SaffronEditor"
) || fail=1

step "control schema contract test (live se output vs schemas/control)"
( cd "$REPO/tools/check-control-schema" && bun run check.ts ) || fail=1

step "frontend: gen @saffron/protocol + tsc --noEmit + vite build"
( cd "$REPO/editor" && bun run build ) || fail=1

echo
if [ "$fail" -eq 0 ]; then echo "ALL GATES PASSED"; else echo "SOME GATES FAILED"; fi
exit "$fail"
