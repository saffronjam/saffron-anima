+++
title = 'VMA allocator'
weight = 3
+++

# VMA allocator

The Vulkan Memory Allocator (VMA) is a library that manages GPU memory on behalf of an application: it chooses a memory type, allocates it, and binds it to a buffer or image in one call. Every device-local buffer and image in the engine — meshes, textures, offscreen targets, light SSBOs, acceleration structures — goes through a single VMA allocator that lives on the renderer for its whole lifetime.

Vulkan leaves memory management to the caller. Done by hand, that means querying memory types, respecting alignment, and sub-allocating to stay under the per-device allocation-count limit, all tracked manually. VMA applies the heuristics and returns a clean `(image, alloc)` pair to free.

## The single impl TU

VMA is a single-header C library that needs exactly one translation unit to instantiate its implementation. That TU is the whole of `cmake/vma_impl.cpp`.

```cpp
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
```

Everywhere else the header is included without that define, leaving only declarations. Compiling the implementation once, in its own file, keeps it out of the module units (which could not define it twice anyway) and clear of `import std`.

## Creating the allocator

The allocator is created in `newRenderer`, right after the logical device. It receives the instance, physical device, device, and API version, and is stored as `context.allocator`. The buffer-device-address flag is set only when [ray tracing](../../global-illumination-and-raytracing/raytracing-foundation/) is supported, because acceleration-structure builds feed vertex, index, and instance buffer addresses to the GPU:

```cpp
if (renderer.context.rtSupported)
    allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
```

The allocator outlives every resource and is the last GPU object torn down in `destroyRenderer`, after every buffer and image is freed.

## Allocating images and buffers

The pattern is the same for color targets, depth targets, cube images, and 3D images: fill the create info and a `VmaAllocationCreateInfo`, then call `vmaCreateImage`, which allocates and binds in one call. Two choices recur for render targets:

- **`VMA_MEMORY_USAGE_AUTO`** lets VMA pick the memory type from how the resource is used, rather than a hand-picked property mask. Render targets land in device-local memory, which is all the engine needs to state.
- **`VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT`** gives each target its own allocation rather than a sub-range of a shared block. Targets are recreated on every viewport resize, so a dedicated allocation makes freeing one a clean, isolated operation.

Host-visible buffers (per-frame UBOs and SSBOs, ray-tracing scratch and instance buffers) use `HOST_ACCESS_SEQUENTIAL_WRITE` and `MAPPED` instead. VMA keeps them persistently mapped, so `pMappedData` is written each frame with no `vkMapMemory` round trip.

Freeing is symmetric: `vmaDestroyImage` for images, `vmaDestroyBuffer` for buffers, each taking the handle and its allocation. The move-only [meta-layer wrappers](../meta-layer-resources/) call exactly these from their destructors. The `nullptr` allocator guard in each `reset()` makes a moved-from or default wrapper a no-op to destroy.

## In the code

| What | File | Symbols |
|---|---|---|
| Single impl TU | `vma_impl.cpp` | `VMA_IMPLEMENTATION` |
| Allocator creation | `renderer.cppm` | `vmaCreateAllocator`, `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` |
| Image allocation | `renderer_detail.cppm` | `newColorImage`, `newDepthImage`, `newCubeImage`, `newImage3D` |
| Host-visible buffers | `renderer_detail.cppm` | `makeRtBuffer` (`HOST_ACCESS_SEQUENTIAL_WRITE`, `MAPPED`) |
| Freeing via RAII | `renderer_types.cppm` | `Image::reset`, `Buffer::reset` |

> [!NOTE]
> The allocator is *borrowed* by every resource wrapper, never owned by one. Every `Image`/`Buffer`/`GpuMesh`/`GpuTexture` `Ref` must drop before `vmaDestroyAllocator` runs. `destroyRenderer` enforces this by `waitGpuIdle`, resetting all of them, then destroying the allocator last. See [meta-layer resources](../meta-layer-resources/).

## Related

- [Meta-layer resources](../meta-layer-resources/) — the wrappers that hold a `VmaAllocator` and free through it
- [Device & swapchain](../device-and-swapchain/) — where the allocator is created
- [Bindless textures](../../materials-and-pipelines/bindless-textures/) — VMA-allocated images behind the texture array
