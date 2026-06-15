# Phase 1 — AnimTrack/AnimClip generalization + .sanim format bump

**Status:** NOT STARTED

**Depends on:** Phase 0

## Why

One track model must carry bone tracks, node tracks, and morph-weights tracks so there is exactly one
sampler and one clip type (NO LEGACY, generalize-don't-parallel). This phase changes the *data shapes*
and the *sampler*, and replaces the `.sanim` format, before the importer (Phase 2) decodes into them.

## Grounding

- `AnimTrack` / `AnimClip` (`geometry.cppm:79-110`): `joint` index + `jointName` durable key, `Path ∈
  {Translation, Rotation, Scale}`, `Interp ∈ {Step, Linear, CubicSpline}`, flat `times`/`values`.
- `.sanim`: `SANimHeader` (32 B, `:406-415`), `SANimTrackRecord` (20 B, `:418-428`),
  `AnimFormatVersion = 1` (`:430`), `saveAnimationToBuffer` (`:1619`), `loadAnimationFromBytes`
  (bounds-checked cursor, `:1657-1731`).
- Sampler: `sampleTrack` (STEP/LINEAR/CUBICSPLINE, slerp for rotation, `animation.cpp`), `sampleClip`
  (`animation.cpp`), `sampleClipResolved` (jointName fallback, `:308-349`).
- The `.sanim` self-test (round-trip) and `runAnimationSelfTest` (`animation.cpp:766`) are the
  regression net to extend in this phase.

## Decisions (locked)

1. **`AnimTrack` gains a target kind and a unified durable name.**
   - `enum class Target : u8 { Bone, Node }`. `Bone` ⇒ `joint` indexes `SkinnedMeshComponent.bones`;
     `Node` ⇒ binds by `targetName` to a scene entity (Phase 4 resolution).
   - `jointName` is **renamed** `targetName` (one durable key for both kinds). Every reader updated in
     this change: `sampleClipResolved` (`animation.cpp:308-349`), the `.sanim` serde, the `nameToIndex`
     seeding in `tickAnimation` (`animation.cpp:632-648`), and the `AnimationClipDto.tracks` count.
   - `Path` gains `Weights`. For a `Weights` track, `joint = -1`, `Target = Node`, `targetName` is the
     mesh node's name, and a new `u32 morphCount` records N (targets per keyframe).
2. **Weights value layout = glTF.** `values` is per-keyframe blocks of N scalars laid end-to-end
   (`count = N·M`; CUBICSPLINE = `3·N·M`, ordered `[inTan[N], value[N], outTan[N]]` per key). This is
   the exact glTF "weights" stream so the importer copies the accessor verbatim.
3. **One sampler, value-kind dispatch — with the exact N-wide offsets.** `sampleTrack` returns a
   `glm::vec4` for T/S/R and is vec4-bound, so it **cannot** sample an N-wide weights block as-is. Add a
   `sampleWeights(track, t, out: std::vector<f32>&)` that reuses the *same* STEP/LINEAR/CUBICSPLINE core
   looped per target `j ∈ [0,N)`, with the glTF per-keyframe block offsets (this is the single most
   error-prone math in the feature, so it is specified exactly):
   - **STEP/LINEAR:** target `j` at keyframe `k` is `values[k*N + j]`; lerp componentwise between
     bracketing keys (no slerp — weights are scalars).
   - **CUBICSPLINE:** each keyframe block is `3·N` laid `[in[0..N), value[0..N), out[0..N)]`, so target
     `j`'s triplet at keyframe `k` is `in = values[k*3N + j]`, `value = values[k*3N + N + j]`,
     `out = values[k*3N + 2N + j]` — NOT contiguous per target. Reuse the existing dt-scaled Hermite
     (`m0 = deltaT*out_k`, `m1 = deltaT*in_{k+1}`); no quaternion normalize.
   This is one interpolation implementation looped over components, not a second math path. `sampleClip`
   routes a `Weights` track into the `PoseBuffer`'s new weights field (decision 4); a `Node` T/R/S track
   samples exactly like a bone track (`sampleTrack`) and writes into a node-pose slot (Phase 4 wires the
   destination).
4. **`PoseBuffer` carries weights.** Add `std::vector<f32> weightsLocal;` (the sampled morph weights)
   and `std::vector<f32> weightsOverride; std::vector<f32> weightsWeight;` mirroring the joint blend
   layer (`animation.cppm:36-41`) — the same sampled/override/weight shape, so morph weights compose
   with transitions/blending through the existing machinery rather than a side channel. Sized by the
   caller to N.
5. **`.sanim` format replaced (no v1 fallback).** Bump `AnimFormatVersion = 2`. `SANimTrackRecord`
   grows a `u8 target` (Bone/Node) and reuses `valueCount` for the weights stream; add a `u32
   morphCount` field (keep the struct a fixed multiple-of-4 size; re-assert `sizeof`). `loadAnimation
   FromBytes` reads v2 only — a v1 file is `Err("unsupported .sanim version")`. Old reader deleted.
   `saveAnimationToBuffer` writes v2. The `.sanim` round-trip self-test updated to cover a Node track
   and a Weights track.

## Edits

- `geometry.cppm`: `AnimTrack` (`:79-101`) — add `Target`, rename `jointName`→`targetName`, add
  `Weights` path + `morphCount`. `SANimTrackRecord` (`:418`) — add `target`/`morphCount`, re-assert
  size. `AnimFormatVersion` (`:430`) → 2. `saveAnimationToBuffer`/`loadAnimationFromBytes` rewritten for
  v2 (delete v1 read path). `toTrackPath` (`:515`) — Phase 2 adds the `weights` case; leave a TODO note
  only (not a second function).
- `animation.cppm`: `PoseBuffer` (`:36-41`) — add the three weights vectors. Declare
  `sampleWeights(...)`. Update `sampleClip` doc.
- `animation.cpp`: `sampleTrack`/`sampleClip`/`sampleClipResolved` — handle `Target`/`Weights`; one
  interpolation core. `tickAnimation` (`:632-648`) uses `targetName`. Extend `runAnimationSelfTest`
  with a Weights-track STEP/LINEAR/CUBICSPLINE assertion (endpoints exact, midpoint matches).
- Every other reader of `jointName` (grep the tree) renamed to `targetName` in the same change.

## Verification

- `make engine`; `make prepare-for-commit`.
- `runAnimationSelfTest` green incl. the new Weights cases; `.sanim` round-trip self-test green for
  Node + Weights tracks.
- A v1 `.sanim` byte buffer fed to `loadAnimationFromBytes` returns `Err` (no silent acceptance).

## Risks

- `targetName` rename touches several TUs (control DTO `AnimationClipDto`, serde, sceneedit). Mechanical
  but wide — do it atomically so no half-renamed reader compiles against the old field.
- `SANimTrackRecord` size: keep it 4-byte-aligned and re-`static_assert` the exact size, or the
  defensive cursor math in `loadAnimationFromBytes` (`:1699`) reads past records.
