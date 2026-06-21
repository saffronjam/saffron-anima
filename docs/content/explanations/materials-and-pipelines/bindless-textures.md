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

Because the slot is just an integer, a draw bucket keys only on `(pipeline, mesh)`. Two textures on
the same mesh become one instanced `cmd_draw_indexed`. `sa render-stats` reports batches, so two
differently textured instances are visible as a single batch.

## One array, set 0

The shared lighting module declares a fixed-size combined-image-sampler array as set 0, binding 0:

```hlsl
[[vk::binding(0, 0)]] public Sampler2D albedoTextures[1024];
```

The Rust layout (`create_bindless_layout`) makes that array partially bound and update-after-bind:

- **PARTIALLY_BOUND** means not every one of the `MAX_BINDLESS_TEXTURES` (1024) slots needs a valid
  descriptor. The shader only samples slots that were written, so the empty tail is fine.
- **UPDATE_AFTER_BIND** means a slot can be written while the set is bound and in use, between draws —
  exactly what the upload path does. The set is allocated from an `UPDATE_AFTER_BIND_POOL`.

Both features are requested at device selection time (`descriptor_binding_partially_bound`,
`descriptor_binding_sampled_image_update_after_bind` in the Vulkan 1.2 features), so a device that
lacks them is not chosen. The set is bound once and stays bound for every mesh draw. Slot 0 is the
default white texture (`DEFAULT_WHITE_SLOT`, claimed first at init and never reclaimed), so a
renderable with no albedo samples white.

## Claiming a slot

`upload_texture` creates the device image, claims the next free slot, writes the descriptor, and
stores the slot on the `GpuTexture`. The allocator (`Descriptors::claim_slot`) pops the reclaim
free-list first and only grows its `next_index` high-water mark when the list is empty. The
descriptor write pokes one element of the live set, pairing the view with the shared `linear_sampler`:

```rust
pub fn write_texture(&self, view: vk::ImageView, index: u32) {
    let image_info = [vk::DescriptorImageInfo {
        sampler: self.linear_sampler,
        image_view: view,
        image_layout: vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
    }];
    let write = vk::WriteDescriptorSet::default()
        .dst_set(self.bindless_set)
        .dst_binding(0)
        .dst_array_element(index)        // the slot
        .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
        .image_info(&image_info);
    let _guard = self.slots.lock().expect("bindless slot allocator lock");
    unsafe { self.resources.device().update_descriptor_sets(&[write], &[]) };
}
```

The descriptors own one linear, mipped, repeat `linear_sampler`, shared across every texture in the
array. A `GpuTexture` owns its image and view but not its sampler. The write takes the bindless
mutex, because a background thumbnail worker can write the same set concurrently.

## The index travels per-instance

The slot ends up in the per-instance storage buffer (set 2, binding 0): `InstanceData.texture.x`.
The vertex stage forwards it flat to the fragment stage, which samples with
`NonUniformResourceIndex` because the index varies across the warp:

```hlsl
struct Instance
{
    float4x4 model;
    float4x4 normalMatrix;
    float4 baseColor;
    uint4 texture;    // x = bindless albedo index
    // ...
};

// fragment (via the material params it dereferences):
float4 tex = albedoTextures[NonUniformResourceIndex(mat.tex0.x)].Sample(uv);
```

## Slot lifetime and mipmaps

The 1024-slot array is finite, so slots are **reclaimed**. A shared free-list
(`BindlessFreeList = Arc<Mutex<Vec<u32>>>`) is cloned by every `GpuTexture`; when a texture is
dropped its `Drop` pushes its `bindless_index` back to the list (LIFO), and the next claim reuses a
freed slot before growing `next_index`. This keeps a hot-reloaded or churny scene bounded instead of
marching the high-water mark to the limit. Reclaim is frame-safe because the draw path holds live
texture `Arc`s for the frame — textures die at cache-clear/teardown, never mid-frame — and the
free-list outlives both the descriptors and the textures. `sa render-stats` reports
`bindless_textures` (high-water) and `bindless_free` (reclaimed).

Uploads generate a full **mip chain** (`cmd_blit_image` down the levels, linear filter — `mip_count`
gives the level count) and the bindless sampler is trilinear, so minified 4K material textures don't
alias. A texture whose `Uuid` is missing from the catalog resolves to the default white slot — never
a null descriptor or black surface.

## In the code

| What | File | Symbols |
|---|---|---|
| Array binding (shader) | `lighting.slang` | `albedoTextures[1024]`, `NonUniformResourceIndex` |
| Layout flags + capacity | `descriptors.rs` | `create_bindless_layout`, `PARTIALLY_BOUND`, `UPDATE_AFTER_BIND`, `MAX_BINDLESS_TEXTURES` |
| Device feature gate | `device.rs` | `descriptor_binding_partially_bound`, `descriptor_binding_sampled_image_update_after_bind` |
| Slot claim + descriptor write | `descriptors.rs` | `claim_slot`, `write_texture`, `DEFAULT_WHITE_SLOT` |
| Slot reclaim | `resources.rs` | `BindlessFreeList`, `GpuTexture` `Drop`, `bindless_index` |
| Upload + mip generation | `upload.rs` | `upload_texture`, `mip_count` |
| Index in instance data | `gpu_types.rs` | `InstanceData::texture` (`.x` albedo) |

## Related

- [Descriptor sets](../descriptor-sets/) — where set 0 sits in the full layout
- [Übershader](../ubershader-and-specialization/) — the shader that samples the array
- [Materials & PSOs](../material-and-pso-selection/) — why texture is not a pipeline key
