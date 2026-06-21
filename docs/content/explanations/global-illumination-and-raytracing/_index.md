+++
title = 'Global illumination & ray tracing'
weight = 12
bookCollapseSection = true
+++

# Global illumination & ray tracing

Dynamic global illumination computes indirect light that tracks moving geometry, and ray tracing
resolves visibility and direct lighting stochastically. [Image-based lighting](../image-based-lighting/)
supplies only a static ambient term, and [screen-space](../screen-space-and-post/) effects
approximate indirect light from what is on screen. This section covers the fully dynamic tier: DDGI
irradiance probes fed by a software voxel trace, an optional hardware ray-tracing path (BLAS/TLAS
plus ray-query shadows), and ReSTIR for many-light direct lighting.

> [!NOTE]
> The RT and ReSTIR paths need a ray-query-capable GPU and run at roughly 1 FPS on the software
> (llvmpipe) dev device, so they are feature-gated. DDGI's software trace runs everywhere.

## Pages

| Page | Covers | Code |
|---|---|---|
| `ddgi-overview` | what DDGI is, the per-frame probe pipeline, why probes over screen-space | `lighting.slang` · `ddgiSampleIrradiance`; `rendering/src/renderer.rs` · `add_ddgi_passes` |
| `voxel-scene-proxy` | per-frame voxel rasterization of draw AABBs, `Image3D`, dynamic volume fitting | `ddgi_voxelize.slang`; `rendering/src/resources.rs` · `Image3D`; `ddgi.rs` · `Ddgi::set_scene` |
| `probe-volume-and-sampling` | the 8×4×8 probe cage, octahedral encoding, trilinear + backface + Chebyshev weights | `lighting.slang` · `ddgiSampleIrradiance`, `ddgiOctEncode`; `rendering/src/ddgi.rs` · `Ddgi` |
| `software-ray-trace` | Fibonacci-sphere rays, voxel march, free multi-bounce via probe reuse | `ddgi_trace.slang` · `computeMain`, `sphericalFibonacci` |
| `irradiance-and-moment-atlases` | temporal irradiance blend, Chebyshev moment atlas, octahedral border wrap | `ddgi_blend_irradiance.slang`, `ddgi_blend_distance.slang`, `ddgi_border.slang` |
| `raytracing-foundation` | per-mesh BLAS, per-frame TLAS + instance buffer, buffer device address | `rendering/src/resources.rs` · `AccelerationStructure`; `rt.rs` · `record_mesh_blas_build`, `record_tlas_build_plan` |
| `raytracing-device-gating` | optional RT extensions, `rt_supported`, the `ash::khr::acceleration_structure` dispatch | `rendering/src/device.rs` · `probe_optional_features`, `Device::accel_dispatch` |
| `ray-query-shadows` | inline `RayQuery` shadow rays in the mesh fragment, replacing shadow maps | `lighting.slang` · `rayQueryShadow`; `rendering/src/renderer.rs` · `set_rt_shadows` |
| `restir-overview` | reservoirs, RIS, the three-pass spatiotemporal resampling pipeline | `restir_initial.slang` · `Reservoir`; `rendering/src/restir.rs` · `Restir` |
| `restir-passes` | initial candidate sampling, temporal+spatial reuse, resolve + shading, M-clamping | `restir_initial.slang`, `restir_reuse.slang`, `restir_resolve.slang` |
