+++
title = 'ImGui integration'
weight = 1
+++

# ImGui integration

The editor UI is Dear ImGui (the docking branch). An immediate-mode library fits an engine that already has a per-frame loop and a command-buffer seam: there is no retained widget tree to keep in sync with the scene. Two backends connect it to the engine — `imgui_impl_sdl3` feeds events from the window, and `imgui_impl_vulkan` records draw lists into the frame's command buffer. Both run through Vulkan 1.3 dynamic rendering, so ImGui draws straight into the swapchain image with no `VkRenderPass`.

## Bring-up

`newUi` builds everything in order: a descriptor pool, the context, then the two backends. Every fallible step is converted to a `Result` at the boundary, the way the rest of the engine works.

The pool holds 1000 each of combined-image-sampler, sampler, and sampled-image descriptors. That headroom lets the viewport texture and every asset thumbnail register as its own descriptor set (see [Viewport panel](../viewport-panel/) and [Mesh thumbnails](../mesh-thumbnails/)). The `eFreeDescriptorSet` flag matters: the viewport texture is freed and re-registered when the offscreen image is recreated, so the pool has to allow individual frees.

The SDL3 backend is wired to events by pushing a sink onto the window's raw event list rather than subclassing anything:

```cpp
window.eventSinks.push_back([](const SDL_Event& event) { ImGui_ImplSDL3_ProcessEvent(&event); });
```

`Window` fans every SDL event out to its `eventSinks`, so ImGui sees keyboard, mouse, and resize events without the window knowing ImGui exists.

## No render pass

The Vulkan backend is told `UseDynamicRendering = true` and handed the swapchain color format through a `PipelineRenderingCreateInfo` instead of a render pass. ImGui builds its pipeline once during `Init` from that format and never reads the format pointer again. It always draws to the 1-sample swapchain even when the scene uses MSAA, because the scene is already resolved into the offscreen texture before ImGui samples it.

## Recording draw data

ImGui's draw data goes into the frame through the renderer's UI seam, not by calling Vulkan from the UI module:

```cpp
void uiRecordDrawData(Renderer& renderer)
{
    submitUi(renderer, [](vk::CommandBuffer cmd)
    {
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), static_cast<VkCommandBuffer>(cmd));
    });
}
```

`submitUi` queues the closure; the [render graph](../../frame-and-render-graph/render-graph-overview/) replays it inside the UI pass targeting the swapchain. The frame is two logical halves: the scene renders to the offscreen, then ImGui (with the offscreen shown as the viewport texture) renders to the swapchain.

## Teardown order

`destroyUi` waits for the device to idle, removes the viewport texture, shuts down the two backends, destroys the context, then destroys the pool. The device-idle first is what makes the rest safe — nothing ImGui owns is still in flight on the GPU when its descriptors and pool go away.

## In the code

| What | File | Symbols |
|---|---|---|
| Setup + teardown | `ui.cppm` | `newUi`, `destroyUi`, `Ui` |
| Descriptor pool | `ui.cppm` | `poolInfo` (1000 sets, `eFreeDescriptorSet`) |
| Backend init | `ui.cppm` | `ImGui_ImplSDL3_InitForVulkan`, `ImGui_ImplVulkan_Init` |
| Event feed | `ui.cppm` | `window.eventSinks.push_back(...)` |
| Frame begin/end | `ui.cppm` | `uiBeginFrame`, `uiEndFrame` |
| Draw-data record | `ui.cppm` | `uiRecordDrawData`, `submitUi` |

## Related

- [Viewport panel](../viewport-panel/) — how the scene texture is shown
- [Theme & fonts](../theme-and-fonts/) — the dark theme + dock layout seeded at bring-up
- [Dynamic rendering](../../vulkan-foundation/dynamic-rendering/) — the no-render-pass model ImGui rides on
- [Render graph](../../frame-and-render-graph/render-graph-overview/) — where the UI pass is replayed
