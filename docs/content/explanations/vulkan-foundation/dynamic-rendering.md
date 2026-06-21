+++
title = 'Dynamic rendering'
weight = 5
+++

# Dynamic rendering

Dynamic rendering is a Vulkan 1.3 feature that binds attachments at command-record time instead of through
pre-built `VkRenderPass` and `VkFramebuffer` objects. A pass becomes a closure plus a list of image views,
opened and closed with `cmd_begin_rendering` / `cmd_end_rendering`.

The renderer requires dynamic rendering at device selection and uses it exclusively; there is no
render-pass or framebuffer object anywhere. This suits a pass set that changes per frame: shadows,
G-buffer, AO, SSGI, DDGI, and ReSTIR passes come and go with toggles, and each one only needs its
attachment views supplied at the moment it records.

## Recording a pass

The render graph's `execute` records the whole frame. For each graphics pass, `record_graphics` builds the
attachment infos, opens a rendering scope, runs the body, and closes the scope:

```rust
raw.cmd_begin_rendering(cmd, &rendering);
raw.cmd_set_viewport(cmd, 0, &[viewport]);
raw.cmd_set_scissor(cmd, 0, &[scissor]);
// body(cmd) records the pass
raw.cmd_end_rendering(cmd);
```

A compute pass (`RgPassKind::Compute`) has no attachments, so it skips the rendering scope and records its
body directly after its barriers.

## Attachment infos

`vk::RenderingInfo` is filled fresh each pass from the graph's tracked image views. Each color attachment
becomes a `vk::RenderingAttachmentInfo` whose load/store ops and clear value come from the pass's declared
[`RgAttachment`](../../frame-and-render-graph/passes-and-attachments/). The layout is always
`COLOR_ATTACHMENT_OPTIMAL` because the
[render graph](../../frame-and-render-graph/render-graph-overview/) has already emitted the barrier that
put the image there. Dynamic rendering does *not* transition layouts, so the graph owns that.

Several color attachments go into one `color_attachments` call. This is how the
[thin G-buffer](../../screen-space-and-post/thin-gbuffer/) writes color and normal targets from one MRT
pass. A depth attachment, when present, uses `depth_attachment` and `DEPTH_ATTACHMENT_OPTIMAL`.

## MSAA resolve in the attachment

[MSAA](../../anti-aliasing/msaa/) resolve is part of the attachment info rather than a separate pass. When
an attachment declares a `resolve` target, the attachment gets a resolve mode and view, and the
multisampled image is averaged down into the 1× target at end-of-pass:

```rust
if let Some(resolve) = att.resolve {
    info = info
        .resolve_mode(vk::ResolveModeFlags::AVERAGE)
        .resolve_image_view(self.resources[resolve.index as usize].view)
        .resolve_image_layout(vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL);
}
```

Color attachments resolve with `AVERAGE`; a depth attachment resolves with `SAMPLE_ZERO`. Under a render
pass this would be a resolve attachment in the subpass description; with dynamic rendering it is two fields
on the attachment info.

## Dynamic viewport and scissor

Pipelines are built with `VIEWPORT` and `SCISSOR` as dynamic state
(`PipelineDynamicStateCreateInfo`), and each pipeline's `vk::PipelineRenderingCreateInfo` names its color
and depth attachment formats (`OFFSCREEN_COLOR_FORMAT`, `DEPTH_FORMAT`) in place of a render pass. After
opening the scope, every graphics pass sets a full-area viewport and scissor from the pass extent, so the
same pipeline draws at any size without a rebuild — which matters because the offscreen target resizes
with the editor's viewport panel.

## In the code

| What | File | Symbols |
|---|---|---|
| Begin/end rendering | `render_graph.rs` | `record_graphics`, `cmd_begin_rendering`, `cmd_end_rendering` |
| Attachment infos | `render_graph.rs` | `RenderingInfo`, `RenderingAttachmentInfo` |
| MSAA resolve | `render_graph.rs` | `resolve_mode`, `resolve_image_view` |
| Dynamic viewport/scissor + formats | `pipelines.rs` | `DynamicState::VIEWPORT`, `PipelineRenderingCreateInfo`, `OFFSCREEN_COLOR_FORMAT`, `DEPTH_FORMAT` |
| Feature enable | `device.rs` | `create_logical_device` (`features13.dynamic_rendering`) |

## Related

- [Render graph overview](../../frame-and-render-graph/render-graph-overview/) — what calls `cmd_begin_rendering` per pass
- [Passes and attachments](../../frame-and-render-graph/passes-and-attachments/) — the `RgAttachment` the infos are built from
- [Barriers](../synchronization2-and-barriers/) — the layout transitions dynamic rendering does *not* do for you
- [MSAA](../../anti-aliasing/msaa/) — the resolve target folded into the attachment
