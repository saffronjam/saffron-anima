+++
title = 'RT device gating'
weight = 7
+++

# RT device gating

Device gating is the practice of detecting an optional GPU capability at startup and enabling its
code paths only on hardware that supports it. Ray tracing is such a capability: the KHR
acceleration-structure and ray-query extensions are not present on every device.

Saffron detects RT support during device bring-up, enables the extensions and features only when
they are present, and resolves the acceleration-structure entry points manually. A single flag,
`rtSupported`, records the result and gates every downstream RT path.

## Detection at device selection

vk-bootstrap's `enable_extension_if_present` enables an extension when the device has it and does
nothing otherwise, so requesting the RT extensions never fails device creation. Presence alone is
insufficient; the feature bits must also be set. The check requires both:

```cpp
const bool hasAsExt = physical.enable_extension_if_present(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
const bool hasRqExt = physical.enable_extension_if_present(VK_KHR_RAY_QUERY_EXTENSION_NAME);
bool rtSupported = hasAsExt && hasRqExt
    && asFeat.accelerationStructure == VK_TRUE && rqFeat.rayQuery == VK_TRUE;
```

The RT feature structs are chained into device creation *only* when `rtSupported`, so a device
without them is never asked to enable a feature it lacks. When RT is on, the VMA allocator also gets
`BUFFER_DEVICE_ADDRESS`, because AS builds feed vertex, index, and instance buffers by device
address.

## Resolving the entry points

The acceleration-structure and ray-query functions are not core, so the loader does not export them
statically; the engine otherwise relies on Vulkan-Hpp's static dispatch. When RT is supported, the
five functions the engine calls are resolved through `vkGetDeviceProcAddr` into a small dispatch
table (`getBuildSizes`, `createAccel`, `destroyAccel`, `cmdBuild`, `getAccelAddress`).

If any resolve returns null, `rtSupported` is forced back to false. A device that advertised the
extensions but cannot supply the functions is treated as non-RT. The `RtDispatch` table is the only
place these C entry points are held, and the rest of the renderer calls through it.

## The gate everywhere downstream

`rtSupported` is a hard precondition for everything in the RT and ReSTIR paths:

- `rtSupported(renderer)` exposes it; `setRtShadows`/`setRestir` are no-ops when it is false.
- `buildTlas` returns immediately if `!rtSupported`.
- `buildBlas` is called from mesh upload only when RT is supported, so `GpuMesh::blas` stays null
  otherwise.

The feature toggles therefore wire into the UI and the `se` control plane unconditionally: on a
non-RT device they are inert rather than fatal.

## What stays compiled regardless

The mesh PSO's set 6 (the TLAS binding) and `rayQueryShadow` are *bound and run* only under the
runtime flag, yet the shader still declares `RaytracingAccelerationStructure rtScene`
unconditionally. The compiled SPIR-V therefore carries the `RayQueryKHR` capability even on a device
that will never trace; the binding is present, just never accessed. See
[ray-query shadows](../ray-query-shadows/) for the consequence.

## In the code

| What | File | Symbols |
|---|---|---|
| Extension + feature detection | `renderer.cppm` | device bring-up (`hasAsExt`, `rtSupported`) |
| Entry-point resolution | `renderer.cppm` | the `vkGetDeviceProcAddr` block |
| The dispatch table | `renderer_types.cppm` | `RtDispatch`, `VulkanContext::rtSupported` |
| The public gate | `renderer.cppm` | `rtSupported`, `setRtShadows`, `setRestir` |
| BDA on the allocator | `renderer.cppm` | the `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` branch |

> [!NOTE]
> On the software (llvmpipe) dev GPU the extensions can be present and the RT path activates, but
> it runs at roughly 1 FPS — correctness-validated, awaiting real ray-tracing hardware. The DDGI
> software trace is the everywhere-fast alternative.

## Related

- [Acceleration structures](../raytracing-foundation/) — what the resolved entry points build
- [Ray-query shadows](../ray-query-shadows/) — the runtime-gated consumer
- [Vulkan foundation](../../vulkan-foundation/) — static dispatch + the no-exceptions result convention
