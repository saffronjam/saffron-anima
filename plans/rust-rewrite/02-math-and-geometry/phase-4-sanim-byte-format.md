# Phase 4 — The `.sanim` byte format + the anim track/clip types

**Status:** COMPLETED

**Depends on:** phase-1 (crate + `Error`/`Result`), phase-3 (the byte-format discipline: `#[repr(C)]`
Pod headers, `bytemuck`, size asserts, the file I/O wrappers — `.sanim` reuses the same shape).

## Goal

Port the `AnimTrack` / `AnimClip` types and the sidecar `.sanim` (`SANM`) byte format: the 32-byte
`SANimHeader`, the 20-byte per-track `SANimTrackRecord`, and the bounded-read decode —
`save_animation_to_buffer`, `load_animation_from_bytes`, plus the file wrappers `save_animation`,
`load_animation`. Byte-for-byte identical so a `.smodel` SANM chunk and a standalone `.sanim` read the
same.

## Why this shape (NO LEGACY)

`.sanim` is the second byte format and mirrors `.smesh`'s discipline (geometry.cppm:264 "Little-endian
raw, versioned, mirroring the .smesh shape"). The `AnimTrack::Path` and `AnimTrack::Interp` enums are
`#[repr(u8)]` with **pinned discriminants** (`Translation=0/Rotation=1/Scale=2`,
`Step=0/Linear=1/CubicSpline=2`) because those bytes are written into the track record
(`record.path = static_cast<u8>(track.path)`, geometry.cppm:1639-1640) and read back by `as` cast
(geometry.cppm:1708-1709). On read, an out-of-range discriminant must not be UB: the Rust decode maps
the byte through an explicit `match` (or `TryFrom`) rather than `transmute`, keeping `#![deny(unsafe_code)]`.

The clip stores `times` and `values` as flat `Vec<f32>` exactly as the glTF sampler does — 3 floats per
key for T/S, 4 (xyzw quat) for R, 3× for CubicSpline (geometry.cppm:97-99). This layout is not
re-interpreted here; it is stored and round-tripped verbatim (the animation crate interprets it).

The C++ decode uses a manual `overran` flag + a `take(count)` cursor that bounds-checks every field so a
malformed count cannot drive a giant allocation (geometry.cppm:1674-1729). In Rust this becomes a small
`Cursor` over the slice whose `take(n) -> Result<&[u8]>` returns `Err(Truncated)` on overrun, and `?`
replaces the flag — one code path, shorter, same anti-DoS guarantee.

## Grounding (real files/symbols)

- `engine-old/source/saffron/geometry/geometry.cppm`:
  - `AnimTrack` (79-101): `joint:i32`, `jointName:String`, `Path`/`Interp` `u8` enums, `times`/`values`
    flat `f32` vectors.
  - `AnimClip` (105-110): `name`, `duration`, `tracks`.
  - `SANimHeader` (406-415, 32 B asserted): `magic='SANM'`, `version`, `trackCount`, `duration:f32`,
    `nameLen`, `reserved[3]`.
  - `SANimTrackRecord` (418-428, 20 B asserted): `joint:i32`, `path:u8`, `interp:u8`, `pad:u16`,
    `nameLen`, `timeCount`, `valueCount`.
  - `AnimFormatVersion = 1` (430).
  - `saveAnimationToBuffer` (1619-1650): header, name bytes, then per-track {record, name, times,
    values}.
  - `loadAnimationFromBytes` (1657-1731): magic/version check, the bounded `take` cursor, per-track
    decode, `overran` → `Err`.
  - `saveAnimation` (1652-1655), `loadAnimation` (1733-1746).
  - `toTrackPath` (515-526), `toTrackInterp` (529-540) — the glTF→enum maps (used by the importer in
    phase 5; the enum values they produce are pinned here).

## Plan

1. `#[repr(u8)] enum Path { Translation = 0, Rotation = 1, Scale = 2 }` and
   `#[repr(u8)] enum Interp { Step = 0, Linear = 1, CubicSpline = 2 }`, each with a `from_u8(u8) ->
   Result<Self>` (explicit `match`, `Err` on unknown). `AnimTrack` / `AnimClip` structs.
2. Private `#[repr(C)]` Pod `SANimHeader` (32 B) and `SANimTrackRecord` (20 B, keeping the `pad: u16`)
   with `const` size asserts. `ANIM_FORMAT_VERSION: u32 = 1`.
3. `save_animation_to_buffer(clip: &AnimClip) -> Vec<u8>` — append header, name, then for each track the
   record + name + `cast_slice(times)` + `cast_slice(values)`.
4. `load_animation_from_bytes(bytes: &[u8]) -> Result<AnimClip>` with a `Cursor { bytes, pos }` whose
   `take(n) -> Result<&[u8]>` bounds-checks; decode the header, the clip name, and each track (record,
   name, times, values), mapping `path`/`interp` through `from_u8`.
5. File wrappers `save_animation(clip, path)` / `load_animation(path)` over `std::fs`, prefixing the
   path into the error message as the C++ does (geometry.cppm:1743).

## Acceptance gate

- `cargo build -p saffron-geometry` + workspace compile.
- A `#[test]` round-trip (from `runGeometrySelfTest`'s `.sanim` block, geometry.cppm:2246-2262): build a
  clip with two tracks (one Rotation/4-wide, one Translation/3-wide), `save_animation_to_buffer`,
  `load_animation_from_bytes`, assert name, duration, track count, each track's joint/path/interp/
  jointName and the exact `times`/`values` floats round-trip.
- A golden-bytes `#[test]`: a fixed clip encodes to a header with `magic == b"SANM"`, `version == 1`,
  the expected `track_count`/`duration`/`name_len`, and a track record with the pinned `path`/`interp`
  bytes and the `pad == 0`.
- Anti-DoS rejection `#[test]`s: a header claiming a huge `track_count`/`nameLen`/`timeCount` over a
  short buffer returns `Err(Truncated)` (no allocation blow-up); bad magic → `Err(BadMagic)`; version 2
  → `Err(UnsupportedVersion(2))`.
- `cargo clippy` clean; no `unsafe`.
