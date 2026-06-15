+++
title = 'Shader compilation'
weight = 6
+++

# Shader compilation

Shader compilation translates shader source into the binary intermediate form the GPU driver
accepts. Saffron writes shaders in Slang and compiles them to SPIR-V during the CMake build; there
is no runtime compiler. Every `.slang` file becomes a `.spv` next to the executable before the
engine starts.

Compiling ahead of time moves the cost off the critical path and surfaces shader errors at build
time rather than first use. The build is the single place where shader source meets the toolchain.

## Build-time compile

The shader sources live under `engine/assets/shaders/`, one file per pass (`mesh.slang`,
`light_cull.slang`, `gtao.slang`, `taa.slang`, and so on). The CMake function
`saffron_compile_shaders(target, src_dir, out_dir)` globs every `*.slang`, runs `slangc` on each,
and makes the target depend on the results:

```cmake
add_custom_command(
    OUTPUT ${out}
    COMMAND ${SAFFRON_SLANGC} ${shader}
            -profile glsl_450 -target spirv -emit-spirv-directly
            -fvk-use-entrypoint-name -matrix-layout-column-major
            -o ${out}
    DEPENDS ${shader}
    VERBATIM)
```

The editor wires it up so the shaders build alongside the executable and land in `bin/shaders/`:

```cmake
saffron_compile_shaders(SaffronAnima
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders
    ${SAFFRON_RUNTIME_DIR}/shaders)
```

Several flags carry weight. `-emit-spirv-directly` emits SPIR-V without routing through GLSL.
`-fvk-use-entrypoint-name` preserves the entry-point names, so the renderer can load multiple
`[shader(...)]`-tagged entry points from one module. `-matrix-layout-column-major` matches GLM's
column-major matrices, so a CPU-side transform lands in the shader unchanged. All entry points in
one `.slang` file compile into a single `.spv`.

## Finding slangc

`Slang.cmake` locates the compiler before anything compiles. It prefers a `slangc` already on
`PATH` or under `SAFFRON_SLANG_DIR`; otherwise it fetches the official prebuilt release (pinned to
**2026.10**) and verifies its checksum. In the toolbox the prebuilt sits under
`~/.cache/saffron-slang/`, so the fetch is a no-op after the first configure.

> [!NOTE]
> The shader glob uses `CONFIGURE_DEPENDS`, so adding a new `.slang` is picked up on the next
> build. Changing the *set* of files outside a configure can still need a reconfigure depending on
> the generator.

## In the code

| What | File | Symbols |
|---|---|---|
| The compile function | `cmake/CompileShaders.cmake` | `saffron_compile_shaders`, the `slangc` flags |
| Locating the compiler | `cmake/Slang.cmake` | `SAFFRON_SLANGC`, `SAFFRON_SLANG_VERSION` |
| Wiring it to the build | `engine/CMakeLists.txt` | `saffron_compile_shaders(SaffronAnima ...)` |
| The shader sources | `engine/assets/shaders/` | `mesh.slang`, `light_cull.slang`, `taa.slang`, … |

## Related
- [Build environment](../build-environment/) — where `slangc` runs (the toolbox)
- [Dependencies](../dependencies/) — how other vendored tools/libraries are pulled in
- [Materials and pipelines](../../materials-and-pipelines/) — how the compiled SPIR-V becomes a PSO
