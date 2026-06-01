+++
title = 'Descriptor sets'
weight = 3
+++

# Descriptor sets

The mesh übershader reads from a fixed set layout. Each Vulkan descriptor set holds one class of resource, numbered by how often it changes. The `vk::binding` attributes in `mesh.slang` and the layout list in `newMeshPipeline` are two views of the same contract, and they must agree.

## The layout

Sets 0–5 are always present in a mesh PSO. Sets 6–7 exist only when the device supports ray tracing, because their layouts need the acceleration-structure extension.

| Set | Contents | Bindings | Why it's here |
|---|---|---|---|
| 0 | Bindless albedo array | `0` combined-image-sampler `[1024]` | one global texture array, indexed per-instance |
| 1 | Lighting | `0` directional UBO · `1` punctual list · `2` cluster lists · `3` cluster params · `4`–`6` shadow maps | per-frame light state |
| 2 | Instances | `0` per-instance storage buffer | model + normal matrix + base color + texture index |
| 3 | IBL | `0` irradiance cube · `1` prefiltered cube · `2` BRDF LUT | the ambient term |
| 4 | Screen-space | `0` AO · `1` contact shadows · `2` SSGI | per-pixel maps sampled by screen UV |
| 5 | DDGI | `0` irradiance atlas · `1` distance atlas | world-space multi-bounce indirect |
| 6 | RT TLAS | `0` acceleration structure | inline ray-query shadows (RT only) |
| 7 | ReSTIR | `0` resolved radiance | stochastic many-light direct (RT only) |

In the shader these read as `vk::binding(binding, set)`:

```hlsl
[[vk::binding(0, 0)]] Sampler2D albedoTextures[1024];   // set 0: bindless albedo
[[vk::binding(0, 1)]] ConstantBuffer<LightGlobals> globals;  // set 1: directional + counts
[[vk::binding(1, 1)]] StructuredBuffer<GpuLight> lights;     //        punctual list
[[vk::binding(0, 2)]] StructuredBuffer<Instance> instances;  // set 2: per-instance data
[[vk::binding(0, 3)]] SamplerCube irradianceMap;             // set 3: IBL
```

`newMeshPipeline` assembles the matching `vk::DescriptorSetLayout` list and appends 6 and 7 only when RT is live. The set-6 and set-7 bindings still compile into the shader unconditionally; they are only *accessed* under a runtime flag (`globals.pointShadowMeta.z` for RT shadows, `globals.screenFlags.w` for ReSTIR), so the unused bindings cost nothing on a device without RT.

## Numbered by change frequency

The ordering is deliberate, following the conventional Vulkan discipline: bind the rarely changing sets low and the per-draw data high, so the driver keeps more descriptor state resident across draws within a frame.

- **Set 0** is bound once and never rebound — the bindless array is a single set for every draw. See [bindless textures](../bindless-textures/).
- **Sets 1–2** are per-frame: light state and the instance buffer are rewritten each frame (double-buffered so a host write never races a frame still reading on the GPU).
- **Sets 3–7** are feature state, stable across frames once their feature is on.

## How a flag decouples from a binding

Most of these features are optional and gated by a flag in the light UBO (`globals.counts`, `globals.screenFlags`) rather than by a different pipeline. The übershader checks the flag, then samples the matching set:

```hlsl
if (globals.counts.z != 0)        // IBL enabled
{
    float3 irradiance = irradianceMap.SampleLevel(n, 0.0).rgb;   // set 3
    // ... split-sum specular ...
}
if (globals.counts.w != 0)        // AO enabled
{
    ambient *= aoMap.SampleLevel(screenUv, 0.0).r;               // set 4
}
```

A feature toggling on or off is a UBO write, not a pipeline switch. The sets are always bound; the flag decides whether the shader reads them.

## In the code

| What | File | Symbols |
|---|---|---|
| Binding declarations | `mesh.slang` | `vk::binding(b, s)` across sets 0–7 |
| Runtime feature flags | `mesh.slang` | `LightGlobals::counts`, `screenFlags` |
| PSO layout list | `renderer_pipelines.cpp` | `newMeshPipeline` — `setLayouts` |
| Set 0/1/2 layout objects | `renderer_types.cppm` | `Descriptors::bindlessSetLayout`, `lightSetLayout`, `instanceSetLayout` |
| Set 1 binding build | `renderer_detail.cppm` | `lightBindings` |

## Related

- [Bindless textures](../bindless-textures/) — set 0 in detail
- [Materials & PSOs](../material-and-pso-selection/) — where the layout list is baked into the PSO
- [Cook-Torrance BRDF](../../lighting-and-brdf/cook-torrance-brdf/) — what set 1's light data feeds
