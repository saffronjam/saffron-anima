+++
title = 'RT device gating'
weight = 7
+++

# RT device gating

Device gating is the practice of detecting an optional GPU capability at startup and enabling its
code paths only on hardware that supports it. Ray tracing is such a capability: the KHR
acceleration-structure and ray-query extensions are not present on every device.

Saffron detects RT support during device bring-up, enables the extensions and features only when
they are present, and resolves the acceleration-structure command dispatch only on a supporting
device. A single flag, `Capabilities::rt_supported`, records the result and gates every downstream
RT path.

## Detection at device selection

`probe_optional_features` enumerates the chosen device's extensions and checks for both the KHR
acceleration-structure and ray-query extensions. Presence alone is insufficient; the feature bits
must also be set, so it chains `vk::PhysicalDeviceAccelerationStructureFeaturesKHR` and
`vk::PhysicalDeviceRayQueryFeaturesKHR` into a `get_physical_device_features2` query and requires
both:

```rust
let has_as = has_ext(ash::khr::acceleration_structure::NAME);
let has_rq = has_ext(ash::khr::ray_query::NAME);
let rt_supported = has_as && has_rq
    && as_feat.acceleration_structure != 0 && rq_feat.ray_query != 0;
```

`create_logical_device` re-probes the same two extensions and pushes them (plus
`VK_KHR_deferred_host_operations`) onto the device only when both are present, chaining the RT
feature structs into `vk::DeviceCreateInfo` only then — so a device without them is never asked to
enable a feature it lacks. The allocator always carries `vk_mem::AllocatorCreateFlags::BUFFER_DEVICE_ADDRESS`
and `buffer_device_address` is in the required feature set, because the renderer uses BDA broadly;
AS builds, which feed vertex, index, and instance buffers by device address, simply ride on that.

## Resolving the command dispatch

The acceleration-structure and ray-query commands are extension entry points, not core Vulkan. When
RT is supported, the renderer constructs an `ash::khr::acceleration_structure::Device` dispatch
(`accel::Device::new`) once and holds it on the `Device`; it exposes it through
`Device::accel_dispatch`, which returns `None` on a non-RT device. The `AccelerationStructure`
wrapper clones this dispatch so it can destroy itself independently, and the BLAS/TLAS build path
calls every AS command through it.

## The gate everywhere downstream

`rt_supported` is a hard precondition for everything in the RT and ReSTIR paths:

- `Device::rt_supported` / `Renderer::rt_supported` expose it; `set_rt_shadows` / `set_restir` are
  no-ops when it is false.
- the `tlas-build` pass is skipped unless `Rt::build_pending` (which requires `Rt::supported`).
- the per-mesh BLAS is built from mesh upload only when RT is supported, so `GpuMesh::blas` stays
  `None` otherwise.

The feature toggles therefore wire into the editor UI and the `sa` control plane unconditionally: on
a non-RT device they are inert rather than fatal.

## What stays compiled regardless

The mesh PSO's set 6 (the TLAS binding) and `rayQueryShadow` are *bound and run* only under the
runtime flag, yet the shader still declares `RaytracingAccelerationStructure rtScene`
unconditionally. The compiled SPIR-V therefore carries the `RayQueryKHR` capability even on a device
that will never trace; the binding is present, just never accessed. See
[ray-query shadows](../ray-query-shadows/) for the consequence.

## In the code

| What | File | Symbols |
|---|---|---|
| Extension + feature detection | `rendering/src/device.rs` | `probe_optional_features`, `Capabilities::rt_supported` |
| Device extension / feature enable | `rendering/src/device.rs` | `create_logical_device` (the `enable_rt` branch) |
| The command dispatch | `rendering/src/device.rs` | `Device::accel_dispatch` (`ash::khr::acceleration_structure::Device`) |
| The public gate | `rendering/src/device.rs`, `renderer.rs` | `Device::rt_supported`; `Renderer::rt_supported`, `set_rt_shadows`, `set_restir` |
| BDA on the allocator | `rendering/src/device.rs` | `create_allocator` (the `BUFFER_DEVICE_ADDRESS` flag) |

> [!NOTE]
> On the software (llvmpipe) dev GPU the extensions can be present and the RT path activates, but
> it runs at roughly 1 FPS — correctness-validated, awaiting real ray-tracing hardware. The DDGI
> software trace is the everywhere-fast alternative.

## Related

- [Acceleration structures](../raytracing-foundation/) — what the resolved entry points build
- [Ray-query shadows](../ray-query-shadows/) — the runtime-gated consumer
- [Vulkan foundation](../../vulkan-foundation/) — the ash dispatch + the `Result` error convention
