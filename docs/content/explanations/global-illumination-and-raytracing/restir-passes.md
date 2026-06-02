+++
title = 'ReSTIR passes'
weight = 10
math = true
+++

# ReSTIR passes

[ReSTIR](../restir-overview/) runs as three compute passes over per-pixel reservoir buffers: initial
candidate sampling, spatiotemporal reuse, and resolve-and-shade. Each pass dispatches one thread per
pixel in 8×8 groups, and the three are wired together through a small set of structured buffers.

The reservoirs carry sampling state from one pass to the next and from one frame to the next. The
temporal link is what makes the estimator converge, and also what introduces the bias that
M-clamping controls.

> [!NOTE]
> ReSTIR is feature-gated on ray-query support and runs at ~1 FPS on the software dev GPU —
> correctness-validated, awaiting hardware.

## The reservoir buffers

Three SSBOs each hold one `Reservoir` (32 bytes) per pixel, sized to the offscreen resolution
(`reservoirCapacity`):

- `initial` — this frame's RIS result (written by pass 1, read by pass 2)
- `combined` — after reuse (written by pass 2, read by pass 3)
- `previous` — last frame's combined, the temporal source (pass 3 writes it for next frame)

Consecutive passes serialize through a sentinel buffer access in the
[render graph](../../frame-and-render-graph/render-graph-overview/). Pass 1 declares
`StorageWriteCompute` on `combined`, passes 2 and 3 declare `StorageReadCompute`, so the graph
derives the write→read barriers between them.

## Pass 1 — initial (RIS)

`restir_initial.slang` reconstructs the world surface from the G-buffer (view normal and view-Z),
finds the pixel's froxel cluster, and draws $K$ candidate lights from that cluster's light list. The
candidate set is the [clustered](../../lighting-and-brdf/clustered-forward/) light pool, and $K = 16$
by default (`candidateCount`). Each candidate is weighted by its unshadowed target contribution
$\hat p$ and kept by weighted reservoir sampling.

`targetContribution` is the scalar luminance of the light's diffuse contribution: intensity ×
$n\cdot l$ × distance attenuation × spot cone, with no shadow term. The pass then computes the
unbiased weight $W = \tfrac{1}{K}\,\text{wSum} / \hat p_\text{chosen}$ and writes the reservoir.
Background pixels with no surface write an empty reservoir and bail.

## Pass 2 — reuse (temporal + spatial)

`restir_reuse.slang` starts the combined reservoir from this pixel's initial one, then merges in more
samples via `combineInto`. The merge is WRS over reservoirs: each incoming reservoir is reweighted by
*its chosen light's* target function at this pixel and contributes $\hat p \cdot W \cdot M$ to the
running sum.

The temporal sample is the previous frame's reservoir, fetched by reprojecting the pixel through the
motion vector (`uv + mv`) and merged when the reprojected UV lands on-screen. The spatial samples are
four random neighbours within a 16-pixel radius, each merged only when its depth and normal are
similar (`abs(Δdepth) < 0.5` and normal dot > 0.9). The similarity test keeps reuse to surfaces that
share lighting.

The final unbiased weight is recomputed from the merged state:

$$
W = \frac{\text{wSum}}{M \cdot \hat p_\text{chosen}}
$$

## M-clamping

The temporal source is last frame's combined reservoir, which itself absorbed the frame before it.
Left unbounded, $M$ grows without limit and the estimator becomes badly biased: stale samples
dominate and lighting lags. The history's $M$ is therefore clamped before merging (`maxM = 20`).

Clamping caps how much weight any past frame carries, trading a little variance for a bounded bias and
keeping the lighting responsive to change. It is the standard ReSTIR bias control.

## Pass 3 — resolve and shade

`restir_resolve.slang` reads the combined reservoir and performs the work deferred from pass 1. It
traces *one* ray-query shadow ray toward the chosen light — the only visibility ray ReSTIR needs — and
if lit, shades the light's contribution scaled by the reservoir weight $W$:

```hlsl
float vis = rayShadow(worldPos, l, dist);   // single ACCEPT_FIRST_HIT ray
if (vis <= 0.0) { radianceOut[tid.xy] = 0; return; }
float3 radiance = lt.colorIntensity.rgb * lt.colorIntensity.a
                * atten * cone * ndotl * res.a.y * vis;   // res.a.y = W
```

The output is geometry × visibility × $W$, *without* the surface albedo. The mesh fragment multiplies
by `albedo / PI` when it samples the radiance, so the material stays with the material. The pass also
copies the combined reservoir into `previous` for next frame's temporal reuse, closing the loop.

## In the code

| What | File | Symbols |
|---|---|---|
| Candidate sampling + RIS | `restir_initial.slang` | `computeMain`, `targetContribution`, `clusterIndexFor` |
| Reservoir merge | `restir_reuse.slang` | `combineInto`, the temporal + spatial blocks |
| M-clamping | `restir_reuse.slang` | `prevM = min(prev.a.w, maxM)`, `nbM` |
| Resolve ray + shade | `restir_resolve.slang` | `computeMain`, `rayShadow` |
| The three graph passes | `renderer.cppm` | `restir-initial`, `restir-reuse`, `restir-resolve` |
| Reservoir SSBOs | `renderer_types.cppm` | `Restir::initial`, `combined`, `previous` |

> [!WARNING]
> The three passes serialize through a single *sentinel* buffer access (`combined`) rather than
> declaring each reservoir buffer to the graph. That's enough to force the RAW barriers between
> consecutive passes, but the graph doesn't track the individual reservoir buffers — the ping-pong of
> `previous` is managed by hand in the resolve pass, not derived.

## Related

- [ReSTIR](../restir-overview/) — the reservoir + RIS theory these implement
- [Clustered forward](../../lighting-and-brdf/clustered-forward/) — where the candidate light pool comes from
- [Ray-query shadows](../ray-query-shadows/) — the `rayShadow` used in resolve
- [Motion vectors](../../screen-space-and-post/) — the temporal reprojection input for reuse
