+++
title = 'Barriers'
weight = 4
+++

# Barriers

A barrier is a Vulkan command that orders GPU work and transitions image layouts. The application owns this responsibility; the driver synchronizes almost nothing on its own.

The engine expresses every barrier with `synchronization2` — the `vk::…Barrier2` family submitted through `pipelineBarrier2`. The legacy single-barrier API is used nowhere. The feature is required at device selection, so every barrier in the engine is a 2-style barrier.

## How a barrier works

A `synchronization2` barrier carries a source and destination scope, each a `{ stage, access }` pair, and for images an old and new layout. The stage says *when* in the pipeline the work happens; the access says *what kind* of memory access it is. A barrier finishes the source scope's work and makes its writes visible before the destination scope reads or writes.

## Where barriers come from

Almost all barriers come from the [render graph](../../frame-and-render-graph/render-graph-overview/), which derives them from declared resource usage. The few that don't fit the graph are hand-written with one helper, `transitionImage` in `renderer_detail.cppm`. It builds a single `vk::ImageMemoryBarrier2`, wraps it in a `vk::DependencyInfo`, and submits it.

The helper covers cases with no graph resource to declare against: the final swapchain transition in `endFrame` (`ColorAttachmentOptimal → PresentSrcKHR`), texture-upload staging copies, IBL baking, and point-shadow cube faces, which manage their own layouts because a 6-layer cube exceeds the graph's single-layer barriers.

## Two barrier shapes

`synchronization2` has two relevant barrier types and the engine uses both:

- **`vk::ImageMemoryBarrier2`** — for images. Carries a layout transition *and* the memory dependency. An image can need a barrier purely to change layout, even with no data hazard.
- **`vk::MemoryBarrier2`** — for buffers and global memory. No layout, just the source→destination scope. A buffer only ever needs a barrier on a real data hazard.

Both batch into one `vk::DependencyInfo` and submit with a single `pipelineBarrier2`. A pass touching several resources pays for one barrier call.

## Stage and access masks

The vocabulary of `{ stage, access, layout }` triples lives in the render graph's `usageInfo` — the single table mapping a declared usage to its masks. A few representative rows:

| Usage | Stage | Access | Layout |
|---|---|---|---|
| Color write | `eColorAttachmentOutput` | `eColorAttachmentWrite` | `eColorAttachmentOptimal` |
| Depth write | `eEarlyFragmentTests \| eLateFragmentTests` | `eDepthStencilAttachmentWrite` | `eDepthAttachmentOptimal` |
| Sampled in fragment | `eFragmentShader` | `eShaderSampledRead` | `eShaderReadOnlyOptimal` |
| Storage image RW (compute) | `eComputeShader` | `eShaderStorageRead \| eShaderStorageWrite` | `eGeneral` |

Depth write spans both early and late fragment tests because depth is read and written across both. A storage image written in place by a compute shader lives in `eGeneral`, the layout that allows read and write. The tonemap and FXAA passes transition the offscreen to `General` before touching it for that reason.

## When a barrier fires

The graph's `applyAccess` decides, per resource, whether a barrier is needed. The data-hazard test is one line:

```cpp
const bool hazard = (target.isWrite && r.touched) || (!target.isWrite && r.lastWasWrite);
```

A write after any prior touch is a hazard (WAW or WAR); a read after a write is a hazard (RAW); read-after-read is not. An image has a second trigger: a layout mismatch emits a barrier even without a data hazard, because the layout must change before the GPU can use the image the new way. Buffers have no layout, so they barrier on the hazard alone. The full derivation is in [usage and barrier derivation](../../frame-and-render-graph/usage-and-barrier-derivation/).

## Why synchronization2

The original barrier API split stage and access into separate top-level fields and could not express per-barrier stage masks cleanly. `synchronization2` folds stage and access into one scope per side, lets image and buffer barriers share a `DependencyInfo`, and pairs with [dynamic rendering](../dynamic-rendering/); both are Vulkan 1.3 core. Targeting 1.4 lets the engine assume them and carry no fallback path.

## In the code

| What | File | Symbols |
|---|---|---|
| Hand-written image barrier | `renderer_detail.cppm` | `transitionImage` |
| Stage/access/layout table | `render_graph.cppm` | `usageInfo`, `RgUsageInfo` |
| Hazard test + barrier emit | `render_graph.cppm` | `applyAccess` |
| Final present transition | `renderer.cppm` | `endFrame` |
| AS-build → fragment barrier | `renderer_detail.cppm` | `recordTlasBuild` |

## Related

- [Render graph overview](../../frame-and-render-graph/render-graph-overview/) — emits almost every barrier
- [Usage and barrier derivation](../../frame-and-render-graph/usage-and-barrier-derivation/) — the full hazard + layout table
- [Dynamic rendering](../dynamic-rendering/) — the other 1.3 pillar barriers pair with
