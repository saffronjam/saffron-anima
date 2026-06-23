+++
title = 'FXAA'
weight = 2
+++

# FXAA

FXAA (Fast Approximate Anti-Aliasing) is a post-process technique: it finds high-contrast
edges in the finished image and blurs along them. It operates only on the final color buffer,
never on how the scene is rasterized, so it costs one screen-sized compute dispatch and a few
texture samples per pixel.

Because it works from the image alone, FXAA smooths edges that rasterization-time anti-aliasing
cannot reach: alpha-tested cutouts, specular highlights, and shader aliasing. The cost is
fidelity. It cannot distinguish a real edge from sharp texture detail, so it softens both
wherever it fires.

## How it works

The shader works in luma — perceived brightness, `dot(c, float3(0.299, 0.587, 0.114))`. Each
invocation handles one pixel through four steps:

1. **Sample a cross.** Read the center pixel and its four diagonal neighbours, take each luma,
   and find the local min and max. Their difference is the contrast range.

2. **Skip flat regions.** If the range is below a threshold relative to the brightest sample,
   this is not an edge: write the original pixel and return. Most of the screen takes this
   branch, which is why FXAA is cheap.

3. **Find the edge direction.** The diagonal luma gradient gives a 2D direction perpendicular
   to the edge. It is normalized and clamped to a maximum span so the blur never reaches more
   than a few texels.

4. **Blend along it.** Sample two pairs of points along the edge direction. The wider 4-tap
   average is preferred, but when it overshoots the local luma range the shader falls back to
   the tighter 2-tap average.

The blur runs only on texels that pass the edge test, and only along the detected edge, which
keeps it from washing out the whole frame.

## In Anima

With FXAA enabled, the scene renders into a 1× scratch image instead of straight into the
offscreen. A compute pass reads the scratch as a sampler and writes the anti-aliased result
into the offscreen as a storage image. This is a read-from-A, write-to-B variant of the
[compute post-process pattern](../../screen-space-and-post/compute-post-process-pattern/),
rather than an in-place pass like tonemap. The scratch matches the offscreen format and is
shared with TAA, since both consume the scene's pre-AA result.

`add_fxaa_pass` declares the dispatch's two resource uses: `SampledReadCompute` on the scratch
(`scene_output`) and `StorageImageRwCompute` on the offscreen (`color`). The graph derives the
transitions from those: the scratch goes to a sampled-in-compute layout, the offscreen to
`GENERAL` for the storage write, then on to shader-read-only for the present blit. None of that
is written by hand. The shader, `fxaa.slang`, is the FXAA 3 console variant; it compiles to
`shaders/fxaa.spv` and the PSO is requested through `Pipelines::request_fxaa`.

## In the code

| What | File | Symbols |
|---|---|---|
| Mode switch | `aa.rs` | `Aa::set`, `Aa::fxaa` |
| Scratch target (shared with TAA) | `view_target.rs` | `ViewTarget::build_aa_targets`, `scratch` |
| Descriptor set (scratch → offscreen) | `view_target.rs`, `descriptors.rs` | `write_aa_sets`, `fxaa_set`, `Descriptors::fxaa_set_layout` |
| Pass + dispatch in the graph | `renderer.rs` | `add_fxaa_pass` |
| PSO | `pipelines.rs` | `Pipelines::request_fxaa` |
| The shader | `fxaa.slang` | `computeMain`, `luma`, `EDGE_THRESHOLD_MIN`, `EDGE_THRESHOLD_MAX` |

> [!NOTE]
> FXAA treats luma contrast as "an edge" and can't tell a real geometry edge from a sharp
> texture or a noisy specular highlight, so it softens all three. That is the trade against
> MSAA, which fixes geometry edges exactly but sees nothing inside a triangle.

## Related

- [AA modes](../aa-modes/) — the full mode table and how the three are switched
- [MSAA](../msaa/) — the rasterization-time alternative
- [TAA](../../screen-space-and-post/taa/) — the temporal alternative, sharing the scratch
- [Compute post-process pattern](../../screen-space-and-post/compute-post-process-pattern/) — the shared dispatch shape
