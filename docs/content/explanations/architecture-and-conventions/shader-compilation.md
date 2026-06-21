+++
title = 'Shader compilation'
weight = 6
+++

# Shader compilation

Shader compilation translates shader source into the SPIR-V binary the GPU driver accepts. Saffron
writes shaders in Slang and compiles them ahead of time with the `xtask` helper; there is no runtime
compiler. Every `.slang` entry-point file becomes a `.spv` next to the host binary before the engine
starts.

Compiling ahead of time moves the cost off the critical path and surfaces shader errors at build
time rather than first use. The `xtask shaders` task is the single place where shader source meets
the Slang toolchain.

## The xtask shaders task

The shader sources live under `engine/assets/shaders/`, one file per pass (`mesh.slang`,
`light_cull.slang`, `gtao.slang`, `taa.slang`, …). Running

```sh
cargo run -p xtask -- shaders            # --profile debug by default
cargo run -p xtask -- shaders --profile release
```

resolves the inputs (`Config::resolve`), then `run` walks `engine/assets/shaders/`, compiles each
`*.slang` entry-point file to `target/<profile>/shaders/<name>.spv`, copies each `.slang` source
next to its `.spv` (the runtime node-graph codegen splices `mesh.slang`), and copies the `models/`,
`fonts/`, `icons/` asset trees next to the host binary so `asset_path(...)` resolves.

The per-shader `slangc` invocation uses a frozen flag set, held in one constant so a drift test can
assert against it:

```rust
// engine/xtask/src/shaders.rs
pub const SLANGC_SPV_FLAGS: &[&str] = &[
    "-profile", "glsl_450",
    "-target", "spirv",
    "-emit-spirv-directly",
    "-fvk-use-entrypoint-name",
    "-matrix-layout-column-major",
];
```

The full argument vector is `<src>` + those flags + `-I <shader_dir>` + `-o <out>`. The flags carry
weight: `-emit-spirv-directly` emits SPIR-V without routing through GLSL; `-fvk-use-entrypoint-name`
preserves the entry-point names, so the renderer can load multiple `[shader(...)]`-tagged entry
points from one module; `-matrix-layout-column-major` matches glam's column-major matrices, so a
CPU-side transform lands in the shader unchanged. All entry points in one `.slang` file compile into
a single `.spv`.

## The shared lighting module

`lighting.slang` is special: it declares no entry points and emits no `.spv`. It is precompiled once
to `lighting.slang-module` with `slangc <src> -emit-ir -o <module>`, and `mesh.slang` plus the
codegen material variants `import lighting` against the precompiled module rather than recompiling
it. Because every shader depends on `lighting.slang`, touching it forces the full fan-out to rebuild.

## Staleness and finding slangc

`run` tracks staleness by source-vs-output mtime, with the `lighting.slang` shared-dependency edge
folded into every shader's dependency set, so a second run with no source changes recompiles nothing
(`Report { spv_compiled: 0, spv_skipped: N, module_compiled: false }`).

`find_slangc` resolves the compiler in order: a `PATH` lookup, then `SAFFRON_SLANG_DIR/bin/slangc`,
then the conventional toolbox cache `$HOME/.cache/saffron-slang/slang/bin/slangc`. A missing
`slangc` is a hard error — the `saffron-build` toolbox provisions it (the pinned `2026.10`); there is
no silent prebuilt fetch at build time.

> [!NOTE]
> `just engine` and `just run` invoke `cargo run -p xtask -- shaders` after `cargo build`, so the
> `.spv` files are always current beside the host binary the editor spawns.

## In the code

| What | File | Symbols |
|---|---|---|
| The task entry point | `engine/xtask/src/main.rs` | `run_shaders`, the `shaders` arm |
| The pipeline + asset copy | `engine/xtask/src/shaders.rs` | `Config::resolve`, `run`, `compile_spv` |
| The frozen flag set | `engine/xtask/src/shaders.rs` | `SLANGC_SPV_FLAGS`, `spv_arg_vector` |
| Locating the compiler | `engine/xtask/src/shaders.rs` | `find_slangc`, `SLANG_VERSION` |
| The shader sources | `engine/assets/shaders/` | `mesh.slang`, `light_cull.slang`, `lighting.slang`, … |

## Related
- [Build environment](../build-environment/) — where `cargo run -p xtask -- shaders` runs
- [Dependencies](../dependencies/) — the rest of the Cargo dependency set
- [Materials and pipelines](../../materials-and-pipelines/) — how the compiled SPIR-V becomes a PSO
