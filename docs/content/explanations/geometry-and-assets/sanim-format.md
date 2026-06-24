+++
title = '.sanim format'
weight = 4
+++

# .sanim format

`.sanim` is a baked binary animation clip: a 32-byte header, the clip name, then one
self-describing record per track — bone, node-TRS, or morph-weight. Each glTF animation is decoded once
on import and written to its own `.sanim` image; the player reads it back directly at runtime. The format
is at `ANIM_FORMAT_VERSION` = 2 (the 24-byte record carrying `target` + `morph_count`); the loader rejects
any other version.

A clip is strictly a *separate image*. It is never folded into the [`.smesh`](../smesh-format/)
— the mesh format and its version stay untouched, and a rig with no clips bakes exactly the
same. The two share a discipline (a fixed `#[repr(C)]` Pod header, raw little-endian arrays,
a version field, a bounded defensive loader) but carry different magic so neither can be
mistaken for the other. Both also embed as chunks in a [`.smodel`](../smodel-container/): a
`SANM` chunk is a standalone `.sanim` image read verbatim.

## Layout

A fixed 32-byte header, then the clip name bytes, then per track a 24-byte record followed by
the track's target name, times, and values:

```rust
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Pod, Zeroable)]
struct SANimHeader {
    magic: [u8; 4],     // b"SANM"
    version: u32,
    track_count: u32,
    duration: f32,      // clip length, seconds
    name_len: u32,      // clip-name bytes that follow the header
    reserved: [u32; 3],
}
const _: () = assert!(size_of::<SANimHeader>() == 32);

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Pod, Zeroable)]
struct SANimTrackRecord {   // target name, times, then values follow it
    index: i32,             // bone index for a Bone track; unused (-1) for a Node track
    target: u8,             // AnimTarget (Bone/Node) discriminant byte
    path: u8,               // AnimPath  (Translation/Rotation/Scale/Weights) discriminant byte
    interp: u8,             // AnimInterp (Step/Linear/CubicSpline) discriminant byte
    pad: u8,                // explicit pad to a 4-byte boundary
    morph_count: u32,       // weights per keyframe for a Weights track, else 0
    name_len: u32,          // the glTF node / morph target name (the durable binding key)
    time_count: u32,        // keyframe count
    value_count: u32,       // flat float count (see the track model)
}
const _: () = assert!(size_of::<SANimTrackRecord>() == 24);
```

The `times` and `values` arrays are written as raw little-endian `f32` blobs, in the exact
flat layout the sampler reads — `Vec3` per key for translation/scale, a quaternion `xyzw` per
key for rotation, the `morph_count` weights per key for a `Weights` track, and the
`3×(in-tangent, value, out-tangent)` stride for cubic-spline tracks. See the
[animation data model](../../animation/animation-data-model/) for what those arrays mean. The
`AnimTarget`/`AnimPath`/`AnimInterp` discriminant bytes are pinned by unit tests, and their
`from_u8` maps a byte through an explicit `match` (never a transmute), so a malformed record
can never produce UB.

## Binding a track to its target

`target` selects what the track drives: a `Bone` track carries a bone index (its position in the
skinned mesh's bones, fast) and the source node name (durable); a `Node` track binds purely by name; a
`Weights` track names the morph target. The index is the source-of-truth glTF joint order fixed at
import; the name survives a reorder or reimport, so a later evaluator can re-resolve a stale index — or
bind a node / morph target — by name. Both are written so neither binding is lost.

## Loading defensively

`load_animation_from_bytes` validates the magic and version, then walks the rest with a
bounded `Cursor`: its `take(n)` returns the next `n` bytes or `Error::Truncated` if fewer
remain. Every field — the clip name, each track record, and each track's name/times/values —
is taken only if that many bytes remain, so a lying `time_count` or `value_count` can never
drive a giant allocation. A short or truncated image returns an `Err` rather than reading
past the buffer, the same discipline [`load_mesh_from_bytes`](../smesh-format/) applies.

## Round-trip coverage

The codec is covered by unit tests in `sanim.rs`: a synthetic clip round-trips through
`save_animation_to_buffer` / `load_animation_from_bytes` with every field byte-for-byte, and
a bad magic / truncated image returns the expected `Err`. The pinned discriminant bytes are
asserted in `geometry/src/lib.rs`.

## In the code

| What | File | Symbols |
|---|---|---|
| Header + track record | `geometry/src/sanim.rs` | `SANimHeader`, `SANimTrackRecord` |
| Version constant | `geometry/src/sanim.rs` | `ANIM_FORMAT_VERSION` |
| Write path | `geometry/src/sanim.rs` | `save_animation`, `save_animation_to_buffer` |
| Defensive load | `geometry/src/sanim.rs` | `load_animation`, `load_animation_from_bytes` |
| Clip decode on import | `geometry/src/gltf_import.rs` | `decode_clips` |
| Container registration | `assets/src/import.rs` | `bake_model`, `catalog_rows_for_model` |

> [!NOTE]
> Importing a rig writes one `.sanim` image per glTF animation as a `SANM` chunk inside the
> model's [`.smodel`](../smodel-container/) and registers an `AssetType::Animation` catalog
> row, so the `.smesh` format never grows an animation section.

## Related

- [Animation data model](../../animation/animation-data-model/) — the clip/track types this serializes
- [.smesh format](../smesh-format/) — the sibling mesh image
- [The .smodel container](../smodel-container/) — the file that embeds a `.sanim` as a chunk
- [Model import](../gltf-and-obj-import/) — where glTF animations are decoded
