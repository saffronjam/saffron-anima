# 01 — Build & toolchain: Cargo workspace, shader pipeline, FFI build, the gate

This area replaces the entire CMake + FetchContent + CMakePresets + C++26-modules + `import std` +
BMI-matching + two-ninja-`.pcm`-race apparatus with a Cargo workspace, and re-establishes the parts of
the build that Cargo does **not** subsume: the `slangc` shader fan-out, the determinism-flagged
vendored-Jolt FFI build, the toolbox-environment lore (NVIDIA ICD, `WEBVIEW_HW`, host-runnable `sa`,
headless weston), and the reproducible verification gate. It also handles the mechanical fallout of the
`engine/` → `engine-old/` relocation: the C++ build references must repoint at `engine-old/` so the C++
`SaffronAnima` stays buildable until cutover.

Deleting CMake is, per the feasibility study, "the most unambiguous win of the whole effort." This area
is where that deletion is designed concretely, and where the residue that *cannot* be deleted (Slang,
the Jolt determinism flags, the toolbox container) is carried forward verbatim so nothing silently
regresses.

## 0. Scope boundary with 00-foundations

`00-foundations` owns the **shape** of `engine/Cargo.toml` — the virtual workspace, `members`,
`[workspace.package]`, `[workspace.dependencies]` pin table, and `[workspace.lints]`. It explicitly
*reserves* the `[profile.*]` section for this area (00 README §2.3). So:

- **00 writes** the workspace skeleton (phase `00:phase-1-workspace-scaffold`) and the empty member
  crates that compile.
- **01 writes** the `[profile.*]` content, the `build.rs`/`xtask` shader pipeline, the
  `saffron-physics-sys` `build.rs` determinism build, the `justfile`, and the reproducible-gate
  scripts. Where this area needs a workspace-table entry (e.g. the `xtask` member, a `[profile.*]`
  block), it edits the file 00 created; it does not re-author the whole file.

This area does **not** decide the dependency pin *versions* (that is PP-2's `dependency-adoption.md`
annex, consumed here) nor the `*-sys`/safe-wrapper *internal* design (that is PP-11 / `05-physics-jolt-bridge`).
It owns how those builds are *driven* and *flagged*.

## 1. What Cargo deletes (the headline simplification)

The C++ build carries machinery that exists only to make C++26 named modules + `import std` work under
clang. Every item below has **no Rust equivalent** — it is deleted outright, not ported:

| Deleted C++ machinery | Where it lives | Why it vanishes |
|------------------------|----------------|-----------------|
| The experimental `import std` UUID gate | root `CMakeLists.txt` `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD` | Rust's std is always available; no gate |
| `CXX_MODULE_STD ON` per target | `engine-old/CMakeLists.txt` | no module-std concept in Cargo |
| gnu++26 BMI matching / `CMAKE_CXX_EXTENSIONS` discipline | root `CMakeLists.txt`, `CMakePresets.json` | crates have no BMI; `rlib` is the unit |
| The `FILE_SET CXX_MODULES` hand-ordered DAG | `engine-old/CMakeLists.txt` | crate `[dependencies]` declare the DAG; Cargo orders the build |
| `FetchContent` of 8 vendored libs | `cmake/Dependencies.cmake` | `[workspace.dependencies]` + `cargo` registry |
| `CMakePresets.json` debug/release/clang-libcxx | `CMakePresets.json` | `[profile.dev]`/`[profile.release]` + the toolbox's pinned toolchain |
| The two-ninja `.pcm` Bus-error race + the `flock` guard | `Makefile` `engine:` | Cargo's build graph has no shared-`.pcm` hazard |
| `-pthread`/`-mavx2` isolation on `physics.cpp` to protect `std.pcm` | `engine-old/CMakeLists.txt`, `Dependencies.cmake` `SAFFRON_JOLT_COMPILE_OPTIONS` | the arch/FP flags are confined to `saffron-physics-sys`'s `build.rs`; there is no module-std BMI to protect |
| The header-only impl TUs (vma/cgltf/stb/tinyobjloader/nanosvg `*_impl.cpp`) | `cmake/*_impl.cpp` | pure-Rust crates (`image`, `gltf`, `tobj`, `resvg`) and the `ash`/allocator crate need no impl-define TU |
| `JSON_NOEXCEPTION` abort firewall | `Dependencies.cmake` | `serde_json` returns `Result` |

