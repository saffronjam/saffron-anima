+++
title = 'Write a Slang shader'
weight = 6
math = false
+++

# Write a Slang shader

Add a `.slang` file, have CMake compile it to SPIR-V, and load it at runtime.

## Steps

1. Drop a `.slang` into `editor/assets/shaders/`. Tag entry points with `[shader("vertex")]` / `[shader("fragment")]` / `[shader("compute")]` — all tagged entry points land in one SPIR-V module. Use `mesh.slang` as a reference for binding layout (set 0 bindless albedo, set 1 lighting, push-constant camera).
2. Re-run CMake configure so the new file is globbed:
   ```sh
   toolbox run -c saffron-build bash -lc '
     cd /var/home/saffronjam/repos/SaffronEngine && cmake --preset debug'
   ```
   `saffron_compile_shaders` GLOBs `*.slang` and emits `<name>.spv` under `bin/shaders/`, compiling each with `slangc ... -profile glsl_450 -target spirv -emit-spirv-directly -fvk-use-entrypoint-name -matrix-layout-column-major`.
3. Build — the shaders are a dependency of `SaffronEditor`, so they compile alongside it:
   ```sh
   toolbox run -c saffron-build bash -lc '
     cd /var/home/saffronjam/repos/SaffronEngine && cmake --build build/debug -j1'
   ```
4. Reference the `.spv` by its runtime-relative path when building a pipeline. A `Material` names its shader (default `"shaders/mesh.spv"`); the renderer loads it via `loadShaderModule(...)` and caches the PSO with `requestMeshPipeline`.

## Verify

- After configure, the build log shows `slangc <name>.slang -> <name>.spv`.
- The compiled module lands at `build/debug/bin/shaders/<name>.spv`.
- A pipeline using it builds without a `loadShaderModule` error, and `se render-stats` reports the `pipelines` count growing when a new PSO is cached.

## In the code

| What | File | Symbols |
|---|---|---|
| The GLOB + slangc invocation | `CompileShaders.cmake` | `saffron_compile_shaders`, `${SAFFRON_SLANGC}` |
| Where the editor wires it | `editor/CMakeLists.txt` | `saffron_compile_shaders(SaffronEditor ...)` |
| Reference shader | `mesh.slang` | `[shader(...)]` entry points, set/binding layout |
| Load + cache the PSO | `renderer_pipelines.cpp` | `loadShaderModule`, `requestMeshPipeline` |
| Material → shader path | `renderer_types.cppm` | `Material::shader` (`"shaders/mesh.spv"`) |

## Related

- [Material and PSO selection](../../explanations/materials-and-pipelines/material-and-pso-selection/)
- [Übershader and specialization](../../explanations/materials-and-pipelines/ubershader-and-specialization/)
- [Descriptor sets](../../explanations/materials-and-pipelines/descriptor-sets/)
