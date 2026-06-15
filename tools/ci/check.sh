#!/usr/bin/env bash
# Reproducible verification gates for the Saffron Anima + Tauri editor.
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
# Gates: engine build (-j1), present-only host smoke, DTO contract test, frontend build.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"
fail=0
step() { echo; echo "=== $* ==="; }

step "engine build (toolbox, -j1)"
(
  bun run tools/gen-control-dto/gen.ts &&
  git diff --exit-code -- \
    engine/source/saffron/control/control_dto_serde.generated.cpp \
    engine/source/saffron/scene/scene_component_serde.generated.cpp \
    engine/source/saffron/assets/script_component_defs.generated.hpp \
    editor/src/protocol/sa-types.ts \
    schemas/control/openrpc.generated.json \
    schemas/control/command-manifest.generated.json
) || fail=1
cmake --preset debug && cmake --build build/debug -j1 || fail=1

step "engine present-only smoke (bounded, headless)"
(
  export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
  cd /tmp && rm -f project.json
  SAFFRON_EXIT_AFTER_FRAMES=5 SAFFRON_CONTROL_SOCK=/tmp/sa-ci.sock "$REPO/build/debug/bin/SaffronAnima"
) || fail=1

step "control DTO contract test (live help/results vs generated manifest/OpenRPC)"
( cd "$REPO/tools/check-control-schema" && bun run check.ts ) || fail=1

step "script-API def drift (live Lua bindings vs library/sa.lua)"
( bun "$REPO/tools/check-script-defs/check.ts" ) || fail=1

step "project startup and asset layout smoke"
( "$REPO/tools/check-projects/check.sh" ) || fail=1

step "frontend: gen @saffron/protocol + tsc --noEmit + vite build + unit tests"
( cd "$REPO/editor" && bun run build && bun test ) || fail=1

echo
if [ "$fail" -eq 0 ]; then echo "ALL GATES PASSED"; else echo "SOME GATES FAILED"; fi
exit "$fail"
