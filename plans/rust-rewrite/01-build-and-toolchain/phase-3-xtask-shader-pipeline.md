# Phase 3 — `xtask` shader pipeline (slangc fan-out + lighting module + staleness)

**Status:** COMPLETED
**Depends on:** phase-2-profiles-and-workspace-build

## Goal

Reproduce `CompileShaders.cmake` as an `xtask shaders` subcommand: the 40-shader `slangc` fan-out, the
`lighting.slang` reusable-module precompile, the `.slang` source-copy next to each `.spv`, and the
staleness tracking — plus the model/font/icon asset copy next to the host binary. After this phase the
engine has its `.spv` + `lighting.slang-module` + source copies in the runtime shader dir, byte-built by
the same `slangc` invocations CMake used.

## Why this shape (NO LEGACY)

Cargo compiles no shaders, and there is no mature Rust slang crate (`shader-slang` is 0.1.0; `shaderc`
is the wrong compiler) — the feasibility study rejects both. The decision (PP-12) is: keep the pinned
`slangc` binary, drive it from `xtask`, not from a `build.rs`.

`xtask` over `build.rs` is the load-bearing choice. The shaders are a **workspace runtime artifact**
loaded from a directory next to the binary at runtime (the host resolves them via its asset-root logic,
exactly as the C++ engine loads `bin/shaders/*.spv` via `assetPath`). They are not linked into any
crate, so a `build.rs` — which re-runs per-crate, scoped to one crate's `OUT_DIR`, and tied to that
crate's rebuild — is the wrong tool. `xtask shaders` is an explicit step (invoked by `just engine` and
the gate), mirroring CMake's `saffron_compile_shaders` being a *custom target the engine depends on*,
not a compile input. One pipeline, one place; no duplicate per-crate shader build.

The staleness logic is re-implemented rather than dropped: re-running `slangc` over 40 shaders every
build is wasteful, and CMake tracked it via `add_custom_command` `OUTPUT`/`DEPENDS`. `xtask` hand-rolls
the same dependency graph (source mtime + the `lighting.slang` shared-dep edge vs `.spv` mtime).

`std::process::Command` invokes `slangc` (the feasibility study notes this is *safer* than the C++
hand-quoted shell string in `CompileShaders.cmake`). The fetch-fallback in `Slang.cmake` is dropped:
the toolbox always ships `slangc`; a missing `slangc` is a toolbox-provisioning failure, not something
the build silently downloads (NO LEGACY — one source for the compiler, the container).

## Grounding (real files/symbols)

- `cmake/CompileShaders.cmake` — `saffron_compile_shaders(TARGET SHADER_DIR OUT_DIR)` is the exact spec:
  - the `file(GLOB ... *.slang)` fan-out (40 files today);
  - the `lighting.slang-module` rule: `slangc ${lighting_src} -emit-ir -o ${lighting_module}` (no entry
    points → no `.spv`), and `lighting` excluded from the fan-out (`if(name STREQUAL "lighting")
    continue()`);
  - the per-shader rule: `slangc ${shader} -profile glsl_450 -target spirv -emit-spirv-directly
    -fvk-use-entrypoint-name -matrix-layout-column-major -I ${SHADER_DIR} -o ${out}`;
  - the `copy_if_different ${shader} ${src_copy}` source-copy next to each `.spv`;
  - the dependency edges `DEPENDS ${shader} ${lighting_src}` (every shader depends on `lighting.slang`)
    and `DEPENDS ${lighting_src}` (the module) — the staleness graph to reproduce.
- `cmake/Slang.cmake` — `SAFFRON_SLANGC` location order (`PATH` / `SAFFRON_SLANG_DIR` / env), the
  `SAFFRON_SLANG_VERSION 2026.10` pin; the prebuilt-fetch fallback is dropped (toolbox provides it).
- `engine-old/CMakeLists.txt` — `saffron_compile_shaders(SaffronAnima assets/shaders
  ${SAFFRON_RUNTIME_DIR}/shaders)` and the `POST_BUILD copy_directory` of `assets/models|fonts|icons`
  to the runtime dir. `xtask` reproduces both (shader build + the asset copy).
- `engine/assets/shaders/` — the Rust engine's shader source dir (copied from `engine-old/assets/shaders/`
  as part of this phase; the `.slang` files are runtime data, not C++ source). The 40 shaders:
  `mesh`/`lighting`/`gbuffer`/`light_cull`/`ddgi_*`/`restir_*`/`ssgi*`/`gtao`/`taa`/`fxaa`/`tonemap`/
  `motion`/`skin`/`atmos_*`/`ibl_*`/`point_shadow`/`contact`/`gizmo_overlay`/`grid`/`sky`/`preview`/
  `thumbnail`/`triangle`/`copy_color`.
- `engine-old/source/saffron/rendering/renderer_capture.cpp` — confirms the runtime loads shaders from a
  dir next to the binary (the `assetPath` convention the output dir must match), not linked-in.

## Acceptance gate

- `cargo run -p xtask shaders` compiles all 40 entry-point shaders to `<runtime>/shaders/<name>.spv`,
  emits `<runtime>/shaders/lighting.slang-module`, and copies each `.slang` source next to its `.spv` —
  the output set is byte-identical in *file inventory* to what `cmake --build` produced under
  `build/debug/bin/shaders/` (same 39 `.spv` + 1 `.slang-module` + 40 `.slang` copies).
- Running `xtask shaders` twice in a row: the second run recompiles nothing (staleness skip); touching
  `lighting.slang` forces a full fan-out rebuild + the module rebuild; touching one non-lighting shader
  rebuilds only that one.
- `cargo run -p xtask shaders` also copies `models/`, `fonts/`, `icons/` next to the host binary (the
  `POST_BUILD` equivalent).
- `xtask` exits non-zero with a clear message if `slangc` is not found (no silent fetch).
- The workspace still compiles (`cargo build --workspace`); `xtask` is a workspace member that builds.
- A `#[test]` in `xtask` (or a `tests/` integration test) asserts the slangc argument vector matches the
  `CompileShaders.cmake` flag set exactly (`-profile glsl_450 -target spirv -emit-spirv-directly
  -fvk-use-entrypoint-name -matrix-layout-column-major -I <dir>`), guarding against flag drift.