What does **not** vanish, and is therefore designed in this area's phases:

- **`slangc`** has no Cargo home — the 40-shader fan-out + the `lighting.slang` module precompile +
  staleness tracking are hand-ported into `xtask`/`build.rs` (`CompileShaders.cmake` is the spec).
- **Vendored Jolt 5.3.0** must be built from source with `JPH_CROSS_PLATFORM_DETERMINISTIC` + single
  precision + `-ffp-model=precise` + confined `-mavx2` — re-applied to *only* `saffron-physics-sys`'s
  TUs (`Dependencies.cmake` is the spec; PP-11 owns the bridge internals).
- **The toolbox container** stays the reproducibility boundary (Vulkan 1.4 SDK, SDL3/winit substrate,
  `slangc`, headless weston, no GitHub-hosted CI).
- **The Makefile environment lore** (NVIDIA ICD `VK_ADD_DRIVER_FILES`, `WEBVIEW_HW`, host-runnable
  `sa`, headless run env) moves into a `justfile` verbatim.

## 2. The `[profile.*]` design

The two CMake build types (`Debug`, `RelWithDebInfo`) map to two Cargo profiles. The pinned toolchain
(clang/libc++/lld) is the toolbox's, not a profile concern — Cargo profiles only carry optimization /
debug-info / overflow-check knobs.

- **`[profile.dev]`** is the `Debug` analogue: `opt-level = 0`, `debug = true`, `overflow-checks = true`.
  The one deviation the renderer/physics make practical: `[profile.dev.package."*"]` with
  `opt-level = 2` (or `3`) so the math-heavy dependency crates (`glam`, `ash` call sites' inlining,
  Jolt) are not unbearably slow in a debug engine build — this is the idiomatic Rust equivalent of
  "the third-party code is `-O2` even in Debug." (The exact level is a phase-2 measurement, not pinned
  here.)
- **`[profile.release]`** is the `RelWithDebInfo` analogue: `opt-level = 3`, `debug = true` (keep line
  tables — `RelWithDebInfo` does), `lto` and `codegen-units` left at defaults until a measured reason
  to change them; `panic = "unwind"` (the default — the shm/FFI seams must be able to unwind cleanly,
  and `abort` would defeat the `#[should_panic]` and FFI-unwind-guard tests).
- **Determinism note:** the physics determinism contract (PP-11) lives in `saffron-physics-sys`'s
  `build.rs` C++ flags, **not** in the Rust profile. Rust `f32`/`f64` arithmetic in `saffron-physics`
  the safe wrapper must avoid `opt-level`-dependent reassociation; this is a code constraint (no
  `fast-math` equivalent is enabled by any profile — Rust has none by default, which is the desired
  state) recorded here and enforced by the determinism gate, not a profile knob.

## 3. The shader pipeline (`xtask` + `build.rs`)

`CompileShaders.cmake` is the exact spec. It does three things this area reproduces:

1. **The fan-out:** every `*.slang` in `assets/shaders/` → `<out>/<name>.spv` via
   `slangc <src> -profile glsl_450 -target spirv -emit-spirv-directly -fvk-use-entrypoint-name
   -matrix-layout-column-major -I <shader_dir> -o <out>` (all `[shader(...)]` entry points in one
   module). 40 shaders today (`ls assets/shaders/*.slang` = 40), `lighting.slang` excluded from the
   fan-out.
2. **The `lighting.slang` module precompile:** `slangc lighting.slang -emit-ir -o
   <out>/lighting.slang-module` (no entry points → no `.spv`). `mesh.slang` `import lighting`, and the
   runtime node-graph codegen links the precompiled module instead of recompiling the übershader. The
   *module*, not the source, ships to `<out>`.
3. **The source-copy:** each `.slang` is copied next to its `.spv` so the runtime material-variant
   codegen can splice `mesh.slang`. (`copy_if_different` in CMake.)

**Mechanism decision (the load-bearing one).** Shader compilation is driven by **`xtask`**, not a
per-crate `build.rs`, with a thin `build.rs` hook only where a crate needs the artifacts present for
its own tests. Rationale:

- The shaders are a **workspace-level** artifact consumed at *runtime* (the engine loads `.spv` from a
  directory next to the binary via `assetPath`), not linked into any crate. A `build.rs` is the wrong
  tool: it would re-run per-crate, has no clean way to emit to a shared runtime dir, and couples shader
  staleness to a crate's rebuild. `xtask shaders` is invoked explicitly (and by the `justfile` build
  recipe + the gate), mirroring how CMake's `saffron_compile_shaders` is a custom target the engine
  *depends on* but is not a compile input.
