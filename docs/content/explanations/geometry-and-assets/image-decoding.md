+++
title = 'Image decoding'
weight = 4
+++

# Image decoding

Image decoding turns encoded image bytes — PNG, JPG, or an HDR float source — into raw,
tightly packed pixels. A GPU sampler reads uncompressed pixels in a fixed format, so an
encoded file must be decoded before it can become a texture.

Encoded textures reach the engine from several sources: a file on disk, an external image
referenced by a model, or bytes baked into a [`.smodel`](../smodel-container/) chunk. The
decode functions normalize all of them into one of two shapes the upload path accepts,
through the `image` crate.

## Two output shapes

The 8-bit path produces a `DecodedImage`: always four channels, 8 bits each, no row padding.
The float path produces a `DecodedImageFloat`: four channels of linear `f32`, for `.hdr`-class
sources whose radiance may exceed `1.0`.

```rust
pub struct DecodedImage {
    pub rgba: Vec<u8>,    // tightly packed, width * height * 4 bytes
    pub width: u32,
    pub height: u32,
}

pub struct DecodedImageFloat {
    pub rgba: Vec<f32>,   // tightly packed, width * height * 4 floats; linear radiance
    pub width: u32,
    pub height: u32,
}
```

`to_rgba8` calls `image::DynamicImage::to_rgba8` regardless of the source's channel count, so
a 3-channel JPG and a 4-channel PNG both come out as RGBA8. `to_rgba32f` does the same with
`to_rgba32f` for the HDR path. That uniformity lets the upload path assume a single format
per shape.

## File versus memory

Each shape has a file entry point and a memory entry point, differing only in where the
`image` crate reads from:

- `decode_image` / `decode_image_hdr` read a path (`image::open`), used when resolving a
  texture file out of the asset directory.
- `decode_image_from_memory` / `decode_image_from_memory_hdr` decode a byte slice
  (`image::load_from_memory`), used for texture bytes the [importer](../gltf-and-obj-import/)
  carried out of a glTF buffer view or a `.smodel` `STEX` chunk.

A decode failure becomes an `Err(Error::Decode(…))`. The decoded pixels are owned by the
returned `Vec`, so no caller manages a separate buffer lifetime.

## Where decoded pixels go

Decoding is half of a texture upload. The [asset server](../asset-server-and-catalog/) passes
the decoded pixels to the renderer's upload seam — `GpuUploader::upload_texture` for RGBA8
(with an `srgb: bool` it sets `true` for albedo, since albedo is authored in sRGB and must be
sampled with hardware sRGB-to-linear so the [BRDF](../../lighting-and-brdf/cook-torrance-brdf/)
sees linear color) and `GpuUploader::upload_texture_float` for the HDR float path (an
`R16G16B16A16_SFLOAT` upload). The decode itself is format-agnostic; the colorspace decision
belongs to the upload step, not here.

## In the code

| What | File | Symbols |
|---|---|---|
| Decoded results | `geometry/src/types.rs` | `DecodedImage`, `DecodedImageFloat` |
| Decode RGBA8 | `geometry/src/image_decode.rs` | `decode_image`, `decode_image_from_memory` |
| Decode HDR float | `geometry/src/image_decode.rs` | `decode_image_hdr`, `decode_image_from_memory_hdr` |
| Upload that consumes it | `assets/src/scan.rs`; `assets/src/gpu.rs` | `register_texture_bytes`, `register_hdr_texture_bytes`, `GpuUploader::upload_texture` |

## Related

- [Model import](../gltf-and-obj-import/) — produces the encoded texture bytes
- [Asset catalog](../asset-server-and-catalog/) — decodes, uploads, caches
- [Cook-Torrance BRDF](../../lighting-and-brdf/cook-torrance-brdf/) — why albedo is sampled sRGB→linear
