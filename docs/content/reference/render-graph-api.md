+++
title = 'Render graph API'
weight = 3
math = false
+++

# Render graph API

The `Saffron.Rendering:RenderGraph` partition: usage enum, pass/attachment shapes, and the import/add/execute functions.

## `enum class RgUsage`
What a pass does with a non-attachment resource — the single source of truth for barrier derivation.

| Value | Stage | Access | Layout | Write |
|---|---|---|---|---|
| `ColorWrite` | ColorAttachmentOutput | ColorAttachmentWrite | ColorAttachmentOptimal | yes |
| `DepthWrite` | Early+LateFragmentTests | DepthStencilAttachmentWrite | DepthAttachmentOptimal | yes |
| `SampledRead` | FragmentShader | ShaderSampledRead | ShaderReadOnlyOptimal | no |
| `StorageWriteCompute` | ComputeShader | ShaderStorageWrite | (buffer) | yes |
| `StorageReadCompute` | ComputeShader | ShaderStorageRead | (buffer) | no |
| `StorageReadFragment` | FragmentShader | ShaderStorageRead | (buffer) | no |
| `StorageImageRWCompute` | ComputeShader | ShaderStorageRead+Write | General | yes |
| `SampledReadCompute` | ComputeShader | ShaderSampledRead | ShaderReadOnlyOptimal | no |

## `enum class RgPassKind`
| Value | Effect |
|---|---|
| `Graphics` | the graph opens a `beginRendering`/`endRendering` scope around the body |
| `Compute` | body runs bare (no rendering scope) |

## Structs
| Type | Fields |
|---|---|
| `RgResource` | `u32 index` — index into the graph resource table |
| `RgAccess` | `RgResource resource`; `RgUsage usage = SampledRead` |
| `RgAttachment` | `RgResource resource`; `vk::AttachmentLoadOp loadOp = eClear`; `vk::AttachmentStoreOp storeOp = eStore`; `vk::ClearValue clearValue`; `std::optional<RgResource> resolve` (MSAA resolve target, color only) |
| `RgPass` | `std::string name`; `RgPassKind kind = Graphics`; `std::vector<RgAccess> accesses`; `std::vector<RgAttachment> colors` (MRT: index 0 = location 0); `std::optional<RgAttachment> depth`; `vk::Extent2D renderArea`; `std::function<void(vk::CommandBuffer)> execute` |
| `RenderGraph` | `std::vector<RgResourceState> resources`; `std::vector<RgPass> passes` |

## Functions
| Symbol | Signature | Effect |
|---|---|---|
| `newRenderGraph` | `auto newRenderGraph() -> RenderGraph` | empty graph |
| `importImage` | `auto importImage(RenderGraph&, vk::Image, vk::ImageView, vk::ImageAspectFlags, vk::ImageLayout initialLayout, vk::ImageLayout* externalLayout) -> RgResource` | track an external 2D image; `externalLayout` seeds + receives the layout (cross-frame) |
| `importImage3D` | `auto importImage3D(RenderGraph&, vk::Image, vk::ImageView, vk::ImageLayout, vk::ImageLayout* externalLayout) -> RgResource` | track an external 3D image (DDGI voxel proxy) |
| `importBuffer` | `auto importBuffer(RenderGraph&, vk::Buffer) -> RgResource` | track an external buffer |
| `addPass` | `void addPass(RenderGraph&, RgPass)` | append a pass |
| `executeRenderGraph` | `void executeRenderGraph(RenderGraph&, vk::CommandBuffer)` | derive + emit barriers, record each body, write back cross-frame layouts |

## Related
- [Render graph](../../explanations/frame-and-render-graph/render-graph-overview/) — the model behind these types
- [Usage and barrier derivation](../../explanations/frame-and-render-graph/usage-and-barrier-derivation/) — how `RgUsage` becomes a barrier
- [Passes and attachments](../../explanations/frame-and-render-graph/passes-and-attachments/) — declaring a pass
