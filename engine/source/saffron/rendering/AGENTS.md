# Saffron.Rendering

The Vulkan renderer: swapchain, the render graph, the deferred submit seam, and every
GPU resource wrapper. Module `Saffron.Rendering`, namespace `se`. This module uses
classic `#include` in the global module fragment (Vulkan-Hpp, VMA, vk-bootstrap, glm,
stb, nanosvg) and **does NOT `import std`** — mixing the std module with these headers
breaks the TU. Consumers still import it normally; the BMI carries the std types.

## Files

| File | Role |
|---|---|
| `renderer.cppm` | Main interface: lifecycle (`newRenderer`/`destroyRenderer`/`beginFrame`/`endFrame`), the submit seam, graph building, scene/lighting/sky/RT/DDGI entry points. Re-exports the partitions. |
| `renderer_types.cppm` | Partition `:Types`. All state structs (`Renderer`, `Swapchain`, …) and the move-only RAII wrappers (`Image`, `Image3D`, `Buffer`, `GpuMesh`, `GpuTexture`, `Pipeline`, `AccelerationStructure`). |
| `render_graph.cppm` | Partition `:RenderGraph`. `RgUsage`, `RgPass`, `RgAttachment`, `RgAccess`, `RgResource`, barrier/layout derivation (`usageInfo`/`applyAccess`), `addPass`/`executeRenderGraph`. |
| `renderer_detail.cppm` | Partition `:Detail` — **internal**. Low-level Vulkan helpers, shader loading, descriptor setup, pass recording. Do not expose to the app layer. |
| `renderer_drawlist.cpp` | Mesh upload, instanced batching, scene/depth-prepass recording. |
| `renderer_textures.cpp` | Texture upload, bindless slot allocation, SVG rasterization. |
| `renderer_pipelines.cpp` | PSO creation + cache (`requestMeshPipeline`). |
| `renderer_lighting.cpp` | Light UBOs, clusters, shadow setup. |
| `renderer_thumbnail.cpp` / `renderer_capture.cpp` / `renderer_aa.cpp` | Asset thumbnails, screenshot capture, AA toggles. |

## Rules that are easy to break

- **Vulkan-Hpp with `VULKAN_HPP_NO_EXCEPTIONS` and no smart handles.** Every `vk::`
  call returns a result — wrap it with `checked(...)` and propagate the `Result<T>` on
  the spot. **No `vk::raii`** (it throws). Ray-tracing entry points are resolved through
  function pointers on the context, not statically linked.
- **The render graph derives all barriers and layout transitions. Never write a
  `pipelineBarrier` by hand.** A pass *declares* what it touches: `colors`, `depth`, and
  non-attachment `accesses` (each an `RgUsage` — `ColorWrite`, `DepthWrite`, `SampledRead`,
  `StorageImageRWCompute`, `StorageReadCompute`, …). A missing declaration means a missing
  barrier, which is a data race or corruption, not a compile error. Layouts persist across
  frames via the `externalLayout` write-back pointer set when importing an image.
- **The submit seam is deferred + render-thread only.** `submit(renderer, [](cmd){…})`
  and `submitUi(...)` stash closures that are replayed inside the matching pass during
  `endFrame`. Closures capture `Renderer` by reference; they run sequentially while the
  command buffer is recording. Do not touch renderer state from other threads, and capture
  `Ref<T>` handles (not raw pointers) for anything that must outlive the call.
- **Drop order on teardown.** Data-plane resources are `Ref<T> = shared_ptr<T>`. Clients
  drop their refs in `onExit`; `run` calls `waitGpuIdle` first, and `destroyRenderer` frees
  internal resources before `vmaDestroyAllocator` / device destroy. Anything still holding a
  `Ref` to an image/buffer past that point is a use-after-free.

## Adding a custom pass (from the app layer)

```cpp
RenderGraph& g = frameGraph(renderer);   // after beginFrameGraph
RgPass pass;
pass.name = "my-pass";
pass.kind = RgPassKind::Compute;
pass.accesses = { /* RgAccess{ resource, RgUsage::StorageImageRWCompute }, … */ };
pass.execute = [&](vk::CommandBuffer cmd) { /* bind + dispatch */ };
addPass(g, std::move(pass));             // executed in order by endFrame
```

A feature that adds inspectable/drivable state also needs a `registerCommand` in
`Saffron.Control` and a `docs/content/` explanation update — see the root `AGENTS.md`.
