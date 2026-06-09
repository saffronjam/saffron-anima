# SaffronEngine convenience targets.
#
# These targets are THIN WRAPPERS around the existing scripts/tools. The toolbox-bound
# ones (everything except help/run-docs) AUTO-ENTER the `saffron-build` toolbox when run
# from the host, so `make lint` works the same from a host shell or from inside the
# toolbox. SaffronEngine only builds there (immutable host OS, clang 21 + libc++
# `import std`, Vulkan/SDL3/slang); see AGENTS.md and tools/ci/README.md.
#
# Run from the host:   make lint        (re-enters the toolbox for you)
# Already inside it:    make lint        (runs directly)
#
# What you still need:
#   1. The `saffron-build` toolbox to exist, and host bun at $(BUN_BIN) (added to PATH
#      automatically when re-entering). 2. For run/run-engine/check/schema a display —
#      your desktop session's DISPLAY/WAYLAND_DISPLAY pass through into the toolbox.
#      Headless instead: weston --backend=headless --socket=wl-ci & ; then export
#      WAYLAND_DISPLAY=wl-ci SDL_VIDEODRIVER=wayland before `make`.

REPO := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
BUILD_DIR := $(REPO)build/debug
ENGINE_BIN := $(BUILD_DIR)/bin/SaffronEngine
EDITOR := $(REPO)editor
DOCS := $(REPO)docs
TOOLBOX := saffron-build
BUN_BIN := /var/home/saffronjam/.bun/bin

# NVIDIA Vulkan ICD for interactive runs. The toolbox carries the NVIDIA GL/EGL userspace
# (and a working nvidia-smi) but not the Vulkan ICD manifest, so the engine otherwise falls
# back to llvmpipe and renders the whole PBR pipeline in software — pegging the CPU and
# starving the editor's webview. Point the loader at the host manifest (it names
# libGLX_nvidia.so.0, which IS present, and matches the loaded 580.x kernel module). Resolves
# to the host copy inside the toolbox, the local copy on a bare host, and empty when neither
# exists (no NVIDIA / other machine) — in which case runs keep their software behaviour.
NVIDIA_ICD := $(firstword $(wildcard /run/host/usr/share/vulkan/icd.d/nvidia_icd.x86_64.json /usr/share/vulkan/icd.d/nvidia_icd.x86_64.json))
# VK_ADD_DRIVER_FILES *adds* the NVIDIA ICD to the default search rather than replacing it, so
# llvmpipe stays available as a fallback — vk-bootstrap prefers the discrete GPU when it can
# present (the real X11 session) and falls back to software if it can't (e.g. headless).
GPU_ENV := $(if $(NVIDIA_ICD),VK_ADD_DRIVER_FILES=$(NVIDIA_ICD))

# Webview render path on NVIDIA. 1 (default) = the hardware GL path (GPU-composited UI;
# lib.rs auto-sets __NV_DISABLE_EXPLICIT_SYNC=1 so it doesn't hit the wp_linux_drm_syncobj
# crash). 0/empty = the safe software (Mesa llvmpipe) path. Set inside the recipe so it
# survives the toolbox boundary; override with `make run WEBVIEW_HW=0` (or `make run-software`).
WEBVIEW_HW ?= 1
WEBVIEW_ENV := $(if $(filter-out 0,$(WEBVIEW_HW)),SAFFRON_WEBVIEW_HW=1)

# Recursive make, aliased so it is NOT force-run under `make -n` (a recipe line with the
# literal $(MAKE) is always executed even in dry-run; via $(MK) it stays a real preview).
MK := $(MAKE)

# Tracked C++ sources we own; excludes the cmake third-party impl TUs, vendored code, and
# generated serde (gen-control-dto owns its style — formatting it just fights the generator).
CPP_LS := git -C "$(REPO)" ls-files '*.cppm' '*.cpp' | grep -vE '^(cmake/|third_party/)|\.generated\.cpp$$'

