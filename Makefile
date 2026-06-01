# SaffronEngine convenience gate.
#
# These targets are THIN WRAPPERS around the existing scripts. They do NOT set up
# the build environment — they assume you are already inside it. SaffronEngine only
# builds on the local Fedora Silverblue host inside the `saffron-build` toolbox; a
# stock CI runner cannot build it (immutable OS, clang 21 + libc++ `import std`,
# Vulkan/SDL3/slang, weston for a headless display). See AGENTS.md and tools/ci/README.md.
#
# PREREQUISITES (the wrappers do NOT provide these — you must):
#   1. Run inside the toolbox:    toolbox run -c saffron-build bash -lc '<make ...>'
#   2. Host bun on PATH:          export PATH="/var/home/saffronjam/.bun/bin:$PATH"
#   3. For check/engine/schema, a display (the engine smoke + schema contract test
#      open a Vulkan swapchain). Start a headless compositor and point SDL at it:
#        export XDG_RUNTIME_DIR=/run/user/$(id -u)
#        weston --backend=headless --width=1280 --height=720 --socket=wl-ci --idle-time=0 &
#        sleep 2; export WAYLAND_DISPLAY=wl-ci SDL_VIDEODRIVER=wayland
#
# One-liner that satisfies all of the above and runs the full gate:
#   toolbox run -c saffron-build bash -lc '
#     export PATH="/var/home/saffronjam/.bun/bin:$PATH" XDG_RUNTIME_DIR=/run/user/$(id -u)
#     weston --backend=headless --width=1280 --height=720 --socket=wl-ci --idle-time=0 &
#     sleep 2; export WAYLAND_DISPLAY=wl-ci SDL_VIDEODRIVER=wayland
#     make check'

REPO := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
BUILD_DIR := $(REPO)build/debug

.DEFAULT_GOAL := help

.PHONY: help check engine editor schema

## help: list the available targets (default)
help:
	@echo 'SaffronEngine convenience gate (run inside the saffron-build toolbox).'
	@echo
	@echo 'Targets:'
	@echo '  make help     - show this help (default)'
	@echo '  make check    - run the full reproducible gate (tools/ci/check.sh)'
	@echo '  make engine   - cmake configure + build the engine/editor (-j1)'
	@echo '  make editor   - build the TypeScript/Tauri frontend (bun run build)'
	@echo '  make schema   - run the control-schema contract test'
	@echo
	@echo 'Prerequisites (see comments at the top of this Makefile and tools/ci/README.md):'
	@echo '  - run inside: toolbox run -c saffron-build bash -lc "..."'
	@echo '  - host bun on PATH; for check/engine/schema a headless weston display.'

## check: full reproducible gate — engine build, headless smoke, schema test, frontend build
check:
	"$(REPO)tools/ci/check.sh"

## engine: configure + build the C++26 engine and editor binary (-j1 avoids a clang module-BMI ICE)
engine:
	cmake --preset debug
	cmake --build "$(BUILD_DIR)" -j1

## editor: gen @saffron/protocol + tsc + vite build of the frontend
editor:
	cd "$(REPO)editor" && bun run build

## schema: contract test — live `se` control output vs schemas/control
schema:
	cd "$(REPO)tools/check-control-schema" && bun run check.ts
