+++
title = 'Vulkan foundation'
weight = 3
bookCollapseSection = true
+++

# Vulkan foundation

The Vulkan foundation is the low-level graphics layer the renderer sits on. The `saffron-rendering` crate
binds Vulkan through `ash` (with VMA via `vk-mem`), turning every fallible call into a typed `Result`. It
targets Vulkan 1.3 — dynamic rendering and synchronization2, no render-pass or framebuffer objects.

## Pages

| Page | Covers | Code |
|---|---|---|
| `vulkan-hpp-no-exceptions` | the `ash` seam, the `Error::Vk` enum, the `checked` conversion to a typed `Result` | `lib.rs` |
| `device-and-swapchain` | hand-rolled device selection, feature negotiation, surface-source split, swapchain build | `device.rs`, `swapchain.rs` |
| `vma-allocator` | VMA via `vk-mem`, allocation, the shared device+allocator bundle | `device.rs`, `resources.rs` |
| `synchronization2-and-barriers` | `vk::…Barrier2`, stage/access masks, layout transitions | `render_graph.rs`, `upload.rs` |
| `dynamic-rendering` | `cmd_begin_rendering`, attachment infos, no passes/framebuffers | `render_graph.rs`, `pipelines.rs` |
| `frame-sync-and-resize` | `MAX_FRAMES_IN_FLIGHT`, per-image fences, viewport + swapchain recreation | `frame.rs`, `swapchain.rs`, `renderer.rs` |
| `meta-layer-resources` | RAII `Buffer`/`Image`/`GpuMesh`/`GpuTexture`/`Pipeline`, `Arc` sharing + the bundle | `resources.rs` |
