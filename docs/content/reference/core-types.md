+++
title = 'Core types'
weight = 1
math = false
+++

# Core types

The `saffron-core` crate is the DAG root: it exports the `Result`/`Error` model, the `Ref` alias, the `Uuid` and `TimeSpan` value types, logging, and the engine identity constants. Every other crate composes against these.

## Error as value

Fallible functions return `Result<T>`; there are no panics on the engine path. Each library crate exports its own `Result<T>` over its own `thiserror` `Error` enum — the table below is `saffron-core`'s.

| What | File | Symbols |
|---|---|---|
| `Result<T>` and the root `Error` | `error.rs` | `Result`, `Error`, `Error::Message` |

`pub type Result<T> = core::result::Result<T, Error>` and `Error` is a `#[derive(thiserror::Error)]` enum whose one variant is `Message(String)`. Downstream crates compose against it with `#[from]` and propagate with `?`.

## Shared references

| What | File | Symbols |
|---|---|---|
| The read-shared handle alias | `lib.rs` | `Ref` |

`pub type Ref<T> = std::sync::Arc<T>`. It is a *readability* alias only — the default for a value that is constructed once and then read through every shared handle (loaded meshes, textures, materials). A shared-*mutable* site does not use `Ref`; it spells `Arc<Mutex<T>>` (or `Arc<RwLock<T>>`) explicitly where it occurs, so the exception is visible.

## Identity

| What | File | Symbols |
|---|---|---|
| The stable 64-bit id newtype | `uuid.rs` | `Uuid`, `Uuid::new`, `Uuid::value` |

`pub struct Uuid(pub u64)`. ECS handles are not stable across a load, so anything serialized carries a `Uuid` instead. `Uuid::new()` mints a fresh id uniformly drawn from `[1024, u64::MAX]` (ids below `1024` are reserved for built-in assets); `value()` returns the raw `u64`. On the JSON wire a `Uuid` is a **decimal string** (ids span the full `u64` range past JavaScript's `2^53` safe-integer limit); the read side accepts a string or a number. That encoding lives once in `saffron-protocol`, not on this newtype.

## Time

| What | File | Symbols |
|---|---|---|
| A span of time in seconds | `time.rs` | `TimeSpan`, `TimeSpan::from_seconds`, `TimeSpan::to_milliseconds` |

`pub struct TimeSpan { pub seconds: f32 }`. `from_seconds` constructs one; `to_milliseconds` returns `seconds * 1000.0`. Both are `const fn`.

## Logging

Every line is `[saffron:<subsystem>] <message>`, with `warn:` / `error:` ahead of the message for the non-info levels. The format is frozen — the validation-clean-log gate parses it.

| What | File | Symbols |
|---|---|---|
| Log level + sink | `log.rs` | `LogLevel`, `LogLevel::{Info, Warn, Error}`, `log`, `subsystem_of` |
| Tagged macros | `log.rs` | `log_info!`, `log_warn!`, `log_error!` |

`log(level, subsystem, message)` prints the frozen line. The `log_info!` / `log_warn!` / `log_error!` macros take a `format!` argument list and derive the subsystem tag from the caller's `module_path!()` (`saffron_core::uuid` → `core`, `saffron_rendering` → `rendering`); a path without the `saffron_` prefix falls back to `engine`. `subsystem_of` is the exposed mapping function.

## Identity constants

| What | File | Symbols |
|---|---|---|
| Engine name and version | `lib.rs` | `ENGINE_NAME` (`"Saffron Anima"`), `ENGINE_VERSION` (`"0.1.0-vulkan"`) |

## Related

- [Error handling](../../explanations/core-and-conventions/error-handling/) — the `Result`/`Error` scheme
- [Type aliases and primitives](../../explanations/core-and-conventions/type-aliases-and-primitives/) — the foundation spellings
- [Ownership and RAII](../../explanations/core-and-conventions/ownership-and-raii/) — what `Ref<T>` points at
