+++
title = 'VMA allocator'
weight = 3
+++

# VMA allocator

The Vulkan Memory Allocator (VMA) manages GPU memory on behalf of an application: it chooses a memory
type, allocates it, and binds it to a buffer or image in one call. Every device-local buffer and image in
the engine — meshes, textures, offscreen targets, light SSBOs, acceleration structures — goes through a
single VMA allocator that lives for the device's whole lifetime.

Vulkan leaves memory management to the caller. Done by hand, that means querying memory types, respecting
alignment, and sub-allocating to stay under the per-device allocation-count limit, all tracked manually.
VMA applies the heuristics and returns a clean `(buffer, allocation)` or `(image, allocation)` pair to
free. The engine binds to VMA through the `vk-mem` crate, the Rust binding over the same VMA C++ library —
so the allocation calls are still `unsafe` FFI at the ash seam, wrapped behind safe resource constructors.

## Creating the allocator

`create_allocator` builds the allocator over the ash instance, device, and physical device right after
the logical device, and it is stored in the shared `DeviceResources` bundle. It sets the target API
version and the buffer-device-address flag, which is always on because the required feature set enables
`buffer_device_address` and VMA must size BDA-flagged allocations correctly:

```rust
create_info.flags = vk_mem::AllocatorCreateFlags::BUFFER_DEVICE_ADDRESS;
let allocator = unsafe { vk_mem::Allocator::new(create_info) };
```

The allocator outlives every resource. `DeviceResources::drop` takes it out of its `Option` and drops it
before the device, so `vmaDestroyAllocator` runs before `vkDestroyDevice` — the order VMA requires, made
structural rather than field-order-hopeful.

## Allocating images and buffers

The resource wrappers fill a create-info plus a `vk_mem::AllocationCreateInfo`, then call the VMA create,
which allocates and binds in one step. Two recurring choices:

- **`MemoryUsage::AutoPreferDevice`** lets VMA pick a device-local memory type from how the resource is
  used, rather than a hand-picked property mask. Render targets, meshes, and AS storage land in
  device-local memory, which is all the wrapper needs to state.
- **`MAPPED` + `HOST_ACCESS_*`** for host-visible buffers (per-frame UBOs/SSBOs, ray-tracing scratch and
  instance buffers, the read-back staging buffer). VMA keeps these persistently mapped; the wrapper reads
  `get_allocation_info().mapped_data` once and writes through that pointer each frame with no
  `vkMapMemory` round trip. `Buffer::mapped_bytes` hands out the mapped span as a `&mut [u8]`.

Freeing is symmetric and happens in each wrapper's `Drop`: `destroy_buffer(buffer, &mut allocation)` for
buffers, `destroy_image(image, &mut allocation)` for images. Because the wrapper holds a clone of the
`Arc<DeviceResources>`, the allocator is guaranteed alive for that destroy call.

## The shared bundle

VMA is *borrowed* by every resource wrapper, never owned by one. The ash device and the allocator live
together behind a single `Arc<DeviceResources>`; each `Buffer` / `Image` / `Image3D` / `GpuMesh` /
`GpuTexture` / `AccelerationStructure` clones that `Arc` at construction. The allocator and device are
destroyed only when the last clone drops — normally the `Device` itself, after the run loop's `wait_idle`
and the owner's resource teardown. This makes "the device must outlive every resource" structural: a
resource that survives the device keeps the bundle alive, which the validation layer would flag.

## In the code

| What | File | Symbols |
|---|---|---|
| Allocator creation | `device.rs` | `create_allocator`, `AllocatorCreateFlags::BUFFER_DEVICE_ADDRESS` |
| The shared bundle | `resources.rs` | `DeviceResources`, `DeviceResources::drop` |
| Buffer allocation | `resources.rs` | `Buffer::new`, `Buffer::mapped_bytes` |
| Image allocation | `resources.rs` | `Image::new`, `Image3D::new`, `ImageDesc` |
| AS-storage allocation | `resources.rs` | `AccelerationStructure::create` |
| Freeing via Drop | `resources.rs` | `Buffer`/`Image`/`GpuMesh` `Drop` impls |

> [!NOTE]
> The leak probe `vmaCalculateStatistics` works on every device including llvmpipe, so the resource
> tests assert the live allocation count returns to its baseline after each wrapper drops — a reliable
> no-leak gate in the toolbox where heap-budget telemetry (`VK_EXT_memory_budget`) may be absent.

## Related

- [Meta-layer resources](../meta-layer-resources/) — the wrappers that hold the bundle and free through VMA
- [Device & swapchain](../device-and-swapchain/) — where the allocator is created
- [Bindless textures](../../materials-and-pipelines/bindless-textures/) — VMA-allocated images behind the texture array
