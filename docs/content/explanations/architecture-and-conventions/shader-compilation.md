+++
title = 'Shader compilation'
weight = 6
+++

# Shader compilation

Shaders are written in Slang and compiled to SPIR-V as part of the CMake build. The engine has no
runtime shader compiler: every `.slang` file becomes a `.spv` next to the executable before the
engine starts.

## Build-time compile

The shader sources live under `editor/assets/shaders/`, one file per pass (`mesh.slang`,
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
saffron_compile_shaders(SaffronEditor
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders
    ${SAFFRON_RUNTIME_DIR}/shaders)
```

A few flags carry weight. `-emit-spirv-directly` makes Slang emit SPIR-V without bouncing through
GLSL. `-fvk-use-entrypoint-name` keeps the entry-point names so the renderer can load multiple
`[shader(...)]`-tagged entry points from one module. `-matrix-layout-column-major` matches GLM's
column-major matrices, so a CPU-side transform lands in the shader the right way around. All entry
points in one `.slang` file compile into a single `.spv`.

## Finding slangc

`Slang.cmake` locates the compiler before anything compiles. It prefers a `slangc` already on
`PATH` or under `SAFFRON_SLANG_DIR`; failing that it fetches the official prebuilt release (pinned
to **2026.10**) with a checksum. In the toolbox the prebuilt sits under `~/.cache/saffron-slang/`,
so the fetch is a no-op after the first configure.

> [!NOTE]
> The shader glob uses `CONFIGURE_DEPENDS`, so adding a new `.slang` is picked up on the next
> build. Changing the *set* of files outside a configure can still need a reconfigure depending on
> the generator.

## In the code

| What | File | Symbols |
|---|---|---|
| The compile function | `cmake/CompileShaders.cmake` | `saffron_compile_shaders`, the `slangc` flags |
| Locating the compiler | `cmake/Slang.cmake` | `SAFFRON_SLANGC`, `SAFFRON_SLANG_VERSION` |
| Wiring it to the build | `editor/CMakeLists.txt` | `saffron_compile_shaders(SaffronEditor ...)` |
| The shader sources | `editor/assets/shaders/` | `mesh.slang`, `light_cull.slang`, `taa.slang`, … |

## Related
- [Build environment](../build-environment/) — where `slangc` runs (the toolbox)
- [Dependencies](../dependencies/) — how other vendored tools/libraries are pulled in
- [Materials and pipelines](../../materials-and-pipelines/) — how the compiled SPIR-V becomes a PSO