- **Staleness tracking** is re-implemented in `xtask`: compare each `.slang` source mtime (and the
  `-I` include set — `lighting.slang` is a dependency of every fan-out shader, exactly as
  `DEPENDS ${shader} ${lighting_src}` declares in CMake) against its `.spv` mtime, skipping unchanged
  outputs. The `lighting.slang-module` is rebuilt when `lighting.slang` changes and *forces* a full
  fan-out rebuild (every shader `import`s it). This is the `add_custom_command` `DEPENDS` graph,
  hand-rolled.
- `slangc` is invoked via `std::process::Command` (the feasibility study notes this is *safer* than
  the C++ hand-quoted shell string). The binary is located the same way `Slang.cmake` does: prefer
  `SAFFRON_SLANGC` / `SAFFRON_SLANG_DIR` / `PATH`, else the toolbox provides it (the prebuilt-fetch
  fallback in `Slang.cmake` is dropped — the toolbox always ships `slangc`; if it is missing that is a
  toolbox-provisioning failure, not something the build silently fetches).

The shader source dir and output dir: source stays at `engine/assets/shaders/` (moved with the
relocation — see §6; the assets are *not* C++ and belong with the Rust engine), output goes next to the
`saffron-host` binary (`target/<profile>/shaders/` or a path the host resolves via its asset-root
logic, matching the C++ `SAFFRON_RUNTIME_DIR/shaders`). `xtask` also copies `models/`, `fonts/`,
`icons/` next to the binary (the `POST_BUILD copy_directory` block in `engine-old/CMakeLists.txt`).

## 4. The Jolt FFI build (`saffron-physics-sys` `build.rs`)

This area owns *how the build is driven and flagged*; PP-11 / `05-physics-jolt-bridge` owns the bridge
C++ shim classes and the safe wrapper. The build contract, lifted from `Dependencies.cmake`:

- Vendored Jolt **5.3.0** compiled from source by `cc`/`cxx-build` inside `saffron-physics-sys`'s
  `build.rs` (not the published `joltc-sys`/`rolt` crates — they pin 5.0.0 and miss the advanced API).
- Flags applied to **only this crate's TUs** (the `*-sys`/safe split is what isolates them — the C++
  `SAFFRON_JOLT_COMPILE_OPTIONS` isolation to `physics.cpp` becomes a crate boundary):
  `JPH_CROSS_PLATFORM_DETERMINISTIC` (= `CROSS_PLATFORM_DETERMINISTIC ON`), single precision
  (`DOUBLE_PRECISION OFF`), `-ffp-model=precise` + `-ffp-contract=off` (the determinism FP pairing),
  and `-mavx2` confined to these TUs. `-Wno-error` for Jolt's own TUs (third-party code; CMake does
  the same: `target_compile_options(Jolt PRIVATE -Wno-error)`).
- The `-pthread` subtlety from CMake (it toggled the std-module POSIX-thread langopt, so it was dropped
  from the per-TU compile options and kept only at link) **disappears**: there is no `import std` BMI
  to protect. Jolt's `JobSystemThreadPool` links against the platform threads normally; `build.rs`
  emits the right link flags.
- This crate is one of the three that `#![allow(unsafe_code)]` (the FFI seam), per the foundations
  lints policy.

The phase here writes the `build.rs` build-driving spec (vendoring, flag application, link emission,
`OUT_DIR` handling) as a skeleton; PP-11 fills the shim TUs and the determinism gate.

## 5. The `justfile` (Makefile lore, carried verbatim)

The `Makefile`'s value is its environment lore, not its build commands (Cargo replaces those). A
`justfile` at the repo root carries it. The recipes map 1:1 from the Makefile targets, rewritten over
`cargo`:

