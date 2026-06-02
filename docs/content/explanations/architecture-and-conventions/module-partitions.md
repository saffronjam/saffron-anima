+++
title = 'Module partitions'
weight = 3
+++

# Module partitions

A module partition is a named fragment of a single C++ module, declared with a colon
(`Saffron.Rendering:Types`), that the primary module interface stitches into one logical unit. A
module can also be spread across implementation units: plain `.cpp` files that open the module with
a bare declaration and export nothing.

Together these split a large module across many files. `Saffron.Rendering` uses interface
partitions for its exported surface and implementation units for its feature code. The choice
between the two is forced by a Clang toolchain bug rather than by preference.

## Interface partitions and impl units

`Saffron.Rendering` is one module spread across several files under
`engine/source/saffron/rendering/`.

Interface partitions carry the exported surface. `renderer_types.cppm` is the `:Types` partition:
every data struct, the `Renderer` aggregate, and all public function declarations.
`renderer_detail.cppm` is the `:Detail` partition of exported internal helpers.

The primary interface `renderer.cppm` does orchestration only (`newRenderer`, `beginFrame`,
`endFrame`, the viewport getters) and stitches the partitions together:

```cpp
export module Saffron.Rendering;

export import :RenderGraph;   // re-exported to consumers
export import :Types;
```

`:Types` and `:RenderGraph` are `export import`ed so consumers see them; `:Detail` is plain
`import`ed so the internal helpers stay off the consumer BMI.

Implementation units are regular `.cpp` files, one per feature, that open the module with a bare
declaration and import nothing extra:

```cpp
module;
#include <vulkan/vulkan.hpp>
// ...
module Saffron.Rendering;   // implementation unit, NOT a partition
```

`renderer_pipelines.cpp`, `renderer_lighting.cpp`, `renderer_aa.cpp` and the rest each define a
slice of the declarations from `:Types`. Cross-unit calls resolve through those public decls; a
purely internal helper is co-located in the `.cpp` with its sole caller.

In CMake the difference is the file set. Interface partitions go in `FILE_SET CXX_MODULES`
(dependency-ordered, before the primary). Implementation units are ordinary `PRIVATE` sources.

## Why impl units instead of more partitions

Making every feature its own interface partition (`export module Saffron.Rendering:Pipelines;` and
so on) triggers a flaky Clang 21 + libc++ `import std` BMI-serialization crash: an internal compiler
error in `ASTWriter` while serializing std declarations, surfacing as a `SIGBUS` in a random
translation unit. An implementation unit produces no BMI, so there is nothing to serialize and no
ICE. Feature code therefore lives in `.cpp` files.

The same mechanism covers the editor (`:Context` partition plus five `.cpp` units) and the control
plane (`:Command` partition plus four `.cpp` units). It is the codebase-wide pattern for splitting
a large module.

> [!WARNING]
> Making each feature an interface partition (`export module Saffron.Rendering:Feature;`) hits a
> Clang 21 + libc++ `import std` BMI-serialization ICE (`SIGBUS` in `ASTWriter`). Feature code must
> be `.cpp` implementation units (`module Saffron.Rendering;`, no `export`) so no BMI is generated.

## In the code

| What | File | Symbols |
|---|---|---|
| Interface partition | `renderer_types.cppm` | `export module Saffron.Rendering:Types;` |
| Internal-helper partition | `renderer_detail.cppm` | `export module Saffron.Rendering:Detail;` |
| Primary + re-export | `renderer.cppm` | `export import :Types;`, `export import :RenderGraph;` |
| Implementation unit | `renderer_pipelines.cpp` | `module Saffron.Rendering;` (no `export`) |
| File-set wiring | `engine/CMakeLists.txt` | `FILE_SET CXX_MODULES` vs `PRIVATE` sources |

## Related
- [C++26 modules](../cxx26-modules/) — why the std module is involved at all
- [Module DAG](../module-dag/) — where the rendering partitions sit in the graph
- [Build environment](../build-environment/) — why builds run `-j1` to dodge the same ICE
