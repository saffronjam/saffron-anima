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

These are pure data and pure math: no scene mutation, no GPU, no playback state. The runtime
that advances a clip over time and writes the pose onto a rigged entity is a separate layer
built on top of this one.

## Clips, tracks, keyframes

A clip mirrors a glTF animation faithfully and losslessly. glTF models an animation as a set of
*channels*, each pairing a *sampler* (a keyframe curve: input times, output values, an
interpolation mode) with a *target* (a node and one of its translation / rotation / scale
properties). Saffron's `AnimTrack` is exactly one such channel:

```cpp
struct AnimTrack {
    i32 joint = -1;                 // stable index into SkinnedMeshComponent.bones
    std::string jointName;          // the glTF node name — the durable binding key
    enum class Path { Translation, Rotation, Scale } path;
    enum class Interp { Step, Linear, CubicSpline } interp;
    std::vector<f32> times;         // sampler input — strictly increasing, seconds
    std::vector<f32> values;        // sampler output — flat floats
};
```

A track binds to a joint by **stable index plus name**, never by an entt handle (handles are a
post-load cache and are not stable across a reload). The index is what the sampler writes through;
the name is the durable key the importer resolves the index from, so a clip survives a reimport
and — later — retargeting onto a different rig.

The `values` array is flat and its stride depends on the path and interpolation: a `vec3` per key
for translation and scale, a quaternion (`xyzw`) per key for rotation, and — for `CubicSpline` —
three elements per key in the order *in-tangent, value, out-tangent*. An `AnimClip` is just a name,
the list of tracks, and the duration (the maximum track end time).

The clip types live in **`Saffron.Geometry`**, next to `Vertex` and `Mesh`, because Geometry owns
the engine's mesh and file formats — the glTF walk that fills these tracks and the `.sanim` sidecar
that persists them belong there. `Saffron.Animation` only consumes them.

## The pose and the blend layer

A pose is the skeleton's transforms for one instant, kept *decomposed* — translation, rotation,
scale held separately — because that is the form clips sample into and that blends cleanly
(rotations slerp; a composed matrix does not):

```cpp
struct JointPose { glm::vec3 translation; glm::quat rotation; glm::vec3 scale; };

struct PoseBuffer {
    std::vector<JointPose> local;     // the sampled/animated TRS, one per joint
    std::vector<JointPose> override_; // external producers (IK/physics) write here
    std::vector<f32> weight;          // 0 = use local, 1 = use override_; per joint
};
```

`PoseBuffer` is indexed 1:1 with `SkinnedMeshComponent.bones`. The `override_` and `weight` arrays
are the **blend layer**: the seam later phases write through. The intended composition is
`out[i] = blend(local[i], override_[i], weight[i])` with a slerp on the rotation, so a foot-IK
solver or a powered ragdoll becomes *just another producer* writing `override_[i]` and raising
`weight[i]` — no change to the sampling code. In v1 every `weight[i]` is 0, so the layer is inert
and the pose is pure animation, but the shape is fixed now so the physics-ahead path needs no
rewrite. This is UE5's Physics Blend Weight model.

The load-bearing decision is that the pose lives **beside** the scene, not in it. The authored bone
`TransformComponent`s keep the rest pose and are never overwritten; the animated pose is a separate
runtime buffer, like the cached `WorldTransformComponent`. That makes previewing a clip
non-destructive — no scene dirtying, no snapshot/restore — which is exactly what scrubbing a clip in
Edit mode needs.

## Sampling

`sampleTrack(track, t)` evaluates one curve at time `t`, returning a `vec4` (a `vec3` in `xyz` for
translation/scale, a normalized quaternion as `xyzw` for rotation). It binary-searches the keys for
the segment bracketing `t`, then interpolates per the track's mode:

- **Step** holds the previous key's value across the segment.
- **Linear** lerps translation and scale componentwise. Rotation is **never** a component lerp — it
  uses `glm::slerp` between the two quaternion keys and normalizes, which keeps constant angular
  velocity and a unit result.
- **CubicSpline** is a Hermite spline. With $u = (t - t_0)/(t_1 - t_0)$ and segment length
  $\Delta = t_1 - t_0$, the value is
  $$p(u) = h_{00}\,p_0 + h_{10}\,\Delta m_0 + h_{01}\,p_1 + h_{11}\,\Delta m_1$$
  using the standard Hermite basis. The tangents are **scaled by $\Delta$** (the glTF requirement);
  the in/out tangents come from the surrounding keys' tangent elements. Rotation interpolates the
  four quaternion components this way and then normalizes.

`t` is **clamped** to the first and last key — clips do not extrapolate past their ends.

`sampleClip(clip, t, out)` samples every track into `out.local`. The caller sizes and pre-fills
`out.local` with the rest pose first; `sampleClip` only writes the joints a track targets, so a
joint with no track keeps its rest value, and a joint animated on only one channel keeps the rest
value on the others.

One gotcha runs through the rotation path: glTF stores quaternions as `[x, y, z, w]`, but
`glm::quat`'s constructor takes `(w, x, y, z)`. The sampler reads the flat `values` as `xyzw` and
reorders when constructing a `glm::quat`, matching the swap the skin import already does for bind
matrices.

A headless self-test (`runAnimationSelfTest`, run under `SAFFRON_SELFTEST`, mirroring the
`jointMatrices` self-test) samples known Step / Linear / CubicSpline keys and asserts endpoints are
exact and midpoints match — a slerp midpoint for rotation, and an asymmetric-tangent cubic bent to
`0.75` to prove the Hermite path actually runs.

## In the code

| What | File | Symbols |
|---|---|---|
| Clip + track types | `geometry.cppm` | `AnimClip`, `AnimTrack` |
| Pose + blend layer | `animation.cppm` | `JointPose`, `PoseBuffer` |
| Sampling | `animation.cpp` | `sampleTrack`, `sampleClip` |
| Self-test | `animation.cpp` | `runAnimationSelfTest` |

## Related

- [Vertex layout](../../geometry-and-assets/mesh-and-vertex-layout/) — the `VertexSkin` stream the palette skins
- [Scene & ECS](../../scene-and-ecs/) — `SkinnedMeshComponent`, the bone entities, `jointMatrices`
