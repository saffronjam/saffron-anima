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

# Recursive make, aliased so it is NOT force-run under `make -n` (a recipe line with the
# literal $(MAKE) is always executed even in dry-run; via $(MK) it stays a real preview).
MK := $(MAKE)

# Tracked C++ sources we own; excludes the cmake third-party impl TUs and vendored code.
CPP_LS := git -C "$(REPO)" ls-files '*.cppm' '*.cpp' | grep -vE '^(cmake/|third_party/)'

# Targets whose tools (clang, cargo, Vulkan, slang, the toolbox-linked engine binary)
# live only in the toolbox. On the host these re-enter it; inside, they run directly.
TOOLBOX_TARGETS := check engine editor schema e2e run run-engine format lint prepare-for-commit

.DEFAULT_GOAL := help
.PHONY: help check engine editor schema e2e run run-engine run-docs format lint prepare-for-commit

## help: list the available targets (default; runs on the host, no toolbox)
help:
	@echo 'SaffronEngine convenience targets. Toolbox-bound targets auto-enter the'
	@echo 'saffron-build toolbox; run them from the host or from inside it.'
	@echo
	@echo 'Build & verify:'
	@echo '  make check              - full reproducible gate (tools/ci/check.sh)'
	@echo '  make engine             - cmake configure + build the engine binary (-j1)'
	@echo '  make editor             - build the TypeScript/Tauri frontend (bun run build)'
	@echo '  make schema             - control-schema contract test (live se vs schemas/control)'
	@echo '  make e2e                - end-to-end control-plane/rendering tests (bun test, headless)'
	@echo
	@echo 'Run:'
	@echo '  make run                - start the Tauri editor (it spawns the engine host)'
	@echo '  make run-engine         - start only the engine host (present-only, for debugging)'
	@echo '  make run-docs           - serve the Hugo docs site locally (http://localhost:1313/saffron-engine/)'
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

## engine: configure + build the C++26 engine binary SaffronEngine (-j1 avoids a clang module-BMI ICE)
engine:
	cmake --preset debug
	cmake --build "$(BUILD_DIR)" -j1

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
	cd "$(EDITOR)" && bun run tauri dev

## run-engine: start only the present-only engine host (control plane + viewport, no editor)
run-engine:
	@test -x "$(ENGINE_BIN)" || { echo "engine not built — run 'make engine' first"; exit 1; }
	"$(ENGINE_BIN)"

## format: clang-format the C++ in place + oxfmt the editor TypeScript
format:
	cd "$(REPO)" && $(CPP_LS) | xargs -r clang-format -i
	cd "$(EDITOR)" && bun run format

## lint: C++ format check + clang-tidy (needs build/debug) + oxlint on the editor TypeScript
lint:
	@test -f "$(BUILD_DIR)/compile_commands.json" || { echo "build/debug not configured — run 'make engine' first (clang-tidy needs compile_commands.json)"; exit 1; }
	cd "$(REPO)" && $(CPP_LS) | xargs -r clang-format --dry-run -Werror
	run-clang-tidy -p "$(BUILD_DIR)" -quiet engine/source tools/se/source
	cd "$(EDITOR)" && bun run lint

## prepare-for-commit: format everything, then run the linters
prepare-for-commit:
	$(MK) format
	$(MK) lint

endif
