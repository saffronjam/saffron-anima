+++
title = 'C++26 modules'
weight = 2
+++

# C++26 modules

A C++ named module is a self-contained unit of code that exports an explicit interface, replacing
the textual `#include` model with a compiled binary interface. C++26 also lets a program pull the
entire standard library in as one module with `import std`, rather than including its headers.

The engine is authored this way: every engine area is a named module, and modules that need the
standard library import it. The module boundary is the real boundary between engine areas — module
code includes no standard headers and relies on no precompiled-header soup.

## Named modules

Every engine area is one named module, `Saffron.<Area>`, declared and exported from a `.cppm` file.
`Saffron.Core` is the smallest complete example:

```cpp
export module Saffron.Core;

import std;

export namespace se
{
    template <typename T>
    using Ref = std::shared_ptr<T>;
    // ...
}
```

`import std` pulls the whole standard library in as a module rather than as headers. It compiles
faster than textual includes and keeps macros out of the picture. Modules that touch only the
standard library (`core`, `signal`, `app`) use it directly. `window` mixes `import std` with the
SDL3 **C** header, which is safe because C headers do not clash with the std module.

## Heavy C++ headers

Modules that wrap heavy **C++** third-party headers — `rendering`, `ui`, `scene`, `geometry`,
`json`, `assets`, `sceneedit`, `control` — do **not** `import std`. They use classic `#include` in the
global module fragment, because mixing `import std` with a heavy C++ header (Vulkan-Hpp, ImGui,
entt) in one translation unit breaks the build. Consumers still get the std types: the compiled
module interface (BMI) carries them across.

```cpp
module;                          // global module fragment
#define VULKAN_HPP_NO_EXCEPTIONS
#include <vulkan/vulkan.hpp>     // heavy C++ header, classic include
#include <glm/glm.hpp>
#include <vector>                // std via include, NOT import std here

export module Saffron.Rendering:Types;
```

## Enabling the std module

`import std` is still experimental in CMake 3.31, gated behind a version-specific UUID set once at
the project root:

```cmake
cmake_minimum_required(VERSION 3.31)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
set(CMAKE_CXX_STANDARD 26)
```

Each target that wants the std module sets `CXX_MODULE_STD ON`, and named module interfaces go in a
`FILE_SET CXX_MODULES`. The UUID changes per CMake version, so a toolchain bump means a new one.

CMake builds the internal std module as `gnu++26`. A consumer compiled as plain `c++26` rejects the
std BMI with "GNU extensions was enabled in precompiled file but is currently disabled". Leaving
extensions on (the default) keeps the std module and its consumers on the same dialect.

> [!WARNING]
> Do **not** set `CMAKE_CXX_EXTENSIONS OFF`. The std module builds as `gnu++26`; a `c++26`
> consumer rejects its BMI outright. Leave extensions on so they match.

> [!WARNING]
> Don't `import std` in a module that includes a heavy C++ third-party header (Vulkan-Hpp, ImGui,
> entt). The two clash in one TU. Use classic `#include` in the global module fragment instead;
> consumers still get the std types through the BMI.

## In the code

| What | File | Symbols |
|---|---|---|
| Import-std gate + dialect | `CMakeLists.txt` | `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD`, `CMAKE_CXX_STANDARD` |
| Per-target std module | `engine/CMakeLists.txt` | `CXX_MODULE_STD ON`, `FILE_SET CXX_MODULES` |
| Clang + libc++ presets | `CMakePresets.json` | `clang-libcxx`, `-stdlib=libc++` |
| A pure-std module | `core.cppm` | `export module Saffron.Core; import std;` |
| A heavy-header module | `renderer_types.cppm` | global module fragment + classic includes |

## Related
- [Module partitions](../module-partitions/) — splitting a big module without the BMI ICE
- [Module DAG](../module-dag/) — how the modules depend on each other
- [Build environment](../build-environment/) — the toolbox that ships the libc++ std module