# clang-tidy parallelism. The default (one per core) re-parses the heavy module/Vulkan
# headers in every process at a few GB each, which OOMs a 32 GB machine at -j24.
TIDY_JOBS ?= 4

# Engine build parallelism. A single `ninja -jN` serializes module producers before their
# consumers via dyndep, so its own .pcm files never race — verified clean across 7 full
# module-DAG rebuilds at -j8 (clean build ~1m35s vs ~8min at -j1). The Bus-error/ICE the
# old -j1 default guarded against is actually the TWO-ninja-in-one-dir hazard (each rewrites
# the other's mmap'd BMIs); see the concurrent-builds rule in AGENTS.md. So: one build at a
# time per dir, parallel within it. Drop to ENGINE_JOBS=1 only if a future clang regresses.
ENGINE_JOBS ?= 8

# Targets whose tools (clang, cargo, Vulkan, slang, the toolbox-linked engine binary)
# live only in the toolbox. On the host these re-enter it; inside, they run directly.
TOOLBOX_TARGETS := check engine editor schema e2e run run-debug run-engine run-software run-wl-debug format lint prepare-for-commit

.DEFAULT_GOAL := help
.PHONY: help check engine editor schema e2e run run-debug run-engine run-software run-wl-debug run-docs format lint prepare-for-commit

## help: list the available targets (default; runs on the host, no toolbox)
help:
	@echo 'SaffronEngine convenience targets. Toolbox-bound targets auto-enter the'
	@echo 'saffron-build toolbox; run them from the host or from inside it.'
	@echo
	@echo 'Build & verify:'
	@echo '  make check              - full reproducible gate (tools/ci/check.sh)'
	@echo '  make engine             - cmake configure + build the engine binary (-j$(ENGINE_JOBS); override ENGINE_JOBS)'
	@echo '  make editor             - build the TypeScript/Tauri frontend (bun run build)'
	@echo '  make schema             - control-schema contract test (live se vs schemas/control)'
	@echo '  make e2e                - end-to-end control-plane/rendering tests (bun test, headless)'
	@echo
	@echo 'Run:'
	@echo '  make run                - start the Tauri editor (it spawns the engine host)'
	@echo '  make run-debug          - start the editor with in-editor developer mode pre-enabled'
	@echo '  make run-engine         - start only the engine host (present-only, for debugging)'
	@echo '  make run-software       - run the editor forcing the llvmpipe software GPU (control case)'
	@echo '  make run-docs           - serve the Hugo docs site locally (http://localhost:1313/saffron-engine/)'
	@echo
	@echo 'run / run-engine use the real NVIDIA GPU when its Vulkan ICD is found; run-software forces llvmpipe.'
	@echo
	@echo 'Quality:'
	@echo '  make format             - clang-format the C++ + oxfmt the editor TypeScript'
	@echo '  make lint               - clang-format check + clang-tidy + oxlint'
	@echo '  make prepare-for-commit - format, then lint'
	@echo
	@echo 'Needs the saffron-build toolbox + host bun; lint needs a configured build/debug.'

## run-docs: init the theme submodule and serve the docs site (host hugo + git)
run-docs:
	git -C "$(REPO)" submodule update --init --depth 1 docs/themes/hugo-book
	cd "$(DOCS)" && hugo server

ifeq ($(wildcard /run/.toolboxenv),)

# On the host: re-run the requested target inside the toolbox with host bun on PATH.
$(TOOLBOX_TARGETS):
	@command -v toolbox >/dev/null || { echo "toolbox not found — install it, or run inside the saffron-build container"; exit 1; }
	toolbox run -c $(TOOLBOX) bash -lc 'export PATH="$(BUN_BIN):$$PATH"; exec "$(MK)" -C "$(REPO)" $@'

else

## check: full reproducible gate — engine build, headless smoke, schema test, frontend build
check:
	"$(REPO)tools/ci/check.sh"

