+++
title = 'The Cargo workspace and crate model'
weight = 2
+++

# The Cargo workspace and crate model

The engine is a Cargo workspace: a set of library crates under `engine/crates/`, plus the `xtask`
tooling crate, sharing one `Cargo.lock`, one resolver, and one place where every third-party
version is pinned. Each engine *area* is its own crate (`saffron-core`, `saffron-rendering`,
`saffron-scene`, …); the crate boundary is the real boundary between areas, and a crate exports an
explicit public surface that consumers reach through `use`.

The workspace is the unit the whole build operates on. `cargo build --workspace` builds every
member, `cargo test --workspace` tests every member, and the dependency edges between members are
the engine's architecture made mechanical — a crate can only call into the crates it lists in its
`Cargo.toml`.

## One workspace, one pin list

The root `engine/Cargo.toml` declares the members and pins every external dependency once under
`[workspace.dependencies]`. Member crates pull each dependency with `dep.workspace = true`, so a
version is written in exactly one place and never drifts across crates:

```toml
[workspace]
resolver = "3"
members = ["crates/*", "xtask"]

[workspace.dependencies]
ash = "=0.38"          # Vulkan, pinned exactly
glam = "0.30"          # math
hecs = "0.11"          # the ECS, wrapped by saffron-scene
serde = { version = "1.0", features = ["derive"] }
thiserror = "2"
```

`[workspace.package]` shares the `edition = "2024"`, `rust-version`, and `version` across every
crate, and `[workspace.lints]` applies the same `clippy` / `unsafe_code` policy everywhere a crate
opts in with `[lints] workspace = true`.

## A crate's public surface

Each crate is a library with a `lib.rs` root that names its internal modules and re-exports the
types that form its public API. `saffron-core` is the smallest complete example:

```rust
// crates/core/src/lib.rs
mod error;
mod log;
mod time;
mod uuid;

pub use error::{Error, Result};
pub use log::{LogLevel, log};
pub use uuid::Uuid;

pub type Ref<T> = Arc<T>;
```

Consumers write `use saffron_core::{Result, Ref};` — they see exactly what `lib.rs` re-exports and
nothing else. A type that is `pub` inside a private `mod` is not reachable until `lib.rs`
re-exports it, which is how a crate keeps its internal files private while presenting one curated
surface.

## Crate vs module

A **crate** is the architectural unit: it has a `Cargo.toml`, a dependency list, and a compilation
boundary. A **module** (`mod foo;` → `foo.rs`) is an organizational unit *inside* one crate — see
[how a crate organizes its modules](../module-partitions/). The dependency DAG that holds the
engine together is a graph of crates, not modules (see [the crate DAG](../module-dag/)).

| Concept | Spelled | Boundary |
|---|---|---|
| Crate | `engine/crates/<area>/` with a `Cargo.toml` | What a crate may depend on |
| Module | `mod name;` inside a crate | File-level organization within a crate |
| Re-export | `pub use` in `lib.rs` | The crate's public API |

## In the code

| What | File | Symbols |
|---|---|---|
| Workspace + pin list | `engine/Cargo.toml` | `[workspace]`, `members`, `[workspace.dependencies]` |
| Shared package metadata + lints | `engine/Cargo.toml` | `[workspace.package]`, `[workspace.lints]` |
| A crate manifest pulling pins | `crates/rendering/Cargo.toml` | `ash.workspace = true`, `saffron-core = { path = ... }` |
| A crate's public surface | `crates/core/src/lib.rs` | `pub use`, `pub type Ref<T>` |

## Related
- [How a crate organizes its modules](../module-partitions/) — files inside one crate
- [The crate DAG](../module-dag/) — how the crates depend on each other
- [Build environment](../build-environment/) — the toolbox that runs `cargo`
- [Dependencies](../dependencies/) — the pins under `[workspace.dependencies]`
