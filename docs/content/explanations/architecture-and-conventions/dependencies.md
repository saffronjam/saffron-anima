+++
title = 'Dependencies'
weight = 7
+++

# Dependencies

A third-party dependency is a library the engine consumes but does not author. Each one is either
*vendored* — pinned to a tag and built from source alongside the engine — or *system* — found as a
package the platform already provides. The choice turns on ABI stability and platform ownership.

Most of the engine's libraries are vendored. Only the two pieces that belong to the platform, SDL3
and the Vulkan headers and loader, come from the system. The whole set is declared in one place,
`cmake/Dependencies.cmake`.

## System vs vendored

A system dependency has a stable C ABI and ships with the platform, so building it from source buys
nothing. SDL3 and Vulkan meet that bar and are found as system packages:

```cmake
find_package(Vulkan REQUIRED)        # headers + loader
find_package(SDL3 REQUIRED CONFIG)   # SDL3 3.4.x, C ABI
```

A vendored dependency is pinned to a tag and built from source, so every build resolves the same
revision. Everything else falls into this group:

```cmake
FetchContent_Declare(EnTT GIT_REPOSITORY ... GIT_TAG v3.16.0 GIT_SHALLOW ON)
FetchContent_Declare(glm  GIT_REPOSITORY ... GIT_TAG 1.0.1   GIT_SHALLOW ON)
# VulkanMemoryAllocator, vk-bootstrap, nlohmann_json, imgui (docking), imguizmo …
FetchContent_MakeAvailable(EnTT glm VulkanMemoryAllocator vk-bootstrap nlohmann_json imgui imguizmo)
```

A few dependencies are header-only with an implementation macro, so each needs a translation unit
of its own. VMA, stb (image write and decode), cgltf, tinyobjloader, and nanosvg each get a one-line
static library that defines the implementation macro in a single `.cpp` under `cmake/`. ImGui has no
upstream CMake, so the build compiles its core, the SDL3 and Vulkan backends, and ImGuizmo into one
`imgui` static library.

A single interface target aggregates everything the engine links against, so each engine target
links only `saffron_third_party`:

```cmake
add_library(saffron_third_party INTERFACE)
target_link_libraries(saffron_third_party INTERFACE
    SDL3::SDL3 Vulkan::Vulkan EnTT::EnTT glm::glm nlohmann_json::nlohmann_json
    vk-bootstrap::vk-bootstrap vma stb cgltf tinyobjloader nanosvg imgui)
```

## Definitions that enforce the house rules

The aggregate target also sets the compile definitions that hold third-party libraries to the
engine's no-exceptions, Vulkan-clip-space rules:

```cmake
target_compile_definitions(saffron_third_party INTERFACE
    JSON_NOEXCEPTION GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_ENABLE_EXPERIMENTAL)
```

`JSON_NOEXCEPTION` makes nlohmann turn would-be throws into `abort()`, matching the
[no-exceptions rule](../../core-and-conventions/error-handling/). `GLM_FORCE_DEPTH_ZERO_TO_ONE`
makes `glm::perspective` emit Vulkan's `[0,1]` clip depth instead of OpenGL's `[-1,1]`. Vulkan
itself runs through Vulkan-Hpp with `VULKAN_HPP_NO_EXCEPTIONS`, set per-module in the global
module fragment rather than here.

> [!WARNING]
> `JSON_NOEXCEPTION` turns a would-be JSON throw into `std::abort`, not a recoverable error. The
> JSON gateway parses defensively and validates before indexing so it never reaches that path. See
> [error handling](../../core-and-conventions/error-handling/).

> [!NOTE]
> ImGui uses the docking branch (`v1.92.8-docking`), separate from master, because the editor needs
> dockable panels. ImGuizmo is fetched but compiled into the `imgui` target rather than using its
> own CMake.

## In the code

| What | File | Symbols |
|---|---|---|
| System packages | `cmake/Dependencies.cmake` | `find_package(Vulkan)`, `find_package(SDL3)` |
| Vendored deps + pins | `cmake/Dependencies.cmake` | `FetchContent_Declare`, `FetchContent_MakeAvailable` |
| Header-only impl TUs | `cmake/` | `vma_impl.cpp`, `stb_impl.cpp`, `cgltf_impl.cpp`, … |
| The aggregate target | `cmake/Dependencies.cmake` | `saffron_third_party`, the compile definitions |

## Related
- [Build environment](../build-environment/) — the toolbox that supplies SDL3 and Vulkan
- [Shader compilation](../shader-compilation/) — how the Slang compiler is fetched the same way
- [Error handling](../../core-and-conventions/error-handling/) — why `JSON_NOEXCEPTION` is set
