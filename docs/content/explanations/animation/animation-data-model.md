+++
title = 'Animation data model'
weight = 1
math = true
+++

# Animation data model

An animation clip is a bundle of per-joint curves; sampling one at a time produces a *pose* —
a local transform for every joint — that the skeleton then composes into world matrices. This
page describes the three things that layer is built from: the clip/track keyframe model, the
decomposed joint pose plus its blend layer, and the sampler that turns one into the other.

These are pure data and pure math: no scene mutation, no GPU, no playback state. The `saffron-animation`
crate is FFI-free and has no GPU concept — it consumes the clip types from `saffron-geometry`, reads
and writes `saffron-scene` components, and emits a per-bone pose override. The runtime that advances a
clip over time and writes the pose onto a rigged entity is a separate layer built on top of this one.

## Clips, tracks, keyframes

A clip mirrors a glTF animation faithfully and losslessly. glTF models an animation as a set of
*channels*, each pairing a *sampler* (a keyframe curve: input times, output values, an
interpolation mode) with a *target* (a node and one of its translation / rotation / scale
properties). Saffron's `AnimTrack` is exactly one such channel:

```rust
pub struct AnimTrack {
    pub joint: i32,             // stable index into SkinnedMesh.bones; -1 until resolved
    pub joint_name: String,     // the glTF node name — the durable binding key
    pub path: AnimPath,         // Translation | Rotation | Scale
    pub interp: AnimInterp,     // Step | Linear | CubicSpline
    pub times: Vec<f32>,        // sampler input — strictly increasing, seconds
    pub values: Vec<f32>,       // sampler output — flat floats
}
```

A track binds to a joint by **stable index plus name**, never by a live ECS handle (handles are a
post-load cache rebuilt by the hierarchy relink and are not stable across a reload). The index is
what the sampler writes through; the name is the durable key the importer resolves the index from,
so a clip survives a reimport and — later — retargeting onto a different rig.

The `values` array is flat and its stride depends on the path and interpolation: a `Vec3` per key
for translation and scale, a quaternion (`xyzw`) per key for rotation, and — for `CubicSpline` —
three elements per key in the order *in-tangent, value, out-tangent*. An `AnimClip` is just a name,
the list of tracks, and the duration (the maximum track end time).

The clip types live in **`saffron-geometry`**, next to `Vertex` and `Mesh`, because Geometry owns
the engine's mesh and file formats — the glTF walk that fills these tracks and the `.sanim` byte
format (a `SANM` chunk in the `.smodel` container) that persists them belong there. `saffron-animation`
only consumes them.

## The pose and the blend layer

A pose is the skeleton's transforms for one instant, kept *decomposed* — translation, rotation,
scale held separately — because that is the form clips sample into and that blends cleanly
(rotations slerp; a composed matrix does not):

```rust
pub struct JointPose {
    pub translation: Vec3,
    pub rotation: Quat,    // unit quaternion, glam xyzw order
    pub scale: Vec3,
}

pub struct PoseBuffer {
    pub local: Vec<JointPose>,     // the sampled/animated TRS, one per joint
    pub override_: Vec<JointPose>, // external producers (IK/physics) write here
    pub weight: Vec<f32>,          // 0 = use local, 1 = use override_; per joint
}
```

`PoseBuffer` is indexed 1:1 with `SkinnedMesh.bones`. The `override_` and `weight` vectors are the
**blend layer**: the seam later pose producers write through. The intended composition is
`blend_joint(local[i], override_[i], weight[i])` with a slerp on the rotation, so a foot-IK solver
or a powered ragdoll becomes *just another producer* writing `override_[i]` and raising `weight[i]`
— no change to the sampling code. In v1 the `override_`/`weight` vectors stay empty/zero, so the
layer is inert and the pose is pure animation, but the shape is fixed now so the physics-ahead path
needs no rewrite. This is UE5's Physics Blend Weight model.

The load-bearing decision is that the pose lives **beside** the scene, not in it. The authored bone
`Transform`s keep the rest pose and are never overwritten; the animated pose is a separate runtime
buffer, written onto each driven bone as a `PoseOverride` component that world composition prefers.
That makes previewing a clip non-destructive — no scene dirtying, no snapshot/restore — which is
exactly what scrubbing a clip in Edit mode needs.

## Sampling

`sample_track(track, t)` evaluates one curve at time `t`, returning a `Vec4` (a `Vec3` in `xyz` for
translation/scale, a normalized quaternion as `xyzw` for rotation). It binary-searches the keys
(`partition_point`) for the segment bracketing `t`, then interpolates per the track's mode:

- **Step** holds the previous key's value across the segment.
- **Linear** lerps translation and scale componentwise. Rotation is **never** a component lerp — it
  uses `Quat::slerp` between the two quaternion keys and normalizes, which keeps constant angular
  velocity and a unit result.
- **CubicSpline** is a Hermite spline. With $u = (t - t_0)/(t_1 - t_0)$ and segment length
  $\Delta = t_1 - t_0$, the value is
  $$p(u) = h_{00}\,p_0 + h_{10}\,\Delta m_0 + h_{01}\,p_1 + h_{11}\,\Delta m_1$$
  using the standard Hermite basis. The tangents are **scaled by $\Delta$** (the glTF requirement);
  the in/out tangents come from the surrounding keys' tangent elements. Rotation interpolates the
  four quaternion components this way and then normalizes.

`t` is **clamped** to the first and last key — clips do not extrapolate past their ends.

`sample_clip(clip, t, out)` samples every track into `out.local`. The caller sizes and pre-fills
`out.local` with the rest pose first; `sample_clip` only writes the joints a track targets, so a
joint with no track keeps its rest value, and a joint animated on only one channel keeps the rest
value on the others.

Quaternions ride straight through: glTF stores `[x, y, z, w]` and glam's `Vec4` and `Quat` share
that lane order, so `Quat::from_vec4` reads a sampled rotation with no reorder — the swizzle the
older toolchains needed is gone.

The crate's unit tests sample known Step / Linear / CubicSpline keys and assert endpoints are exact
and midpoints match — a slerp midpoint for rotation, and an asymmetric-tangent cubic bent to `0.75`
to prove the Hermite path actually runs.

## In the code

| What | File | Symbols |
|---|---|---|
| Clip + track types | `engine/crates/geometry/src/types.rs` | `AnimClip`, `AnimTrack`, `AnimPath`, `AnimInterp` |
| Pose + blend layer | `engine/crates/animation/src/pose.rs` | `JointPose`, `PoseBuffer` |
| Sampling | `engine/crates/animation/src/sample.rs` | `sample_track`, `sample_clip` |
| `.sanim` byte format | `engine/crates/geometry/src/sanim.rs` | `encode`, `decode` |

## Related

- [Vertex layout](../../geometry-and-assets/mesh-and-vertex-layout/) — the `VertexSkin` stream the palette skins
- [Scene & ECS](../../scene-and-ecs/) — `SkinnedMesh`, the bone entities, `joint_matrices`