| `just` recipe | Replaces Makefile target | Carries |
|---------------|--------------------------|---------|
| `engine` | `make engine` | `cargo build` (+ `xtask shaders`); **no `flock`** (Cargo has no two-ninja race) |
| `editor` | `make editor` | `cd editor && bun run build` (unchanged) |
| `schema` | `make schema` | control-schema contract test (now `xtask`-generated artifacts vs live `sa`) |
| `e2e` | `make e2e` | `cd tests/e2e && bun test` (unchanged wire; `SAFFRON_ANIMA_BIN` → Rust host) |
| `run` | `make run` | the NVIDIA `VK_ADD_DRIVER_FILES` ICD + `SAFFRON_WEBVIEW_HW` env, then `bun run tauri dev` |
| `run-engine` | `make run-engine` | the present-only Rust host with `GPU_ENV` |
| `run-software` | `make run-software` | force llvmpipe (no ICD) |
| `run-docs` | `make run-docs` | hugo (unchanged, host-side) |
| `format` | `make format` | `cargo fmt` + `cd editor && bun run format` (replaces clang-format) |
| `lint` | `make lint` | `cargo clippy --workspace -- -D warnings` + `cargo fmt --check` + `bun run lint` (replaces clang-format check + clang-tidy) |
| `check` | `make check` | the reproducible gate (`tools/ci/check.sh` rewrite) |

The load-bearing env knobs reproduced verbatim from the Makefile, **set inside the recipe** (the
toolbox-boundary rule from AGENTS.md — a host-side `ENV=… make` no-ops):

- **`NVIDIA_ICD` / `GPU_ENV`:** `VK_ADD_DRIVER_FILES=$(firstword <host icd> <local icd>)` — *adds* the
  NVIDIA ICD so llvmpipe stays a fallback; resolves empty on non-NVIDIA machines (Makefile lines
  34–38).
- **`WEBVIEW_HW` / `WEBVIEW_ENV`:** `SAFFRON_WEBVIEW_HW=1` default; `WEBVIEW_HW=0` forces the software
  webview path (Makefile lines 44–45).
- **Host-runnable `sa`:** the `sa` binary runs on the host outside the toolbox. In C++ this needed an
  explicit `-stdlib=libstdc++` override (`tools/sa/CMakeLists.txt`) so it didn't link toolbox-only
  libc++. In Rust this **simplifies away**: `sa` is a statically-linked-ish Rust binary depending only
  on `saffron-protocol` (no engine, no libc++), so it runs on the bare host with no stdlib-override
  ceremony. The `justfile` records that `sa` is built and runnable host-side; no special flags.
- **Headless run env:** the weston headless lore (`weston --backend=headless --socket=wl-x
  --idle-time=0`, then `WAYLAND_DISPLAY=wl-x SDL_VIDEODRIVER=wayland`, unique socket +
  `SAFFRON_CONTROL_SOCK` per run) is captured in a `just` recipe and the gate script
  (`tools/ci/check.sh` header).

The toolbox auto-enter behavior (the Makefile's `ifeq ($(wildcard /run/.toolboxenv),)` re-exec into
`toolbox run -c saffron-build`) is preserved: `just` recipes that need the toolbox toolchain detect
`/run/.toolboxenv` and re-enter, exactly as the Makefile does. `just` is itself a host tool (like
`make`); the toolbox provides `cargo`/`slangc`/Vulkan.

## 6. The `engine/` → `engine-old/` relocation fallout (keep C++ buildable until cutover)

The C++26 tree moved from `engine/` to `engine-old/`; `engine/` is now the Rust workspace. Per pre-plan
§0, the C++ engine **stays buildable until cutover** (the greenfield-parallel-binary strategy: the
editor keeps spawning the C++ `SaffronAnima` via `SAFFRON_ANIMA_BIN` until the Rust host passes the
full gate). So the C++ build references that pointed at `engine/` must repoint at `engine-old/`. The
broken references, all of which this area's relocation phase fixes:

| Reference | File | Fix |
|-----------|------|-----|
| `add_subdirectory(engine)` | root `CMakeLists.txt` | → `add_subdirectory(engine-old)` |
| The DTO/serde/scene/component generated paths | `tools/gen-control-dto/gen.ts` (`engine/source/saffron/...`) | → `engine-old/source/saffron/...` |
| The script-def drift sources | `tools/check-script-defs/check.ts` (`/engine/source/saffron/...`) | → `engine-old/source/...` |
| The generated-file diff guard | `tools/ci/check.sh` (the `git diff --exit-code` list) | → `engine-old/...` paths |
| The clang-tidy source roots | `Makefile` `lint:` (`engine/source tools/sa/source`) | → `engine-old/source` |
| Model-import smoke path | `tools/check-projects/check.sh` (`engine/assets/models/cube.gltf`) | → `engine-old/assets/...` |
| Assets the C++ engine copies (models/fonts/icons/shaders) | `engine-old/CMakeLists.txt` `POST_BUILD` (already relative to `CMAKE_CURRENT_SOURCE_DIR`) | already correct (they moved with the tree to `engine-old/assets/`) |

