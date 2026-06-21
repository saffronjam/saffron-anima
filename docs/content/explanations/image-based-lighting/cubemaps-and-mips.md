+++
title = 'Cubemaps and mips'
weight = 2
math = true
+++

# Cubemaps and mips

A cubemap is a texture of six square faces arranged as the sides of a cube, sampled by a 3D direction rather than a 2D coordinate. Image-based lighting stores the environment, the diffuse irradiance, and the roughness-prefiltered specular this way, because each is a function of direction. Two of these cubes carry mip chains.

A compute shader cannot write through a cube view; it writes a 2D-array storage image. Each IBL cube is therefore one image carried with two kinds of view: a cube view for sampling, and per-mip 2D-array views for the bake to write through.

## One image, two view shapes

`IblCube::new` creates a `CUBE_COMPATIBLE` image with 6 array layers, `N` mip levels, and the `R16G16B16A16_SFLOAT` HDR format, with both `SAMPLED` and `STORAGE` usage. The image's own view is `CUBE`, covering all mips and all six layers, and the mesh fragment samples it as a `SamplerCube`. A direction goes in and a filtered color comes out, with trilinear mip selection for the prefiltered cube.

The cube view cannot serve as a storage target. Cube sampling is a read-only abstraction, and the hardware writes layers, not faces. The bake therefore builds transient `TYPE_2D_ARRAY` views, one per mip it writes, via `IblCube::storage_view`:

```rust
.view_type(vk::ImageViewType::TYPE_2D_ARRAY)
.subresource_range(vk::ImageSubresourceRange {
    aspect_mask: vk::ImageAspectFlags::COLOR,
    base_mip_level: mip,
    level_count: 1,
    base_array_layer: 0,
    layer_count: 6,
});
```

The compute shaders bind these as `RWTexture2DArray<float4>` and address them by `tid.z`, the face index 0..5. The same memory is seen as a cube to sample and as a layered 2D array to write.

## Why mips, and how many

The diffuse irradiance and environment cubes are single-mip. Irradiance is already fully blurred by its hemisphere convolution, and the environment is only sampled at level 0. The prefiltered specular cube is the one that needs a chain: each mip holds the environment blurred by an increasing roughness, so a rough surface samples a coarse mip and a mirror samples mip 0.

The chain is `IBL_PREFILTER_MIPS = 5` levels over a `128²` base. Roughness maps linearly onto it: mip $m$ is baked at roughness $m/(\text{mips}-1)$, so mip 0 is roughness 0 (sharp) and mip 4 is roughness 1 (fully rough). The mesh shader picks the mip with `roughness * IblPrefilterMaxMip`, where `IblPrefilterMaxMip = 4.0`.

## Bake sizes

The cubes are deliberately small. IBL is a low-frequency signal, so a tiny irradiance cube is indistinguishable from a large one, and the one-time bake on a software rasterizer stays quick.

| Texture | Size | Mips |
|---|---|---|
| Environment | `128²` × 6 | 1 |
| Irradiance | `32²` × 6 | 1 |
| Prefiltered | `128²` × 6 | 5 |
| BRDF LUT | `256²` (2D) | 1 |

The BRDF LUT is the exception: a flat 2D image, not a cube, built by `IblImage::new` with the same HDR format and sampled + storage usage.

## In the code

| What | File | Symbols |
|---|---|---|
| Cube image + cube view | `engine/crates/rendering/src/ibl.rs` | `IblCube`, `IblCube::new` |
| Per-mip storage views | `engine/crates/rendering/src/ibl.rs` | `IblCube::storage_view` |
| 2D LUT / atmosphere images | `engine/crates/rendering/src/ibl.rs` | `IblImage`, `IblImage::new` |
| Sizes and mip count | `engine/crates/rendering/src/ibl.rs` | `IBL_ENV_SIZE`, `IBL_IRRADIANCE_SIZE`, `IBL_PREFILTER_SIZE`, `IBL_PREFILTER_MIPS`, `IBL_LUT_SIZE` |
| Mip ↔ roughness constant | `engine/assets/shaders/lighting.slang` | `IblPrefilterMaxMip` |

> [!NOTE]
> The `IblPrefilterMaxMip = 4.0` in `lighting.slang` and `IBL_PREFILTER_MIPS = 5` in `ibl.rs` are coupled by hand (`MaxMip == Mips - 1`). There is no compile-time check across the shader/Rust boundary, so changing the mip count means editing both. A comment in each file flags the pairing.

## Related

- [Specular prefilter](../specular-prefilter/) — what fills the mip chain
- [Baking](../ibl-bake-pass/) — where the transient views are created and freed
- [Lighting and BRDF](../../lighting-and-brdf/) — the other cube-image user (point shadows)
