+++
title = 'Morph targets'
weight = 6
math = true
+++

# Morph targets

A morph target — a blend shape — is a stored per-vertex offset from a mesh's base pose. Driving a
weight from 0 to 1 slides the mesh toward that offset; several targets blended together give facial
expressions, corrective shapes, and other deformations a skeleton cannot express. Anima imports morph
targets from glTF, stores them sparsely, and applies them on the GPU **before** skinning, so a morphed
mesh flows through the rest of the frame as an ordinary deformed vertex stream.

The weight curve is just another animation channel: an [`AnimTrack`]({{< relref "animation-data-model" >}})
with `path = Weights` carries `morph_count` weights per keyframe, sampled by the same evaluator that
drives bone and node tracks. There is one clip model for all three.

## Sparse storage

Most vertices do not move for most targets, so a dense per-target copy of the mesh would be almost all
zeros. Each target instead stores only the vertices it actually perturbs:

```rust
#[repr(C)]
pub struct MorphDelta {
    pub vertex_index: u32,  // which base vertex this delta perturbs
    pub d_position: Vec3,   // position offset
    pub d_normal: Vec3,     // normal offset
}                           // 28 B, bytemuck::Pod, const-asserted
```

The engine `Vertex` (32 B) carries no tangent stream, so a tangent delta would be dead weight — the
deform shader re-derives the tangent against the morphed normal instead. Import drops any delta whose
position and normal offsets are both below `MORPH_DELTA_EPSILON_SQ`, so a target keeps only the
vertices it genuinely moves.

A `.smesh` carries its targets in an optional morph section, gated by the `MESH_FLAG_MORPH` bit (see
[the `.smesh` format]({{< relref "smesh-format" >}})). Spawn seeds a durable `MorphComponent { weights,
names }` on the mesh-bearing entity — import-managed identity like `SkinnedMesh`, neither addable nor
removable — with the per-target names from `mesh.extras.targetNames` (or synthesized `morph_{k}`).

## GPU deform: fixed-point atomic scatter

The morph compute pass writes $\text{base} + \sum_i w_i \cdot \delta_i$ into the shared deformed-vertex
buffer. It is a three-pass kernel (`morph.slang`), the same shape Unreal uses, chosen because integer
atomics **commute** — the accumulated sum is bit-identical regardless of GPU thread order, which a
floating-point atomic add is not. That determinism is what lets a golden-buffer test and the llvmpipe CI
GPU agree.

1. **Clear** — one thread per vertex zeroes a per-vertex fixed-point accumulator (6 × `i32`).
2. **Scatter** — one thread per active `(target, delta)` quantizes $w \cdot \delta$ to fixed point
   (`MORPH_FIXED_SCALE = 65536`) and `atomicAdd`s it into the accumulator.
3. **Resolve** — one thread per vertex dequantizes, adds the base vertex, renormalizes the normal, and
   writes the 32 B `Vertex` to the deformed buffer at the instance's offset.

Each frame the CPU compacts the *active* targets — those whose weight clears `MORPH_WEIGHT_THRESHOLD` —
into a flat list, so the kernel never scatters a zero-weight target. An unskinned morph mesh draws the
deformed buffer directly as a static stream; a skinned mesh has the skin pass run after, on the same
buffer slice — one deformed-buffer contract for both.

> [!NOTE]
> Morph runs **before** skin and writes the same deformed buffer, so the data dependency *is* the
> ordering — the render graph derives the morph → skin barrier from both passes writing that buffer, not
> from a hand-placed flag.

## Motion vectors and ray tracing

The deform carries through the two consumers that read the final deformed slice. The motion pass runs a
second morph dispatch on the **previous** frame's weights into the prev-deformed buffer (the twin of the
skin prev-pose path), so a morphing mesh produces real deformation motion vectors, not just object
motion. The ray-traced BLAS refits over the post-morph slice each frame; an unskinned morph instance is
placed in the TLAS at its node world matrix, a skinned one at identity (its deformed vertices are already
world-space).

## Driving weights

| What | File | Symbols |
|---|---|---|
| Sparse delta + CPU aggregates | `geometry/src/types.rs` | `MorphDelta`, `MorphTarget`, `MorphData` |
| Durable + runtime weights | `scene/src/component.rs` | `MorphComponent`, `MorphWeightOverride` |
| The deform kernel | `assets/shaders/morph.slang` | three-pass clear / scatter / resolve |
| GPU buffers + dispatch | `rendering/src/skinning.rs` | `record_morph`, `wire_morph_dispatches`, `MORPH_FIXED_SCALE` |
| Active-target compaction | `rendering/src/instancing.rs` | `build_active_targets`, `MORPH_WEIGHT_THRESHOLD` |
| Control + Luau | `control/src/commands_animation.rs`; `script/src/entity.rs` | `set-morph-weights`, `get-morph-weights`; `Entity:set_morph_weights` |

The control plane exposes `set-morph-weights` / `get-morph-weights` (canonical `0..1`, never `0..100`),
and `Entity:set_morph_weights({...})` reaches the same write seam from Luau. The Inspector renders one
`0..1` slider per target, labelled by the durable target names.
