+++
title = 'Bindless textures'
weight = 4
+++

# Bindless textures

Every albedo texture in the scene lives in one global descriptor array. A texture is addressed by an integer slot, not by a per-material descriptor set. The slot rides along in the per-instance data, so two objects that differ *only* by texture still batch into a single instanced draw.

That batching is the payoff. With per-material texture descriptor sets, two cubes with different albedos would each need their own set and couldn't share an instanced draw — texture *was* a batch key. With bindless, texture is just an integer in the instance buffer, so a `DrawBatch` keys only on `(pipeline, mesh)`. Two textures on the same mesh become one instanced `drawIndexed`; `se render-stats` reports batches, so you can watch two differently textured instances stay a single batch.

## One array, set 0

The übershader declares a fixed-size combined-image-sampler array as set 0, binding 0:

```hlsl
[[vk::binding(0, 0)]] Sampler2D albedoTextures[1024];
```

The C++ layout makes that array partially bound and update-after-bind:

- **partiallyBound** means not every one of the 1024 slots needs a valid descriptor. The shader only samples slots that were written, so the empty tail is fine.
- **updateAfterBind** means a slot can be written while the set is bound and in use, between draws — exactly what `uploadTexture` does.

Both features are requested at device selection time (`descriptorBindingPartiallyBound`, `descriptorBindingSampledImageUpdateAfterBind`), so a device that lacks them won't be chosen. The set is bound once and stays bound for every mesh draw. Slot 0 is the default white texture, so a renderable with no albedo samples white.

## Claiming a slot

`uploadTexture` creates the device image, claims the next free slot, writes the descriptor, and stores the slot on the `GpuTexture`. `nextBindlessIndex` is a bump allocator — slots are handed out monotonically and not recycled. The descriptor write pokes one element of the live set, pairing the view with the shared `linearSampler`:

```cpp
void writeBindlessTexture(Renderer& renderer, vk::ImageView view, u32 index)
{
    vk::DescriptorImageInfo info{ renderer.descriptors.linearSampler, view, vk::ImageLayout::eShaderReadOnlyOptimal };
    vk::WriteDescriptorSet write{};
    write.dstSet = renderer.descriptors.bindlessSet;
    write.dstBinding = 0;
    write.dstArrayElement = index;          // the slot
    write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    write.setImageInfo(info);
    renderer.context.device.updateDescriptorSets(write, {});
}
```

One linear, mipped, repeat sampler the renderer owns is shared across every texture, so the array is really a combined-image-sampler array sharing one sampler. A `GpuTexture` owns its image and view but not its sampler.

## The index travels per-instance

The slot ends up in the per-instance storage buffer (set 2). The vertex stage forwards it flat to the fragment stage, which samples with `NonUniformResourceIndex` because the index varies across the warp:

```hlsl
struct Instance
{
    float4x4 model;
    float4x4 normalMatrix;
    float4 baseColor;
    uint4 texture;    // x = bindless albedo index
    // ...
};

// fragment:
float4 tex = albedoTextures[NonUniformResourceIndex(input.textureIndex)].Sample(input.uv0);
```

## In the code

| What | File | Symbols |
|---|---|---|
| Array binding (shader) | `mesh.slang` | `albedoTextures[1024]`, `NonUniformResourceIndex` |
| Layout flags | `renderer_detail.cppm` | `albedoBinding`, `ePartiallyBound`, `eUpdateAfterBind` |
| Device feature gate | `renderer.cppm` | `descriptorBindingPartiallyBound`, `…SampledImageUpdateAfterBind` |
| Slot claim + upload | `renderer_textures.cpp` | `uploadTexture`, `nextBindlessIndex`, `GpuTexture::bindlessIndex` |
| Descriptor write | `renderer_detail.cppm` | `writeBindlessTexture` |
| Index in instance data | `mesh.slang` | `Instance::texture.x` |

## Related

- [Descriptor sets](../descriptor-sets/) — where set 0 sits in the full layout
- [Übershader](../ubershader-and-specialization/) — the shader that samples the array
- [Materials & PSOs](../material-and-pso-selection/) — why texture is not a pipeline key
