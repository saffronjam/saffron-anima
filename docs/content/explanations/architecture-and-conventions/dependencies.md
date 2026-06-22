+++
title = 'Dependencies'
weight = 7
+++

# Dependencies

A third-party dependency is a crate the engine consumes but does not author. Cargo resolves and
builds them from source against one `Cargo.lock`, and every version is pinned in exactly one place —
`[workspace.dependencies]` in `engine/Cargo.toml` — so a version never drifts between the crates
that use it.

The point of the single pin list is that a bump happens once. A member crate never writes a version
number; it writes `dep.workspace = true` and inherits the pin. Escalating a choice (say, swapping the
ECS) stays a one-line change in the workspace manifest plus the one crate that wraps it.

## One pinned set

`[workspace.dependencies]` declares each external crate and its version (and default feature set).
Member manifests pull each with `dep.workspace = true`:

```toml
# engine/Cargo.toml
[workspace.dependencies]
ash = "=0.38"          # Vulkan bindings, pinned exactly
vk-mem = "0.4"         # VMA allocator
winit = "0.30"         # windowing
hecs = "0.11"          # the ECS, wrapped by saffron-scene
glam = "0.30"          # math
serde = { version = "1.0", features = ["derive"] }
serde_json = { version = "1.0", features = ["preserve_order"] }
thiserror = "2"
anyhow = "1"
```

```toml
# crates/rendering/Cargo.toml
[dependencies]
ash.workspace = true
vk-mem.workspace = true
```

A few pins carry intent beyond the version. `ash = "=0.38"` is pinned *exactly* to dodge Vulkan
binding churn. `serde_json`'s `preserve_order` feature makes result objects emit keys in
DTO-declaration order, so the control protocol's generated artifacts reproduce byte-for-byte. `hecs`
is named only inside `saffron-scene`, which wraps it behind `Scene`/`Entity` so the ECS choice is a
single-crate decision.

## The major dependencies

| Area | Crate(s) | Role |
|---|---|---|
| Vulkan | `ash`, `ash-window`, `raw-window-handle`, `vk-mem` | the `vk::` API surface, surface creation, VMA allocation |
| Windowing | `winit` | the OS window + event loop |
| ECS | `hecs` | the world behind `saffron-scene` |
| Math | `glam` | vectors/matrices, `bytemuck`-castable to GPU layout |
| Scripting | `mlua` (Luau, vendored) | the per-entity script VM |
| Serde stack | `serde`, `serde_json`, `serde_with`, `schemars`, `ts-rs` | JSON, the wire DTOs, schema + TypeScript codegen |
| Physics FFI | `cxx`, `cxx-build`, `cc` | the vendored Jolt 5.3.0 bridge in `saffron-physics-sys` |
| Import / images | `gltf`, `tobj`, `image`, `resvg`/`usvg`/`tiny-skia` | glTF/OBJ import, texture decode, SVG icon raster |
| Syscalls / CLI | `rustix`, `clap`, `clap_complete`, `walkdir` | shm/socket syscalls, the `sa` CLI, the asset-catalog scan |
| GPU casts / blobs | `bytemuck`, `base64` | struct→bytes for upload, control-protocol blobs |
| Errors | `thiserror`, `anyhow` | typed library errors; tooling `anyhow` |

`bytemuck` features on `glam` give the engine math types a zero-copy cast into the GPU struct
layout; `glam`'s column-major matrices pair with the `-matrix-layout-column-major` shader flag (see
[shader compilation](../shader-compilation/)) so a CPU transform reaches the shader unchanged.

## FFI and unsafe

`unsafe_code = "deny"` holds workspace-wide. The two crates that must cross a language boundary
declare the exception locally: `saffron-rendering` calls `ash`'s Vulkan entry points, and
`saffron-physics-sys` builds the vendored Jolt C++ through a `#[cxx::bridge]` (`build.rs` drives
`cxx-build` + `cc`). Both keep the `unsafe` confined to the FFI seam.

## In the code

| What | File | Symbols |
|---|---|---|
| The pin list | `engine/Cargo.toml` | `[workspace.dependencies]`, `[workspace.lints]` |
| A crate pulling pins | `crates/rendering/Cargo.toml` | `ash.workspace = true`, `vk-mem.workspace = true` |
| The ECS wrap | `crates/scene/Cargo.toml`, `crates/scene/src/scene.rs` | `hecs.workspace = true`; `Scene`, `Entity` |
| The Jolt FFI sys crate | `crates/physics-sys/Cargo.toml` | `cxx`, `cxx-build`, `cc`, `build = "build.rs"` |

## Related
- [The Cargo workspace and crate model](../cargo-workspace/) — the workspace the pins live in
- [Build environment](../build-environment/) — the toolbox `cargo` that resolves them
- [Shader compilation](../shader-compilation/) — the `slangc` half of the toolchain
