+++
title = 'Descriptor sets'
weight = 3
+++

# Descriptor sets

A descriptor set is a group of GPU resource bindings — textures, buffers, samplers — that a shader reads through a single bound object. Vulkan numbers the sets, and a pipeline declares a layout for each one. A shader and the pipeline that hosts it must agree on that layout exactly.

The convention is to give each set one class of resource and to order the sets by how often the data changes. Binding a low-numbered set rarely and a high-numbered set per draw lets the driver keep more descriptor state resident across draws within a frame.

## The mesh layout

The mesh übershader reads from a fixed set layout. The `vk::binding` attributes in `lighting.slang` (the shared lighting module `mesh.slang` imports) and the set-layout list `Pipelines::new` assembles are two views of the same contract, and they must agree.

Sets 0–5 are always present in a mesh PSO. Sets 6–7 exist only when the device supports ray tracing, because their layouts need the acceleration-structure extension.

| Set | Contents | Bindings | Why it's here |
|---|---|---|---|
| 0 | Bindless albedo array | `0` combined-image-sampler `[1024]` | one global texture array, indexed per-instance |
| 1 | Lighting | `0` directional UBO · `1` punctual list · `2` cluster list · `3` cluster params · `4`–`6` shadow maps | per-frame light state |
| 2 | Instances | `0` per-instance storage buffer · `1` joint matrices · `2` material params | model + normal matrix + base color + texture index |
| 3 | IBL | `0` irradiance cube · `1` prefiltered cube · `2` BRDF LUT | the ambient term |
| 4 | Screen-space | `0` AO · `1` contact shadows · `2` SSGI | per-pixel maps sampled by screen UV |
| 5 | DDGI | `0` irradiance atlas · `1` distance atlas | world-space multi-bounce indirect |
| 6 | RT TLAS | `0` acceleration structure | inline ray-query shadows (RT only) |
| 7 | ReSTIR | `0` resolved radiance | stochastic many-light direct (RT only) |

In the shader these read as `vk::binding(binding, set)`:

```hlsl
[[vk::binding(0, 0)]] public Sampler2D albedoTextures[1024];     // set 0: bindless albedo
[[vk::binding(0, 1)]] ConstantBuffer<LightGlobals> globals;      // set 1: directional + counts
[[vk::binding(1, 1)]] StructuredBuffer<GpuLight> lights;         //        punctual list
[[vk::binding(0, 2)]] StructuredBuffer<Instance> instances;      // set 2: per-instance data
[[vk::binding(2, 2)]] StructuredBuffer<MaterialParams> materialParams;
[[vk::binding(0, 3)]] SamplerCube irradianceMap;                 // set 3: IBL
```

`Pipelines::new` assembles the matching `vk::DescriptorSetLayout` list from the device-global layouts and appends 6 and 7 only when both `rt_mesh_set_layout` and `restir_mesh_set_layout` are present. The set-6 and set-7 bindings still compile into the shader unconditionally; they are only *accessed* under a runtime flag (`globals.pointShadowMeta.z` for RT shadows, `globals.screenFlags.w` for ReSTIR), so the unused bindings cost nothing on a device without RT.

## Numbered by change frequency

The mesh layout follows the change-frequency ordering throughout:

- **Set 0** is bound once and never rebound — the bindless array is a single set for every draw. See [bindless textures](../bindless-textures/).
- **Sets 1–2** are per-frame: light state and the instance buffer are rewritten each frame (double-buffered so a host write never races a frame still reading on the GPU).
- **Sets 3–7** are feature state, stable across frames once their feature is on.

## Flags decoupled from bindings

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
| Binding declarations | `lighting.slang` | `vk::binding(b, s)` across sets 0–7 |
| Runtime feature flags | `lighting.slang` | `LightGlobals::counts`, `screenFlags` |
| PSO set-layout list | `pipelines.rs` | `Pipelines::new` — `set_layouts` |
| Set 0/1/2 layout accessors | `descriptors.rs` | `Descriptors::bindless_set_layout`, `light_set_layout`, `instance_set_layout` |
| RT set accessors (gate 6/7) | `descriptors.rs` | `rt_mesh_set_layout`, `restir_mesh_set_layout` |

## Related

- [Bindless textures](../bindless-textures/) — set 0 in detail
- [Materials & PSOs](../material-and-pso-selection/) — where the layout list is baked into the PSO
- [Cook-Torrance BRDF](../../lighting-and-brdf/cook-torrance-brdf/) — what set 1's light data feeds
