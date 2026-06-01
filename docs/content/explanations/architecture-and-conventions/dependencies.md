+++
title = 'Dependencies'
weight = 7
+++

# Dependencies

The engine pulls most of its libraries in as pinned, vendored source built statically through
CMake's FetchContent. Only the two pieces that belong to the platform — SDL3 and the Vulkan
headers/loader — come from the system. It is all set up in one place, `cmake/Dependencies.cmake`.

## System vs vendored

SDL3 and Vulkan are found as system packages. They have stable C ABIs and ship with the platform,
so vendoring them buys nothing:

```cmake
find_package(Vulkan REQUIRED)        # headers + loader
find_package(SDL3 REQUIRED CONFIG)   # SDL3 3.4.x, C ABI
```

Everything else is declared with a pinned tag and built from source:

```cmake
FetchContent_Declare(EnTT GIT_REPOSITORY ... GIT_TAG v3.16.0 GIT_SHALLOW ON)
FetchContent_Declare(glm  GIT_REPOSITORY ... GIT_TAG 1.0.1   GIT_SHALLOW ON)
# VulkanMemoryAllocator, vk-bootstrap, nlohmann_json, imgui (docking), imguizmo …
FetchContent_MakeAvailable(EnTT glm VulkanMemoryAllocator vk-bootstrap nlohmann_json imgui imguizmo)
```

A few deps are header-only with an implementation macro, so each needs a translation unit of its
own. VMA, stb (image write + decode), cgltf, tinyobjloader, and nanosvg each get a one-line static
library that defines the implementation macro in a single `.cpp` under `cmake/`. ImGui has no
upstream CMake, so the build compiles its core, the SDL3 and Vulkan backends, and ImGuizmo into one
`imgui` static library.

A single interface target aggregates everything the engine links against, so each engine target
just links `saffron_third_party`:

```cmake
add_library(saffron_third_party INTERFACE)
target_link_libraries(saffron_third_party INTERFACE
    SDL3::SDL3 Vulkan::Vulkan EnTT::EnTT glm::glm nlohmann_json::nlohmann_json
    vk-bootstrap::vk-bootstrap vma stb cgltf tinyobjloader nanosvg imgui)
```

## Definitions that enforce the house rules

The aggregate target also sets the compile definitions that keep third-party libraries inside the
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
