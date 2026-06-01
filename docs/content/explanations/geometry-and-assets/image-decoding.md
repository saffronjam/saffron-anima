+++
title = 'Image decoding'
weight = 4
+++

# Image decoding

Textures arrive as encoded PNG or JPG bytes: a file on disk, an external image referenced
by a model, or bytes embedded inside a glTF. `decodeImage` and `decodeImageFromMemory` turn
any of those into tightly packed RGBA8 pixels through stb_image, the only form the GPU
upload path accepts.

## One output shape

Both decoders produce a `DecodedImage`: always four channels, always 8 bits each, no row
padding.

```cpp
struct DecodedImage
{
    std::vector<u8> rgba;   // tightly packed, width*height*4 bytes
    u32 width = 0;
    u32 height = 0;
};
```

stb_image is asked for `STBI_rgb_alpha` regardless of the source's channel count, so a
3-channel JPG and a 4-channel PNG both come out as RGBA8. That uniformity lets the upload
path assume a single format.

## File versus memory

The two entry points differ only in where stb_image reads from. `decodeImage` reads a path,
used when resolving a copied texture file out of the asset directory.
`decodeImageFromMemory` decodes a byte vector, used for the albedo bytes the
[importer](../gltf-and-obj-import/) carried out of a glTF buffer view or an external file.
Both copy the decoded pixels into the `rgba` vector and immediately `stbi_image_free` the
stb buffer, so ownership is the `std::vector`. A decode failure becomes an `Err`. The
channel count stb reports is ignored, since the forced `STBI_rgb_alpha` already normalized
it.

## Where decoded pixels go

Decoding is half of a texture import. The [asset server](../asset-server-and-catalog/)
passes the decoded RGBA8 to `uploadTexture` with a trailing `true` that requests an sRGB
format. Albedo is authored in sRGB and must be sampled with hardware sRGB-to-linear
conversion so the [BRDF](../../lighting-and-brdf/cook-torrance-brdf/) sees linear color. The
decode itself is format-agnostic; the sRGB decision is made at upload, not here.

## In the code

| What | File | Symbols |
|---|---|---|
| Decoded result | `geometry.cppm` | `DecodedImage` |
| Decode from a file | `geometry.cppm` | `decodeImage` |
| Decode from bytes | `geometry.cppm` | `decodeImageFromMemory` |
| Upload that consumes it | `assets.cppm` | `registerTextureBytes`, `loadTextureAsset` |

## Related

- [Model import](../gltf-and-obj-import/) — produces the encoded albedo bytes
- [Asset catalog](../asset-server-and-catalog/) — decodes, uploads, caches
- [Cook-Torrance BRDF](../../lighting-and-brdf/cook-torrance-brdf/) — why albedo is sampled sRGB→linear
