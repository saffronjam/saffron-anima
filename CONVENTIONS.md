# Saffron code conventions — Go-flavored C++

Saffron is written in a deliberately **Go-like** style: small, data-oriented,
functional, with very limited OOP. When in doubt, ask: *"how would Go do this?"*

## Naming
| Thing | Case | Example |
|-------|------|---------|
| Types (struct, enum, concept) | `PascalCase` | `RenderGraph`, `ImageFormat` |
| Functions & methods | `camelCase` | `createApp`, `submitModel` |
| Variables, params, fields | `camelCase` | `frameIndex`, `swapchainImages` |
| Constants & enum values | `PascalCase` | `MaxFramesInFlight`, `ImageFormat::Rgba16f` |
| Files | `snake_case` | `render_graph.cppm`, `vulkan_context.cppm` |
| Directories | `lowercase` | `engine/source/saffron/rendering/` |
| Namespace | `sa` (one package-like namespace) | `sa::createApp()` |
| Modules | `Saffron.<Area>` | `Saffron.Rendering` |

(CMake files under `cmake/` keep CMake's own `PascalCase.cmake` convention — they
are not our source.)

## Allowed — Go has these
- `struct`s with public fields and methods (a method is a function with a receiver).
- **Free functions — prefer these.** Pure functions wherever practical (testability).
- **Concepts** as compile-time interfaces (the method set a type must satisfy).
- **Closures / `std::function`** for behavior injection and *runtime* interfaces.
- `std::variant` for sum types; `Result<T>` (= `std::expected<T, std::string>`) for fallible
  returns, with `Err("message")` for failure (see *Return types*).
- Generics via templates/concepts (Go has generics) — keep them simple and flat.
- **RAII wrapper types for GPU / data-plane resources** (the rendering "meta-layer"):
  a `struct` that owns a Vulkan handle and frees it in its destructor. These are
  move-only (deleted copy, defaulted/explicit move) — the destructor + move
  assignment are *resource management*, NOT the prohibited operator overloading.
  Vulkan is used via **Vulkan-Hpp `vk::`** with `VULKAN_HPP_NO_EXCEPTIONS` (calls
  return results, converted to `Result`); never `vk::raii` (it throws).

## Prohibited — Go does not have these
- **Inheritance.** No `: public Base`, no class hierarchies.
- **`virtual` / abstract base classes.** Model a *runtime* interface as a struct of
  function values (an explicit itable — which is exactly what a Go interface is),
  or as a `concept` for compile-time dispatch.
- **Exceptions — entirely.** Never `throw`, `try`, or `catch` in our code. Every
  fallible operation returns `Result<T>` (or `Result<void>`) and reports failure with
  `Err("message")`; the result MUST be checked at the call site **immediately** — never
  propagate an unchecked `Result`. Third-party libraries that can throw are driven
  through their no-throw APIs/configs and converted to `Result` at the boundary.
- **The ternary operator `?:`.** Use `if`/`else`.
- **Operator overloading on our own types.** Use named free functions (`add(a, b)`).
  Third-party libs (e.g. GLM) may use operators internally; that's fine.
- Implicit conversions, RTTI-driven designs, deep encapsulation ceremony.

## Shape of things
- Data and behavior are separated where it aids testing: plain data `struct`s +
  free functions that transform them.
- A "constructor" is a free function `newThing(...) -> Thing` (or `-> Result<Thing>`).
- Prefer composition over any form of subtyping.
- One namespace, `sa`. Modules provide the real boundaries.

## Return types
- **Trailing return type for value-returning functions:** `auto functionName(ParamType param) -> ReturnType`.
  The name lands right after `auto`, so names align in a column and signatures read left-to-right.
  **Void functions stay `void functionName(...)`** — `auto` and `void` are both 4 chars, so the names
  still align, and `auto f() -> void` is needless. Lambdas keep deduced/explicit returns as written;
  `main` and constructors/destructors are unchanged.
- **Fallible returns are `Result<T>`**, never the spelled-out `std::expected<T, std::string>`;
  return failure with `Err("message")`, success as the value itself (`return x;`) or `{}`
  for `Result<void>`. (`Result`/`Err` live in `Saffron.Core`.)
- **Prefer `auto` for locals bound to a call result** — `auto x = makeThing();` — especially
  `Result`/`Ref`/iterator types. Keep an explicit type only when it documents intent or the
  initializer's type isn't obvious at the call site.

## Comments
- **No inline comments unless the logic is genuinely non-obvious.** Prefer a clear
  name over a comment that restates the code.
- **No section / banner comments. Never.** No `// --- Helpers ---`, `// ======`,
  `// --- Pass 1 ---`, no ASCII dividers. Modules and functions are the structure;
  dividers are noise.
- **Doc comments on exported types and functions: encouraged** — the C++ equivalent
  of a Godoc/JSDoc comment. Put a brief `///` (one or two lines) on the *declaration*
  in the `export namespace` block: what it is and any contract (ownership, who calls
  it, what an error means). Don't repeat the doc on the definition.
- **No migration / change-journey comments.** Strictly prohibited: anything that
  only makes sense if the reader remembers a previous version of the code. If a
  comment contains any of these, delete or rewrite it:
  - "previously", "used to", "was X", "no longer", "formerly", "legacy", "historic(al)"
  - "refactor", "this refactor", "the new model", "now that", "has been moved/renamed/routed"
  - "rather than the old", "replaces X", "split out of X", "unified from X"
  - "newly added", "recently added", "this commit", "this PR"
- Comments describe what the code does **now**. For non-obvious current behavior,
  say **why it is that way now** — never by contrast with what it used to be.
  `git log` / `git blame` carry the history.
