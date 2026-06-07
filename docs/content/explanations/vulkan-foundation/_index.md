+++
title = 'Vulkan foundation'
weight = 3
bookCollapseSection = true
+++

# Vulkan foundation

The Vulkan foundation is the low-level graphics layer the renderer sits on. It uses Vulkan-Hpp with exceptions off, so every call returns a checked `Result`. It targets Vulkan 1.4, which has no render-pass or framebuffer objects.

## Pages

| Page | Covers | Code |
|---|---|---|
| `vulkan-hpp-no-exceptions` | `VULKAN_HPP_NO_EXCEPTIONS`, the `checked()` conversion to `Result` | `renderer.cppm`, `renderer_detail.cppm` |
| `device-and-swapchain` | vk-bootstrap device/swapchain selection, feature negotiation | `renderer.cppm` |
| `vma-allocator` | VMA setup, allocation, the single impl TU | `renderer.cppm`, `vma_impl.cpp` |
| `synchronization2-and-barriers` | `vk::…Barrier2`, stage/access masks, layout transitions | `renderer_detail.cppm` |
| `dynamic-rendering` | `beginRendering`, attachment infos, no passes/framebuffers | `render_graph.cppm` |
| `frame-sync-and-resize` | `MaxFramesInFlight`, per-image fences, swapchain recreation | `renderer_types.cppm`, `renderer.cppm` |
| `meta-layer-resources` | move-only `Pipeline`/`Image`/`Buffer`/`GpuMesh`/`GpuTexture`, `Ref` ownership | `renderer_types.cppm` |
