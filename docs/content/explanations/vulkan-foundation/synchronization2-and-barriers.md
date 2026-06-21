+++
title = 'Barriers'
weight = 4
+++

# Barriers

A barrier is a Vulkan command that orders GPU work and transitions image layouts. The application owns
this responsibility; the driver synchronizes almost nothing on its own.

The engine expresses every barrier with `synchronization2` — the `vk::…Barrier2` family submitted through
`cmd_pipeline_barrier2`. The legacy single-barrier API is used nowhere. The feature is required at device
selection, so every barrier in the engine is a 2-style barrier.

## How a barrier works

A `synchronization2` barrier carries a source and destination scope, each a `{ stage, access }` pair, and
for images an old and new layout. The stage says *when* in the pipeline the work happens; the access says
*what kind* of memory access it is. A barrier finishes the source scope's work and makes its writes
visible before the destination scope reads or writes.

## Where barriers come from

Almost all barriers come from the [render graph](../../frame-and-render-graph/render-graph-overview/),
which derives them from declared resource usage. The few that don't fit the graph are hand-written: the
final swapchain transition in the present path, the `transition_image` helper used by texture-upload
staging and mip generation, IBL baking, point-shadow cube faces (a 6-layer cube exceeds the graph's
single-layer barriers), and the acceleration-structure build barriers recorded around a TLAS build. Each
builds a `vk::ImageMemoryBarrier2` (or `vk::BufferMemoryBarrier2`), wraps it in a `vk::DependencyInfo`,
and submits it with one `cmd_pipeline_barrier2`.

## Two barrier shapes

`synchronization2` has the relevant barrier types and the engine uses both:

- **`vk::ImageMemoryBarrier2`** — for images. Carries a layout transition *and* the memory dependency. An
  image can need a barrier purely to change layout, even with no data hazard.
- **`vk::MemoryBarrier2`** (and `vk::BufferMemoryBarrier2` for buffer-scoped host visibility) — for
  buffers and global memory. No layout, just the source→destination scope. A buffer only ever needs a
  barrier on a real data hazard.

Both batch into one `vk::DependencyInfo` and submit with a single `cmd_pipeline_barrier2`. A pass touching
several resources pays for one barrier call.

## Stage and access masks

The vocabulary of `{ stage, access, layout }` triples lives in the render graph's `usage_info` — the
single function mapping a declared `RgUsage` to its masks. A few representative rows:

| Usage | Stage | Access | Layout |
|---|---|---|---|
| `ColorWrite` | `COLOR_ATTACHMENT_OUTPUT` | `COLOR_ATTACHMENT_WRITE` | `COLOR_ATTACHMENT_OPTIMAL` |
| `DepthWrite` | `EARLY_FRAGMENT_TESTS \| LATE_FRAGMENT_TESTS` | `DEPTH_STENCIL_ATTACHMENT_WRITE` | `DEPTH_ATTACHMENT_OPTIMAL` |
| `SampledRead` | `FRAGMENT_SHADER` | `SHADER_SAMPLED_READ` | `SHADER_READ_ONLY_OPTIMAL` |
| `StorageImageRwCompute` | `COMPUTE_SHADER` | `SHADER_STORAGE_READ \| SHADER_STORAGE_WRITE` | `GENERAL` |

Depth write spans both early and late fragment tests because depth is read and written across both. An
image read and written in place by a compute shader lives in `GENERAL`, the layout that allows read and
write — the tonemap and post passes transition the offscreen to `GENERAL` for that reason.

## When a barrier fires

`apply_access` decides, per resource, whether a barrier is needed. The data-hazard test is one line:

```rust
let hazard = (target.is_write && r.touched) || (!target.is_write && r.last_was_write);
```

A write after any prior touch is a hazard (WAW or WAR); a read after a write is a hazard (RAW);
read-after-read is not. An image has a second trigger: a layout mismatch emits a barrier even without a
data hazard, because the layout must change before the GPU can use the image the new way. Buffers have no
layout, so they barrier on the hazard alone. The full derivation is in
[usage and barrier derivation](../../frame-and-render-graph/usage-and-barrier-derivation/).

## Why synchronization2

The original barrier API split stage and access into separate top-level fields and could not express
per-barrier stage masks cleanly. `synchronization2` folds stage and access into one scope per side, lets
image and buffer barriers share a `DependencyInfo`, and pairs with
[dynamic rendering](../dynamic-rendering/); both are Vulkan 1.3 core, required at device selection, so the
engine assumes them and carries no fallback path.

## In the code

| What | File | Symbols |
|---|---|---|
| Hand-written image barrier | `upload.rs` | `transition_image` |
| Stage/access/layout table | `render_graph.rs` | `usage_info`, `RgUsageInfo` |
| Hazard test + barrier emit | `render_graph.rs` | `apply_access`, `DerivedBarriers` |
| Final present transition | `renderer.rs` | `record_clear` |
| AS-build barriers | `rt.rs` | `record_tlas_build_plan` |

## Related

- [Render graph overview](../../frame-and-render-graph/render-graph-overview/) — emits almost every barrier
- [Usage and barrier derivation](../../frame-and-render-graph/usage-and-barrier-derivation/) — the full hazard + layout table
- [Dynamic rendering](../dynamic-rendering/) — the other 1.3 pillar barriers pair with
