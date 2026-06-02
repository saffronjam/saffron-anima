+++
title = 'Go-flavored C++'
weight = 1
+++

# Go-flavored C++

Go-flavored C++ is a coding style that uses C++ as if it were Go: small data structs, free
functions over plain data, errors returned as values, and no class hierarchies. It trades the
language's object-oriented machinery for the procedural model that Go enforces by design.

SaffronEngine is written entirely in this style. The whole codebase follows `CONVENTIONS.md`, and
those rules are not optional. The data stays visible, the control flow stays explicit, and a design
question resolves to one test: how would Go do this.

## Allowed constructs

- **Structs with public fields and methods.** A method is a function with a receiver. Data is
  visible, not buried behind getters.
- **Free functions.** Most logic is a pure function over plain data, which keeps it testable. A
  "constructor" is a free function `newThing(...) -> Thing` (or `-> Result<Thing>`).
- **Concepts** as compile-time interfaces, **`std::function` closures** for runtime ones.
- **`std::variant`** for sum types, `Result<T>` for anything that can fail.
- **RAII wrapper structs** for GPU resources. They own a Vulkan handle and free it in their
  destructor. They are move-only — the deleted copy plus defaulted move is resource management,
  not the operator overloading the style otherwise bans.

## Banned constructs

- **Inheritance and `virtual`.** No base classes. A runtime interface is a struct of function
  values, the explicit version of what a Go interface is under the hood. The clearest example is
  `Layer` in the main loop: a bundle of optional callbacks, pushed with `attachLayer`, not a
  subclass.
- **Exceptions.** No `throw`, `try`, or `catch` in engine code. Fallible work returns
  [`Result<T>`](../../core-and-conventions/error-handling/), checked at the call site. Libraries
  that would throw are driven through their no-throw surfaces (Vulkan-Hpp with exceptions
  disabled, nlohmann with `JSON_NOEXCEPTION`, cgltf and tinyobjloader through their C returns).
- **The ternary operator, and operator overloading on our own types.** Use `if`/`else` and named
  functions like `add(a, b)`. GLM's operators are fine; they belong to GLM.

Naming is small and uniform: `PascalCase` types and constants, `camelCase` functions and
variables, `snake_case` files, one `se` namespace. Value-returning functions use a trailing return
type so the name lands right after `auto` and signatures align in a column; void functions stay
`void fn(...)`.

```cpp
auto newRenderer(Window& window) -> Result<Renderer>;   // value-returning: trailing return
void destroyRenderer(Renderer& renderer);               // void stays plain
```

## Why it holds up in a renderer

Graphics code is where object-oriented engines grow the deepest hierarchies: a `Resource` base, a
`RenderPass` base, a `Material` base. SaffronEngine has none. A render pass is an
[`RgPass`](../../frame-and-render-graph/render-graph-overview/) struct with a closure inside it. A
GPU buffer is a move-only struct passed as a `Ref<T>` (a `std::shared_ptr` alias). A component is a
plain struct the [registry](../../scene-and-ecs/component-registry/) knows how to serialize. No
vtable stands between the data and the call site when something breaks.

## In the code

| What | File | Symbols |
|---|---|---|
| The full rules | `CONVENTIONS.md` | naming, allowed/prohibited, return types |
| Aliases + `Result`/`Err` | `core.cppm` | `Ref`, `Result`, `Err`, fixed-width types |
| The itable pattern | `app.cppm` | `Layer` (struct of closures), `attachLayer` |
| RAII GPU wrappers | `renderer_types.cppm` | move-only `Pipeline`/`Image`/`Buffer`/`GpuMesh` |

## Related
- [Error handling](../../core-and-conventions/error-handling/) — the `Result<T>` half of the style
- [Main loop](../../app-lifecycle-and-window/main-loop-and-run/) — the layer itable in action
- [Vulkan foundation](../../vulkan-foundation/) — the move-only GPU wrappers
