+++
title = 'Rust house style'
weight = 1
+++

# Rust house style

The house style is the small set of rules the whole codebase follows so that the data stays
visible, the control flow stays explicit, and there is one obvious way to write each thing. It
favours plain data with free functions and methods over deep abstraction, errors returned as
values, and `clippy -D warnings` as a hard gate rather than a suggestion.

Saffron Anima is written this way end to end. The rules are not optional: a design question
resolves to "what is the idiomatic Rust here", and the answer is the one the rest of the tree
already uses.

## What the style favours

- **Plain structs with public fields and inherent methods.** Data is visible, not buried behind
  accessors. A "constructor" is a free function or an associated `fn new(...) -> Self` (or
  `-> Result<Self>` when it can fail).
- **Traits as the runtime interface.** A behaviour boundary is a trait, dispatched statically with
  generics where it can be and behind `Box<dyn Trait>` where the loop must hold a heterogeneous
  thing. The clearest example is [`Layer`](../../app-lifecycle-and-window/main-loop-and-run/): a
  set of default-empty lifecycle hooks an app implements, pushed with `attach_layer` as a
  `Box<dyn Layer>`.
- **Enums for sum types**, `Result<T>` for anything that can fail, and closures (`FnOnce` /
  `FnMut`) for the deferred-work seams.
- **RAII via `Drop`.** A GPU wrapper owns a Vulkan handle and frees it in its `Drop` impl. The
  ownership default is `Ref<T> = Arc<T>` for a value built once and then only read; a shared-mutable
  site spells `Arc<Mutex<T>>` explicitly at its declaration so the exception is visible where it
  occurs.

## Errors as values

Fallible work returns [`Result<T>`](../../core-and-conventions/error-handling/), never a panic for
a recoverable condition. Each library crate declares its own `Error` enum with
[`thiserror`](https://docs.rs/thiserror) and a `Result<T>` alias over it; downstream crates compose
those with `#[from]` and propagate with `?`. `saffron-core` carries the root `Error`; a leaf like
`saffron-app` adds its own `Error` whose variants `#[from]` the crates it drives
(`saffron_window::Error`, `saffron_rendering::Error`).

```rust
#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("failed to create renderer: {0}")]
    Renderer(#[from] saffron_rendering::Error),
}

pub type Result<T> = std::result::Result<T, Error>;
```

`unsafe` is denied workspace-wide (`unsafe_code = "deny"`); the FFI seams that genuinely need it
(the `ash` Vulkan calls, the `cxx` physics bridge) are confined to the crates that own them.

## Why it holds up in a renderer

Graphics code is where engines grow the deepest hierarchies: a `Resource` base, a `RenderPass`
base, a `Material` base. Saffron Anima has none. A render pass is an
[`RgPass`](../../frame-and-render-graph/render-graph-overview/) struct with a closure inside it. A
GPU buffer is a `Buffer` struct that frees its allocation in `Drop`, passed around as a `Ref<T>`. A
component is a plain struct the [registry](../../scene-and-ecs/component-registry/) knows how to
serialize. Nothing stands between the data and the call site when something breaks.

## In the code

| What | File | Symbols |
|---|---|---|
| Root error + `Result` + `Ref` | `crates/core/src/error.rs`, `crates/core/src/lib.rs` | `Error`, `Result`, `Ref` |
| The trait-as-itable pattern | `crates/app/src/lib.rs` | `Layer`, `attach_layer`, `App` |
| RAII GPU wrappers | `crates/rendering/src/resources.rs` | `Buffer`, `Image`, `GpuMesh` (each with `Drop`) |
| Lint policy | `engine/Cargo.toml` | `[workspace.lints]` (`unsafe_code = "deny"`, `clippy.all`) |

## Related
- [Error handling](../../core-and-conventions/error-handling/) — the `Result<T>` half of the style
- [Main loop](../../app-lifecycle-and-window/main-loop-and-run/) — the `Layer` itable in action
- [Vulkan foundation](../../vulkan-foundation/) — the `Drop`-owned GPU wrappers