**Decision on the Rust engine's assets:** the runtime assets (`shaders/`, `models/`, `fonts/`, `icons/`)
the *Rust* engine needs are **copied into `engine/assets/`** (the Rust workspace root) as part of the
shader-pipeline phase, sourced from `engine-old/assets/`. They are runtime data, not C++ source; the
Rust `xtask shaders` reads from `engine/assets/shaders/`. The C++ `engine-old/assets/` stays untouched
until cutover deletes `engine-old/` (NO LEGACY). This is one copy at cutover-prep time, recorded so it
is not a silent duplication.

**This area does not park the C++ build** — it keeps it green via the repoint, because the cutover
strategy depends on the C++ binary running until the Rust one passes parity. The relocation phase's
acceptance gate is: the C++ `make engine` / `tools/ci/check.sh` C++ steps still pass against
`engine-old/`.

## 7. The reproducible gate (`tools/ci/check.sh` rewrite)

The gate is rebuilt around Cargo + `xtask`, keeping the toolbox/weston framing and every wire-contract
check. The C++ gate's seven steps map to:

| C++ gate step (`tools/ci/check.sh`) | Rust gate step |
|--------------------------------------|----------------|
| `gen.ts` + `git diff --exit-code` generated files | `cargo run -p xtask gen` (codegen) + `git diff --exit-code` the generated `@saffron/protocol`/OpenRPC/manifest |
| `cmake --preset debug && cmake --build -j1` | `cargo build --workspace` + `cargo run -p xtask shaders` |
| present-only smoke (`SAFFRON_EXIT_AFTER_FRAMES=5`) | the Rust `saffron-host` present-only smoke, same env vars (frozen contract) |
| control DTO contract test (`check-control-schema`) | unchanged (it drives live `sa` vs `schemas/control/`) |
| script-API def drift (`check-script-defs`) | **deleted** — the Luau defs are generated from the binding source (PP-8); replaced by an `xtask`-freshness check (generated `.luau` defs are up-to-date), or removed if PP-8 folds it into the gen-freshness diff |
| project startup / asset layout smoke (`check-projects`) | unchanged (drives the live engine + `sa` over the wire) |
| frontend `bun run build` + `bun test` | unchanged |
| — (new) | `cargo test --workspace` (the unit-test gate; the C++ tree had no equivalent — self-tests are deleted, real `#[test]`s run here) |
| — (new) | `cargo clippy --workspace -- -D warnings` + `cargo fmt --check` (the lint gate folded in) |

The `13-testing-and-verification` area owns the *test strategy*; this area owns the *gate orchestration*
(the script that runs the build + smoke + contract + frontend in order inside the toolbox under weston).
There is exactly one gate script; `just check` invokes it.

## 8. Grounding (What | File | Symbols)

