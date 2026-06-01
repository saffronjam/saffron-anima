+++
title = 'Viewport panel'
weight = 2
+++

# Viewport panel

The scene is not drawn to the window directly. It renders to an offscreen image, and that image shows inside a dockable ImGui window as a texture. This lets the scene live as one panel among others — resizable, dockable, and composited with the rest of the UI in a single pass. `viewportPanel` runs each frame: it sizes the offscreen to the panel, keeps the texture handle current, and draws it with `ImGui::Image`.

## Showing a GPU image as a texture

ImGui's Vulkan backend can sample any image view registered as a descriptor set. `ImGui_ImplVulkan_AddTexture` returns a handle usable as an `ImTextureID`. The layout passed in is `SHADER_READ_ONLY_OPTIMAL`, the layout the [render graph](../../frame-and-render-graph/render-graph-overview/) leaves the offscreen in at end of frame, exactly so ImGui can sample it. The handle is registered once in `newUi` and reused every frame.

## Refreshing the texture

The descriptor that `AddTexture` builds points at a specific `VkImageView`. When the panel resizes, the renderer recreates the offscreen at the new size, giving it a new view and invalidating the descriptor. The viewport tracks this with a generation counter rather than diffing sizes:

```cpp
if (viewportGeneration(renderer) != ui.knownViewportGeneration)
{
    if (ui.viewportTexture != 0)
        ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)ui.viewportTexture);
    ui.viewportTexture = (ImTextureID)ImGui_ImplVulkan_AddTexture(
        viewportImageView(renderer), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ui.knownViewportGeneration = viewportGeneration(renderer);
}
```

The old descriptor is freed and a new one created against the new view. This is safe because the recreate path issues a full device idle before the next frame's UI runs, so nothing is sampling the old view when it goes away. The counter means the refresh costs one integer compare in the common no-resize frame.

## One-frame-lag resize

The panel does not resize the image itself; it only requests a size via `setViewportDesiredSize`, scaled by `DisplayFramebufferScale` so the offscreen is sized in physical pixels on a HiDPI display while `ImGui::Image` gets the logical size. The renderer applies the desired size at the *start* of the next frame, before recording anything, so a resize takes effect one frame late. That lag is deliberate: recreating the offscreen mid-frame would tear down an image the in-flight command buffer might still reference. Deferring the recreate to a clean frame boundary avoids that.

## Capturing the rect for the gizmo

After drawing the image, the panel records its screen-space rectangle and hover state:

```cpp
ImGui::Image(ui.viewportTexture, avail);
ui.viewportPos = ImGui::GetItemRectMin();
ui.viewportSize = ImGui::GetItemRectSize();
ui.viewportHovered = ImGui::IsItemHovered();
```

The [gizmo](../gizmo/), the light/camera billboards, and click-pick all map between screen pixels and the rendered image, so they read this rect back through `viewportContentPos` / `viewportContentSize` / `viewportHovered`. The panel window is drawn with zero padding and no title bar so the image fills it edge to edge.

## In the code

| What | File | Symbols |
|---|---|---|
| The panel | `ui.cppm` | `viewportPanel` |
| Texture register/refresh | `ui.cppm` | `ImGui_ImplVulkan_AddTexture`, `knownViewportGeneration` |
| Resize request | `ui.cppm` | `setViewportDesiredSize`, `DisplayFramebufferScale` |
| Rect/hover readback | `ui.cppm` | `viewportContentPos`, `viewportContentSize`, `viewportHovered` |
| Generation source | `renderer.cppm` | `viewportGeneration`, `viewportImageView`, `targets.generation` |

## Related

- [Gizmo](../gizmo/) — reads the captured viewport rect to place the overlay
- [Selection](../selection/) — click-pick uses the same rect to build the ray
- [Render graph](../../frame-and-render-graph/render-graph-overview/) — leaves the offscreen in `ShaderReadOnly` for ImGui
