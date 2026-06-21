+++
title = 'Write a Slang shader'
weight = 6
math = false
+++

# Write a Slang shader

Add a `.slang` file, have the `xtask` shader pipeline compile it to SPIR-V, and load it at runtime.

## Steps

1. Drop a `.slang` into `engine/assets/shaders/`. Tag entry points with `[shader("vertex")]`, `[shader("fragment")]`, or `[shader("compute")]`; all tagged entry points land in one SPIR-V module. Use `mesh.slang` as a reference for binding layout: set 0 bindless albedo, set 1 lighting, push-constant camera.
2. Run the shader pipeline. It scans every `*.slang` under `engine/assets/shaders/` and compiles each to `<name>.spv` next to the host binary:
   ```sh
   toolbox run -c saffron-build bash -lc '
     cd /var/home/saffronjam/repos/SaffronEngine/engine && cargo run -p xtask -- shaders'
   ```
   `xtask shaders` compiles each entry-point shader with `slangc <shader>.slang -profile glsl_450 -target spirv -emit-spirv-directly -fvk-use-entrypoint-name -matrix-layout-column-major -I <shader_dir> -o <out>`. The shared `lighting.slang` is precompiled once to `lighting.slang-module` (`slangc … -emit-ir`, no `.spv`) and every other shader `import lighting` against it. Staleness is mtime-tracked, so a re-run recompiles nothing untouched.
3. The full engine build runs the same pipeline after the Cargo build, so a plain build picks up the new file too:
   ```sh
   toolbox run -c saffron-build bash -lc '
     cd /var/home/saffronjam/repos/SaffronEngine/engine && cargo build --workspace && cargo run -p xtask -- shaders'
   ```
   (`just engine` wraps both steps.)
4. Reference the `.spv` by its runtime-relative path when building a pipeline. A `Material` names its shader (default `"shaders/mesh.spv"`); the renderer loads it via `load_shader_module(...)` and caches the PSO with `request_mesh_pipeline`.

## Verify

- The run prints `xtask shaders: N compiled, …`, and the new `<name>.slang -> <name>.spv` is among them on the first run.
- The compiled module lands at `engine/target/debug/shaders/<name>.spv` (the `release` profile lands under `engine/target/release/shaders/`).
- A pipeline using it builds without a `load_shader_module` error, and `sa render-stats` reports the `pipelines` count growing as a new PSO is cached.

## In the code

| What | File | Symbols |
|---|---|---|
| The shader scan + slangc invocation | `engine/xtask/src/shaders.rs` | `run`, `compile_spv`, `SLANGC_SPV_FLAGS`, `Config::resolve` |
| The `xtask shaders` task | `engine/xtask/src/main.rs` | `run_shaders` |
| Reference shader | `engine/assets/shaders/mesh.slang` | `[shader(...)]` entry points, set/binding layout |
| Load + cache the PSO | `engine/crates/rendering/src/pipelines.rs` | `load_shader_module`, `request_mesh_pipeline` |
| Material → shader path | `engine/crates/rendering/src/gpu_types.rs` | `Material::shader` (`"shaders/mesh.spv"`) |

## Related

- [Material and PSO selection](../../explanations/materials-and-pipelines/material-and-pso-selection/)
- [Übershader and specialization](../../explanations/materials-and-pipelines/ubershader-and-specialization/)
- [Descriptor sets](../../explanations/materials-and-pipelines/descriptor-sets/)