| What | File | Symbols |
|------|------|---------|
| `import std` UUID gate, C++26 standard, runtime-dir, `add_subdirectory(engine)` | `CMakeLists.txt` | `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD`, `CMAKE_CXX_STANDARD 26`, `SAFFRON_RUNTIME_DIR`, `add_subdirectory(engine)` / `add_subdirectory(tools/sa)` |
| The clang/libc++/lld preset + debug/release build types | `CMakePresets.json` | `clang-libcxx`, `debug`, `release`, `-stdlib=libc++`, `-fuse-ld=lld` |
| FetchContent vendored deps + JSON_NOEXCEPTION + GLM depth + Lua build | `cmake/Dependencies.cmake` | `EnTT`/`glm`/`VulkanMemoryAllocator`/`vk-bootstrap`/`nlohmann_json`, `lua_static`, `LuaBridge3`, `JoltPhysics`, `saffron_third_party`, `JSON_NOEXCEPTION GLM_FORCE_DEPTH_ZERO_TO_ONE` |
| The Jolt determinism flags + arch-flag isolation | `cmake/Dependencies.cmake` | `CROSS_PLATFORM_DETERMINISTIC`, `DOUBLE_PRECISION OFF`, `OVERRIDE_CXX_FLAGS OFF`, `SAFFRON_JOLT_COMPILE_OPTIONS`, the `-pthread` removal note |
| The shader fan-out + lighting-module precompile + source-copy + staleness | `cmake/CompileShaders.cmake` | `saffron_compile_shaders`, `lighting.slang-module` (`-emit-ir`), the `slangc … -emit-spirv-directly` flags, `DEPENDS ${shader} ${lighting_src}`, `copy_if_different` |
| Locating `slangc` (prefer PATH/env, else fetch) | `cmake/Slang.cmake` | `SAFFRON_SLANGC`, `SAFFRON_SLANG_DIR`, `SAFFRON_SLANG_VERSION` (the fetch fallback is dropped) |
| The header-only impl TUs that Rust crates delete | `cmake/*_impl.cpp` | `vma_impl.cpp`, `cgltf_impl.cpp`, `stb_impl.cpp`, `tinyobjloader_impl.cpp`, `nanosvg_impl.cpp` |
| The C++ module DAG + `physics.cpp` arch-flag application + asset copy | `engine-old/CMakeLists.txt` | `FILE_SET CXX_MODULES`, `CXX_MODULE_STD ON`, `set_source_files_properties(... physics.cpp ... SAFFRON_JOLT_COMPILE_OPTIONS)`, the `POST_BUILD copy_directory` block |
| The C++ `sa` host-runnable libstdc++ override (which Rust deletes) | `tools/sa/CMakeLists.txt` | `target_compile_options(sa PRIVATE -stdlib=libstdc++)` |
| NVIDIA ICD, WEBVIEW_HW, toolbox auto-enter, flock, clang-tidy roots | `Makefile` | `NVIDIA_ICD`/`GPU_ENV` (`VK_ADD_DRIVER_FILES`), `WEBVIEW_HW`/`WEBVIEW_ENV`, `TOOLBOX_TARGETS`, the `ifeq /run/.toolboxenv` re-exec, the `engine:` flock, `run-clang-tidy -p "$(BUILD_DIR)" engine/source tools/sa/source` |
| The reproducible gate's seven steps + weston/headless header | `tools/ci/check.sh` | `gen.ts` + `git diff --exit-code`, `cmake --build -j1`, present-only smoke (`SAFFRON_EXIT_AFTER_FRAMES`), `check-control-schema`, `check-script-defs`, `check-projects`, frontend build; the weston headless run-comment |
| The codegen source paths to repoint | `tools/gen-control-dto/gen.ts` | `dtoFile`/`cppOut`/`sceneSerdeOut`/`componentDefsOut` (`engine/source/...`), `repoRoot` |
| The script-def + project-smoke + e2e paths to repoint | `tools/check-script-defs/check.ts`, `tools/check-projects/check.sh`, `tests/e2e/harness.ts` | `read("/engine/source/...")`, `ENGINE="$REPO/build/debug/bin/SaffronAnima"`, `SAFFRON_ANIMA_BIN` |
| The shm publish primitives (BGRA8 seqlock, runtime asset dir) | `engine-old/source/saffron/rendering/renderer_capture.cpp` | `shm_open`/`mmap`, the 32-byte header + ring, `seq` release-fence, `WL_SHM_FORMAT_XRGB8888` |

## 9. Phases in this area

| Phase | What it locks | Depends on |
|-------|---------------|------------|
| `phase-1-relocation-repoint` | repoint all C++ build refs to `engine-old/`; keep the C++ build green | — |
| `phase-2-profiles-and-workspace-build` | the `[profile.*]` content; `cargo build --workspace` produces artifacts | `00:phase-1-workspace-scaffold`, phase-1 |
| `phase-3-xtask-shader-pipeline` | the `xtask shaders` slangc fan-out + lighting-module + staleness + asset copy | phase-2 |
| `phase-4-physics-sys-build-driver` | the `saffron-physics-sys` `build.rs` determinism build skeleton | phase-2 |
| `phase-5-justfile-and-toolbox` | the `justfile` carrying the Makefile/toolbox lore | phase-2, phase-3 |
| `phase-6-reproducible-gate` | the `tools/ci/check.sh` rewrite over Cargo + `xtask` + the toolbox/weston framing | phase-3, phase-5 |

Phase 1 is sequenced first because it keeps the existing C++ build/tooling green through the rest of the
rewrite; the Rust-side phases (2–6) build on the workspace scaffold from `00-foundations`. PP-14
interleaves these into the global linear order: the foundations+build block (`00`+`01`) is first, since
nothing in any other area compiles until the workspace + shader/FFI build scripts exist.
