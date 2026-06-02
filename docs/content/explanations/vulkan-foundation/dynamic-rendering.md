+++
title = 'Dynamic rendering'
weight = 5
+++

# Dynamic rendering

Dynamic rendering is a Vulkan 1.3 feature that binds attachments at command-record time instead of through pre-built `VkRenderPass` and `VkFramebuffer` objects. A pass becomes a closure plus a list of image views, opened and closed with `beginRendering` / `endRendering`.

The engine targets Vulkan 1.4 and uses dynamic rendering exclusively; there is no render-pass or framebuffer object anywhere. This suits a pass set that changes per frame: shadows, G-buffer, AO, SSGI, DDGI, and ReSTIR passes come and go with toggles, and each one only needs its attachment views supplied at the moment it records.

## Recording a pass

`executeRenderGraph` records the whole frame. For each graphics pass it builds the attachment infos, opens a rendering scope, runs the body, and closes the scope:

```cpp
cmd.beginRendering(rendering);
// set viewport/scissor, run pass.execute(cmd)
cmd.endRendering();
```

A compute pass has no attachments, so it skips the rendering scope and runs its body after its barriers.

## Attachment infos

`vk::RenderingInfo` is filled fresh each pass from the graph's tracked image views. Each color attachment becomes a `vk::RenderingAttachmentInfo` whose load/store ops and clear value come from the pass's declared [`RgAttachment`](../../frame-and-render-graph/passes-and-attachments/). The layout is always `eColorAttachmentOptimal` because the [render graph](../../frame-and-render-graph/render-graph-overview/) has already emitted the barrier that put the image there. Dynamic rendering does *not* transition layouts, so the graph owns that.

Several color attachments go into one `setColorAttachments` call. This is how the [thin G-buffer](../../screen-space-and-post/thin-gbuffer/) writes color and normal targets from one MRT pass. A depth attachment, when present, uses `setPDepthAttachment` and `eDepthAttachmentOptimal`.

## MSAA resolve in the attachment

[MSAA](../../anti-aliasing/msaa/) resolve is part of the attachment info rather than a separate pass. When a color attachment declares a `resolve` target, the attachment gets a resolve mode and view, and the multisampled image is averaged down into the 1× target at end-of-pass:

```cpp
if (att.resolve)
{
    colorInfo.resolveMode = vk::ResolveModeFlagBits::eAverage;
    colorInfo.resolveImageView = graph.resources[att.resolve->index].view;
    colorInfo.resolveImageLayout = vk::ImageLayout::eColorAttachmentOptimal;
}
```

Under a render pass this resolve would be a resolve attachment in the subpass description; with dynamic rendering it is two fields on the attachment info.

## Dynamic viewport and scissor

After opening the scope, every graphics pass sets a full-area viewport and scissor from the pass extent. The pipelines are built with dynamic viewport/scissor state, so the same pipeline draws at any size without a rebuild. This matters because the offscreen target resizes with the editor panel.

## In the code

| What | File | Symbols |
|---|---|---|
| Begin/end rendering | `render_graph.cppm` | `executeRenderGraph`, `beginRendering`, `endRendering` |
| Attachment infos | `render_graph.cppm` | `vk::RenderingInfo`, `vk::RenderingAttachmentInfo` |
| MSAA resolve | `render_graph.cppm` | `resolveMode`, `resolveImageView` |
| Dynamic viewport/scissor | `render_graph.cppm` | `setViewport`, `setScissor` |
| Feature enable | `renderer.cppm` | `features13.dynamicRendering` |

## Related

- [Render graph overview](../../frame-and-render-graph/render-graph-overview/) — what calls `beginRendering` per pass
- [Passes and attachments](../../frame-and-render-graph/passes-and-attachments/) — the `RgAttachment` the infos are built from
- [Barriers](../synchronization2-and-barriers/) — the layout transitions dynamic rendering does *not* do for you
- [MSAA](../../anti-aliasing/msaa/) — the resolve target folded into the attachment
