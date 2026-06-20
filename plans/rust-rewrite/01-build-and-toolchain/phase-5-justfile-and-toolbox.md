# Phase 5 — `justfile` carrying the Makefile + toolbox lore

**Status:** COMPLETED
**Depends on:** phase-2-profiles-and-workspace-build, phase-3-xtask-shader-pipeline

## Goal

Replace the `Makefile` with a `justfile` at the repo root that carries its environment lore verbatim —
the NVIDIA ICD `VK_ADD_DRIVER_FILES` knob, `WEBVIEW_HW`, the host-runnable `sa`, the headless weston
run env, and the toolbox auto-enter — over `cargo`/`xtask`/`bun` recipes instead of CMake. After this
phase a developer runs `just engine`, `just run`, `just lint`, etc. from the host or inside the toolbox
with the same behavior the Makefile gave.

## Why this shape (NO LEGACY)

The `Makefile`'s build commands are deleted (Cargo replaces them); its **lore** is the asset worth
keeping — the hard-won environment knobs that make the engine actually run on this machine. The
feasibility study is explicit: "Carry the Makefile/toolbox environment lore (NVIDIA ICD
`VK_ADD_DRIVER_FILES`, `WEBVIEW_HW`, host-runnable `sa`) into a justfile verbatim." `just` is chosen
over keeping `make` because the build recipes are entirely new (`cargo`, not `cmake`), so there is no
reason to retain `make` syntax; `just` is the idiomatic Rust-project task runner and reads cleaner.
There is exactly one task runner; the `Makefile` is deleted at cutover (it still serves the C++ build
until then — see below), not kept as a second entry point.

The toolbox boundary rule is preserved exactly: env vars are set **inside** the recipe, never as a
host-side prefix (a host-side `ENV=… just …` would no-op across the `toolbox run` boundary, the same
trap the Makefile documents). The `flock` guard on `make engine` is **dropped**: it existed solely for
the two-ninja `.pcm` race, which has no Cargo equivalent (Cargo's build graph never shares mutable BMI
files between processes). One fewer guard.

The host-runnable `sa` *simplifies*: the C++ `sa` needed `-stdlib=libstdc++` to avoid linking
toolbox-only libc++ on the bare host (`tools/sa/CMakeLists.txt`). The Rust `sa` depends only on
`saffron-protocol` (no engine, no libc++) and runs host-side with no stdlib ceremony — the `justfile`
just records that `sa` is built and host-runnable.

**Transition note (NO LEGACY, honestly).** While `engine-old/` still ships, the C++ build needs its
CMake recipes. Rather than keep two runners, the `justfile`'s `engine` recipe builds the **Rust**
workspace (the thing under active development); the C++ reference build keeps using the
already-repointed `Makefile`/CMake from phase 1 until cutover deletes both `engine-old/` and the
`Makefile`. The `justfile` is the Rust front door; the `Makefile` is the soon-to-be-deleted C++ one.
This is not a duplicate path for the *same* artifact — they build different binaries — so it does not
violate one-code-path; it is the parallel-binary strategy made literal, and it collapses to a single
runner at cutover.

## Grounding (real files/symbols)

- `Makefile` — every knob to carry:
  - `NVIDIA_ICD` (`firstword $(wildcard /run/host/.../nvidia_icd.x86_64.json /usr/.../nvidia_icd...)`)
    and `GPU_ENV := $(if $(NVIDIA_ICD),VK_ADD_DRIVER_FILES=$(NVIDIA_ICD))` (lines 34–38) — *adds* the
    NVIDIA ICD, llvmpipe stays a fallback, empty on non-NVIDIA;
  - `WEBVIEW_HW ?= 1` / `WEBVIEW_ENV := $(if ...,SAFFRON_WEBVIEW_HW=1)` (lines 44–45);
  - the toolbox auto-enter: `ifeq ($(wildcard /run/.toolboxenv),)` → re-exec
    `toolbox run -c saffron-build bash -lc 'export PATH="$(BUN_BIN):$$PATH"; exec ...'` (lines 107–113);
  - `TOOLBOX_TARGETS` (line 69) — the set of recipes that need the toolbox toolchain;
  - the recipes `run`/`run-debug`/`run-engine`/`run-software`/`run-docs`/`format`/`lint`/
    `prepare-for-commit`/`check`/`engine`/`editor`/`schema`/`e2e` (lines 116–189) — mapped to `cargo`/
    `xtask`/`bun` per README §5;
  - the `engine:` `flock` guard (lines 126–129) — **dropped** (no two-ninja race in Cargo);
  - the `lint:` `run-clang-tidy -p "$(BUILD_DIR)" engine-old/source tools/sa/source` and
    `clang-format --dry-run -Werror` (lines 179–183) — **replaced** by `cargo clippy --workspace --
    -D warnings` + `cargo fmt --check` for the Rust side (the C++ lint stays in the `Makefile` until
    cutover); the editor `bun run lint` stays.
- `tools/sa/CMakeLists.txt` — `target_compile_options(sa PRIVATE -stdlib=libstdc++)` — the host-runnable
  override that the Rust `sa` deletes.
- `tools/ci/check.sh` (header, lines 7–12) — the weston headless run env
  (`weston --backend=headless --socket=wl-ci --idle-time=0`, `WAYLAND_DISPLAY=wl-ci
  SDL_VIDEODRIVER=wayland`, `XDG_RUNTIME_DIR`) — the `just`-recipe headless framing references it.
- `00-foundations` crate graph — `sa (bin) → protocol only` and `xtask (bin) → protocol, ...`; the
  `justfile` invokes `cargo run -p sa`, `cargo run -p xtask <task>`.

## Acceptance gate

- A root `justfile` exists with recipes `engine`, `editor`, `schema`, `e2e`, `run`, `run-engine`,
  `run-software`, `run-docs`, `format`, `lint`, `check` (README §5 table), each over `cargo`/`xtask`/
  `bun`.
- `just engine` (inside the toolbox) runs `cargo build --workspace` + `cargo run -p xtask shaders` and
  produces the Rust host binary + shaders next to it.
- `just lint` runs `cargo fmt --check` + `cargo clippy --workspace -- -D warnings` + the editor
  `bun run lint`, and passes on the workspace as built so far.
- `just format` runs `cargo fmt` + the editor `bun run format`.
- The NVIDIA-ICD and `WEBVIEW_HW` env are set **inside** the `run`/`run-engine` recipes (not as host
  prefixes); `just run-software` omits the ICD.
- Recipes that need the toolbox toolchain auto-enter `toolbox run -c saffron-build` when
  `/run/.toolboxenv` is absent (host invocation), and run directly when present — matching the
  Makefile's behavior.
- `just run-engine` launches the Rust present-only host; `just run` launches `bun run tauri dev` with
  `SAFFRON_ANIMA_BIN` still pointing at the C++ binary until cutover (the parallel-binary contract).
