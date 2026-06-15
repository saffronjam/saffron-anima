+++
title = 'Bindless textures'
weight = 4
+++

# Bindless textures

Bindless texturing addresses every texture through one global descriptor array, indexed by an
integer slot rather than a per-material descriptor set. A shader reads the slot from per-draw data
and samples the array at that index. Binding the array once covers every texture the scene uses.

The integer slot is what makes this useful. When texture is data rather than binding state, two
objects that differ only by texture share the same pipeline and the same draw, so texture stops
being a batch key.

## How it works

Every albedo texture in the scene lives in one fixed-size descriptor array bound at set 0. A
texture is identified by its position in that array. The slot travels in the per-instance data, so
the shader can look up the right texture for each instance from a single bound array.

Because the slot is just an integer, a `DrawBatch` keys only on `(pipeline, mesh)`. Two textures on
the same mesh become one instanced `drawIndexed`. `sa render-stats` reports batches, so two
differently textured instances are visible as a single batch.

## One array, set 0

The übershader declares a fixed-size combined-image-sampler array as set 0, binding 0:

```hlsl
[[vk::binding(0, 0)]] Sampler2D albedoTextures[1024];
```

The C++ layout makes that array partially bound and update-after-bind:

- **partiallyBound** means not every one of the 1024 slots needs a valid descriptor. The shader
  only samples slots that were written, so the empty tail is fine.
- **updateAfterBind** means a slot can be written while the set is bound and in use, between draws —
  exactly what `uploadTexture` does.

Both features are requested at device selection time (`descriptorBindingPartiallyBound`,
`descriptorBindingSampledImageUpdateAfterBind`), so a device that lacks them is not chosen. The set
is bound once and stays bound for every mesh draw. Slot 0 is the default white texture, so a
renderable with no albedo samples white.

## Claiming a slot

`uploadTexture` creates the device image, claims the next free slot, writes the descriptor, and
stores the slot on the `GpuTexture`. `nextBindlessIndex` is a bump allocator: slots are handed out
monotonically and not recycled. The descriptor write pokes one element of the live set, pairing the
view with the shared `linearSampler`:

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

The renderer owns one linear, mipped, repeat sampler, shared across every texture in the array. A
`GpuTexture` owns its image and view but not its sampler.

## The index travels per-instance

The slot ends up in the per-instance storage buffer (set 2). The vertex stage forwards it flat to
the fragment stage, which samples with `NonUniformResourceIndex` because the index varies across
the warp:

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

## Slot lifetime and mipmaps

The 1024-slot array is finite, so slots are **reclaimed**. A shared free-list lives on `Descriptors` and is
held (as a `Ref`) by every `GpuTexture`; when a texture is destroyed its slot returns to the list, and the
next upload reuses a freed slot before growing `nextBindlessIndex`. This keeps a hot-reloaded or churny
scene bounded instead of marching the high-water mark to the limit. Reclaim is frame-safe because the draw
path holds live texture `Ref`s for the frame — textures die at cache-clear/teardown, never mid-frame — and
the free-list outlives both the descriptors and the textures. `sa render-stats` reports `bindlessTextures`
(high-water) and `bindlessFree` (reclaimed).

Uploads generate a full **mip chain** (`vkCmdBlitImage` down the levels, linear filter) and the bindless
sampler is trilinear, so minified 4K material textures don't alias. A texture whose `Uuid` is missing from
the catalog warns once and resolves to the default white slot — never a null descriptor or black surface.

## In the code

| What | File | Symbols |
|---|---|---|
| Array binding (shader) | `mesh.slang` | `albedoTextures[1024]`, `NonUniformResourceIndex` |
| Layout flags | `renderer_detail.cppm` | `albedoBinding`, `ePartiallyBound`, `eUpdateAfterBind` |
| Device feature gate | `renderer.cppm` | `descriptorBindingPartiallyBound`, `…SampledImageUpdateAfterBind` |
| Slot claim + reclaim | `renderer_textures.cpp` | `claimBindlessSlot`, `bindlessFreeList`; `renderer_types.cppm` · `GpuTexture::reset` |
| Mip generation | `renderer_textures.cpp` | `mipCount`, `recordMipChain` |
| Descriptor write | `renderer_detail.cppm` | `writeBindlessTexture` |
| Index in instance data | `mesh.slang` | `Instance::texture.x` |

## Related

- [Descriptor sets](../descriptor-sets/) — where set 0 sits in the full layout
- [Übershader](../ubershader-and-specialization/) — the shader that samples the array
- [Materials & PSOs](../material-and-pso-selection/) — why texture is not a pipeline key
