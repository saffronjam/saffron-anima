+++
title = 'Ash and the Vulkan seam'
weight = 1
+++

# Ash and the Vulkan seam

`ash` is a thin, unchecked Rust binding over the Vulkan C API. It exposes the raw entry points — `create_instance`, `acquire_next_image`, `queue_submit2`, the VMA FFI — as `unsafe` functions that hand back a `Result<T, vk::Result>`. The rendering crate is the one place in the engine that crosses this seam: every other crate denies `unsafe`, and `saffron-rendering` opts in with a crate-wide `#![allow(unsafe_code)]` because there is no safe way to call a C binding. The seam is confined to the `device`, `swapchain`, and `renderer` modules and wrapped in safe methods (`Device::new`, `Swapchain::new`, `Renderer::render_frame`), so no caller of the crate ever touches a raw handle.

A Vulkan failure stays a value, never a panic: it becomes a typed [`Error`](../../core-and-conventions/error-handling/) at the call site, exactly like a file-parse or JSON error elsewhere in the engine.

## The error type

Bring-up and per-frame failures are one `thiserror` enum, `Error`. The load-bearing variant is `Error::Vk`, which carries the failing operation's name and the raw `vk::Result`:

```rust
#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("vulkan call '{context}' failed: {result:?}")]
    Vk { context: &'static str, result: vk::Result },
    // Loader, NoDevice, NoQueueFamily, EmptyMesh, …
}

pub type Result<T> = std::result::Result<T, Error>;
```

Keeping the raw `vk::Result` in the variant lets a caller `match` on the exact code — which is what the swapchain path needs, where `ERROR_OUT_OF_DATE_KHR` means "rebuild," not "fail" (see [frame sync](../frame-sync-and-resize/)).

## The `checked` conversion

One free function maps an ash call's `Result<T, vk::Result>` into the engine's typed error, tagging the operation that failed:

```rust
pub(crate) fn checked<T>(
    result: std::result::Result<T, vk::Result>,
    context: &'static str,
) -> Result<T> {
    result.map_err(|result| Error::Vk { context, result })
}
```

This is the single point that maps the ash seam onto the engine error model. A fallible call reads as `checked(unsafe { … }, "create_swapchain")?` — the `unsafe` block does the FFI, `checked` attaches the label, and `?` propagates. The message a caller sees is the `context` label plus the `vk::Result`, enough to locate a failure without a bespoke error enum per call.

Some sites skip `checked` and build the variant inline — `instance.create_device(…).map_err(|result| Error::Vk { context: "create_device", result })?` — which is the same mapping written by hand where the `context` is more naturally placed next to the call.

## Where the seam is widened, not narrowed

A few flows need the raw `vk::Result` even on a non-success code, so they match it directly instead of going through `checked`:

- **Acquire and present.** `acquire_next_image` and `queue_present` return `ERROR_OUT_OF_DATE_KHR` or `SUBOPTIMAL_KHR`, which mean "the swapchain must be rebuilt." `render_frame` matches these to skip the frame and signal a rebuild, treating only other codes as `Error::Vk`.
- **Loader load.** `ash::Entry::load` fails with its own error type (no `libvulkan` / no ICD); it maps to `Error::Loader`, not `Error::Vk`.

## Raw handles and `Drop`

ash hands back plain Vulkan handles (`vk::Buffer`, `vk::Image`, `vk::Pipeline`); it does not own them. Ownership is the engine's job, paid by [move-only RAII wrappers](../meta-layer-resources/) whose `Drop` bodies call the matching ash/VMA destroy function. The wrappers hold a shared device+allocator bundle so a resource can free itself in its own `Drop` without a live `&Device`.

## In the code

| What | File | Symbols |
|---|---|---|
| The crate-wide unsafe opt-in | `lib.rs` | `#![allow(unsafe_code)]` |
| The error enum + alias | `lib.rs` | `Error`, `Error::Vk`, `Result` |
| The conversion | `lib.rs` | `checked` |
| Acquire/present (code-not-failure) | `renderer.rs` | `render_frame` |
| Loader load | `device.rs` | `Device::new`, `Error::Loader` |

## Related

- [Error handling](../../core-and-conventions/error-handling/) — the engine-wide `Result` scheme `checked` feeds into
- [Meta-layer resources](../meta-layer-resources/) — the wrappers that own ash's raw handles and free them in `Drop`
- [Frame sync](../frame-sync-and-resize/) — where acquire/present results are rebuild signals, not errors
