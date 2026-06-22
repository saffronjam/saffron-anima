+++
title = 'How a crate organizes its modules'
weight = 3
+++

# How a crate organizes its modules

A large crate is not one file. It is split into many module files under `src/`, each declared with
`mod name;` in the crate root, and the root re-exports the curated public surface with `pub use`.
The module files are private organization; the `pub use` block is the crate's API. This is how a
crate like `saffron-rendering` carries thirty-odd feature files behind one tidy import.

The split is by feature, one module per concern, and it costs nothing at the boundary: modules in
one crate share a single compilation unit, so a call from one module file to another is an ordinary
function call with no extra ceremony. The only thing the crate root decides is what escapes.

## Module files and the re-export root

`saffron-rendering` is one crate spread across many files under `crates/rendering/src/`. Each
feature is its own module file — `pipelines.rs`, `lighting.rs`, `aa.rs`, `render_graph.rs`,
`ssao.rs`, and so on — declared privately in `lib.rs`:

```rust
// crates/rendering/src/lib.rs
mod lighting;
mod pipelines;
mod render_graph;
mod renderer;
mod resources;
// … ~30 module files

pub use render_graph::{RenderGraph, RgPass, RgUsage};
pub use renderer::{Renderer, ViewId, ViewMode};
pub use resources::{Buffer, Image, GpuMesh};
```

Every `mod` is private, so the module files are internal by default. A type is reachable to
consumers only when `lib.rs` re-exports it with `pub use`. A consumer writes
`use saffron_rendering::{Renderer, RenderGraph};` and sees exactly the curated surface — the feature
files that produced those types stay invisible.

Items a module file needs from a *sibling* file are reached with `use crate::pipelines::Pipelines;`
(an internal path), distinct from the `pub use` that publishes to the outside world. Internal
visibility lives between the two: a helper used across sibling modules but not exported is `pub(crate)`,
visible crate-wide but absent from the public API.

## Where the file lines fall

The division is by responsibility, not by size cap:

- The orchestration file (`renderer.rs`) owns the top-level type (`Renderer`) and the frame entry
  points, and calls into the feature files.
- Each feature file owns one subsystem's types and logic — `lighting.rs` the clustered lighting,
  `ssao.rs` the ambient-occlusion pass, `render_graph.rs` the [`RgPass`/`RgUsage`
  graph](../../frame-and-render-graph/render-graph-overview/).
- A purely internal helper lives in the file with its sole caller and is never re-exported.

A nested module group (a `mod foo { ... }` block, or a `foo/` directory with a `mod.rs`) is used
when a feature has several closely-related files of its own — the same pattern one level down. The
crate root re-exports only the public leaves either way.

## In the code

| What | File | Symbols |
|---|---|---|
| Private module files | `crates/rendering/src/lib.rs` | `mod lighting;`, `mod pipelines;`, … |
| The re-export surface | `crates/rendering/src/lib.rs` | `pub use renderer::{Renderer, ...}` |
| An orchestration module | `crates/rendering/src/renderer.rs` | `Renderer`, `submit`, the frame entry points |
| A feature module | `crates/rendering/src/render_graph.rs` | `RenderGraph`, `RgPass`, `RgUsage` |

## Related
- [The Cargo workspace and crate model](../cargo-workspace/) — crates vs modules
- [The crate DAG](../module-dag/) — where the rendering crate sits in the graph
- [Render graph overview](../../frame-and-render-graph/render-graph-overview/) — the `RgPass` API
