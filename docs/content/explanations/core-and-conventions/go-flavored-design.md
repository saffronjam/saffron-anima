+++
title = 'Rust house style'
weight = 1
+++

# Rust house style

The house style is idiomatic Rust with the conventional Rust answers to design questions:
plain data structs, free functions and inherent methods over them, errors as typed values,
traits for behaviour, and ownership tracked by the borrow checker rather than by hand. When a
question has a clear idiomatic Rust answer, that is the answer Saffron takes.

Knowing the style first makes the later pages easier to read: the rendering, scene, and control
code all lean on it.

## What the style uses

The vocabulary is small and built from plain data:

- Structs with public fields plus inherent methods (a method is a function with a `self`
  receiver). Most logic is a free function or an associated function over plain data, which
  keeps it testable.
- Traits as interfaces — `Layer` (the lifecycle hook bundle) and `FrameHost` (the loop's GPU
  seam) are both traits. A boxed `dyn Trait` is the runtime-polymorphic form; closures
  (`Box<dyn FnOnce(..)>`, `impl FnMut(..)`) are the lightweight one.
- `enum` for sum types and the typed error model — and `Result<T>` (each crate's alias over its
  own `Error`) for anything that can fail.
- `Drop` for resource cleanup; ownership and `Arc<T>` for sharing (the [ownership
  page](../ownership-and-raii/) covers the rules).

## Clippy is law

The workspace turns the whole Clippy `all` group on as a warning and denies `unsafe_code`
crate-wide; the leaf crates (`saffron-core`, `saffron-signal`, `saffron-json`) re-deny
`unsafe_code` at the crate root for good measure. The lint gate is part of "done": a change that
trips a Clippy lint is not finished until the lint is clean. Where `unsafe` is genuinely
required — the VMA and shared-memory seams in `saffron-rendering` — it is local, documented with
a `// SAFETY:` note, and confined to the crate that owns the FFI boundary.

```toml
[workspace.lints.rust]
unsafe_code = "deny"

[workspace.lints.clippy]
all = "warn"
```

## Errors are typed values

No panics on the fallible path. Each library crate declares its own error `enum` with
[`thiserror`](../error-handling/) and exports a `Result<T>` alias over it; callers compose
errors with `#[from]` and propagate with `?`. Panics are reserved for genuine invariants the
type system can't express, and `#[should_panic]` tests pin those.

## Why it holds up in a renderer

Graphics code is where engines usually grow the deepest class hierarchies — a `Resource` base, a
`RenderPass` base, a `Material` base. Saffron has none. A GPU buffer is a plain struct
(`Buffer`) whose `Drop` frees its Vulkan handle; a render pass is data the [render
graph](../../frame-and-render-graph/render-graph-overview/) walks; a component is a plain struct
the [registry](../../scene-and-ecs/component-registry/) knows how to serialize. The data is
visible, the control flow is explicit, and the borrow checker — not a hand-audited teardown
order — proves the lifetimes.

## In the code

| What | File | Symbols |
|---|---|---|
| The lint gate | `engine/Cargo.toml` | `[workspace.lints.rust]` (`unsafe_code = "deny"`), `[workspace.lints.clippy]` (`all = "warn"`) |
| The lifecycle trait | `engine/crates/app/src/lib.rs` | `Layer`, `attach_layer` |
| Drop-based GPU wrappers | `engine/crates/rendering/src/resources.rs` | `Buffer`, `Image`, `GpuMesh`, `GpuTexture`, `Pipeline` |

## Related

- [Error handling](../error-handling/) — the typed `Result<T>` / `thiserror` half of the style
- [Ownership](../ownership-and-raii/) — `Drop`, ownership, and `Arc<T>`
- [Main loop and run](../../app-lifecycle-and-window/main-loop-and-run/) — the `Layer` trait in action
