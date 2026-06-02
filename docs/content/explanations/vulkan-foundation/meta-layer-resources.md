+++
title = 'Meta-layer resources'
weight = 7
+++

# Meta-layer resources

A meta-layer resource is a move-only RAII wrapper around one or more raw Vulkan and VMA handles, owning them and freeing them in its destructor. The engine defines a small set — `Pipeline`, `Image`, `Buffer`, `GpuMesh`, `GpuTexture`, plus `Image3D` and `AccelerationStructure`. Each is passed around as `Ref<T>` (a `std::shared_ptr`), so a logical resource is a reference-counted object rather than an opaque integer behind a manager.

The layer sits between the raw Vulkan handles and the rest of the engine. It is thin enough to carry no abstraction tax, yet present enough that nothing outside the renderer touches a raw handle.

## The wrapper shape

Every wrapper follows the same shape, illustrated by `Pipeline`:

```cpp
struct Pipeline
{
    vk::Device device;  // borrowed
    vk::Pipeline pipeline;
    vk::PipelineLayout layout;

    Pipeline(Pipeline&&) noexcept;             // steal, null out other
    auto operator=(Pipeline&&) noexcept -> Pipeline&;  // reset() then steal
    ~Pipeline() { reset(); }
    void reset();  // null-check every handle, destroy, null out
};
```

Three invariants hold for all of them:

- **Move-only.** Copy is deleted; move steals the handles and nulls the source, so the destructor cannot double-free. The raw handle has no shared ownership.
- **`reset()` is the single free path.** Destructor, move-assignment, and explicit `reset()` all funnel through it. It null-checks every handle and the owning device or allocator, so a default or moved-from wrapper destroys to nothing.
- **Borrowed device and allocator.** The wrapper holds a `vk::Device` and/or `VmaAllocator` it does *not* own, used only to free its handles. Teardown order depends on this.

Each wrapper frees its own handles: `Image`, `GpuTexture`, and `Image3D` free a view and an image; `Buffer` and `GpuMesh` free buffers; `AccelerationStructure` frees the AS handle through a borrowed function pointer, then its backing buffer.

## Ref = shared_ptr

The wrappers are move-only, but logical resources are shared between the scene draw list, the PSO cache, ECS components, and capture closures. One alias in `Saffron.Core` bridges the gap:

```cpp
template <typename T>
using Ref = std::shared_ptr<T>;
```

Factories return `Result<Ref<T>>` and `make_shared` the moved wrapper: `uploadMesh` returns `Ref<GpuMesh>`, `uploadTexture` returns `Ref<GpuTexture>`, `requestMeshPipeline` returns `Ref<Pipeline>`. The shared_ptr owns the single move-only wrapper. When the last `Ref` drops, the destructor runs and frees the GPU resource. The meta-layer is therefore a move-only RAII wrapper for correctness, held in a `shared_ptr` for sharing — no base class, no virtual destructor, no handle table. A `Ref<GpuMesh>` is a plain value backed by a plain struct, not a subclass of a `Resource` base.

```mermaid
flowchart LR
    A[uploadMesh / uploadTexture /<br/>requestMeshPipeline] --> B["Result&lt;Ref&lt;T&gt;&gt;<br/>make_shared(move(wrapper))"]
    B --> C[DrawItem / PSO cache /<br/>ECS component]
    C --> D{last Ref<br/>dropped?}
    D -- yes --> E["~T() → reset()<br/>vk::/VMA free"]
```

## The teardown contract

Every wrapper borrows the device and allocator, so nothing may be alive when those are destroyed. The contract has two halves:

1. **`waitGpuIdle` before releasing Refs.** No in-flight command buffer may still reference a resource when its destructor runs. The run loop calls `waitGpuIdle` before the client's `onExit`, and `destroyRenderer` idles again first thing.
2. **Drop every `Ref` before the allocator and device.** `destroyRenderer` explicitly resets the renderer's own Refs and the closure vectors that might capture them — scene draw list, PSO cache, submission vectors, default white texture, every pipeline and target — then destroys descriptor pools, the allocator, and the device, in that order.

A wrong order makes a wrapper's destructor call `vmaDestroyImage` on an already-destroyed allocator. The explicit reset sequence in `destroyRenderer` prevents this.

## In the code

| What | File | Symbols |
|---|---|---|
| The wrappers | `renderer_types.cppm` | `Pipeline`, `Image`, `Buffer`, `GpuMesh`, `GpuTexture`, `Image3D`, `AccelerationStructure` |
| Move-only + `reset()` | `renderer_types.cppm` | each wrapper's move ctor / `reset` |
| The `Ref` alias | `core.cppm` | `Ref` |
| Factories returning `Ref` | `renderer.cppm`, `renderer_detail.cppm` | `uploadMesh`, `uploadTexture`, `requestMeshPipeline`, `makeRtBuffer` |
| Teardown order | `renderer.cppm` | `destroyRenderer`, `waitGpuIdle` |

> [!NOTE]
> A `GpuTexture` destructor does *not* reclaim its bindless slot. No live material references a destroyed texture's slot, so its stale descriptor is never sampled. Slots leak across the session, but the array holds 1024, and a real reclaim would need to defer past in-flight frames — not worth it for v1.

## Related

- [No-exceptions Vulkan-Hpp](../vulkan-hpp-no-exceptions/) — why smart handles are off and the engine owns handles itself
- [VMA allocator](../vma-allocator/) — the borrowed allocator these wrappers free through
- [GPU mesh upload](../../geometry-and-assets/gpu-mesh-upload/) — `uploadMesh` building a `GpuMesh` and returning a `Ref`
- [Material and PSO selection](../../materials-and-pipelines/material-and-pso-selection/) — the `Ref<Pipeline>` cache