## engine: configure + build the C++26 engine binary SaffronEngine (parallel; override with ENGINE_JOBS=1)
# A flock serializes concurrent `make engine` on the same BUILD_DIR. Parallel compilation
# WITHIN one ninja is safe; what corrupts the module .pcm files (Bus error + a trashed
# .ninja_log) is TWO ninja processes writing one dir at once. The lock makes a second build
# WAIT for the first instead of racing it — the rule from AGENTS.md, now enforced not advised.
# It is per-BUILD_DIR, so private dirs (cmake --preset debug -B build/<name>) never contend.
engine:
	@mkdir -p "$(BUILD_DIR)"
	@flock -n "$(BUILD_DIR)/.build.lock" true 2>/dev/null || echo '==> another build holds $(BUILD_DIR); waiting for it to finish…'
	flock "$(BUILD_DIR)/.build.lock" sh -ec 'cmake --preset debug && cmake --build "$(BUILD_DIR)" -j$(ENGINE_JOBS)'

## editor: gen @saffron/protocol + tsc + vite build of the frontend
editor:
	cd "$(EDITOR)" && bun run build

## schema: contract test — live `se` control output vs schemas/control
schema:
	cd "$(REPO)tools/check-control-schema" && bun run check.ts

## e2e: end-to-end tests that drive a headless engine over the control plane (bun test)
e2e:
	cd "$(REPO)tests/e2e" && bun test

## run: start the Tauri editor, which spawns build/debug/bin/SaffronEngine as a native child
run:
	cd "$(EDITOR)" && $(GPU_ENV) $(WEBVIEW_ENV) bun run tauri dev

## run-debug: like run, but starts with the editor's developer mode pre-enabled
run-debug:
	cd "$(EDITOR)" && $(GPU_ENV) $(WEBVIEW_ENV) VITE_SAFFRON_DEV_MODE=1 bun run tauri dev

## run-software: run the editor on the llvmpipe software GPU (control case; ignores the NVIDIA ICD)
run-software:
	cd "$(EDITOR)" && bun run tauri dev

## run-wl-debug: run the editor with Wayland protocol tracing; writes full log to /tmp/wl-debug.log
## then prints the 60 lines around the first protocol error. Ctrl-C after the crash appears.
run-wl-debug:
	cd "$(EDITOR)" && $(GPU_ENV) WAYLAND_DEBUG=1 bun run tauri dev > /tmp/wl-debug.log 2>&1 & \
	  PID=$$! ; \
	  tail -f /tmp/wl-debug.log & TAILPID=$$! ; \
	  wait $$PID ; \
	  kill $$TAILPID 2>/dev/null ; \
	  echo '--- grep for error context ---' ; \
	  grep -n "Error 71\|protocol error\|wl_display.*error\|GDK\|fatal" /tmp/wl-debug.log | head -20 ; \
	  echo '--- 30 Wayland lines before first error ---' ; \
	  grep -n "" /tmp/wl-debug.log | grep -B30 "Error 71\|protocol error" | head -40

## run-engine: start only the present-only engine host (control plane + viewport, no editor)
run-engine:
	@test -x "$(ENGINE_BIN)" || { echo "engine not built — run 'make engine' first"; exit 1; }
	$(GPU_ENV) "$(ENGINE_BIN)"

## format: clang-format the C++ in place + oxfmt the editor TypeScript
format:
	cd "$(REPO)" && $(CPP_LS) | xargs -r clang-format -i
	cd "$(EDITOR)" && bun run format

## lint: C++ format check + clang-tidy (needs build/debug) + oxlint on the editor TypeScript
lint:
	@test -f "$(BUILD_DIR)/compile_commands.json" || { echo "build/debug not configured — run 'make engine' first (clang-tidy needs compile_commands.json)"; exit 1; }
	cd "$(REPO)" && $(CPP_LS) | xargs -r clang-format --dry-run -Werror
	run-clang-tidy -p "$(BUILD_DIR)" -quiet -j $(TIDY_JOBS) engine/source tools/se/source
	cd "$(EDITOR)" && bun run lint

## prepare-for-commit: format everything, then run the linters
prepare-for-commit:
	$(MK) format
	$(MK) lint

endif
