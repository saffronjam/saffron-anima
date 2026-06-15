+++
title = 'SSGI'
weight = 4
math = true
+++

# SSGI

Screen-space global illumination (SSGI) approximates one bounce of indirect diffuse light using only
the data already on screen. Each pixel fires a few short rays into the hemisphere above it, and where
a ray hits nearby geometry it gathers that surface's lit color from the previous frame as incoming
indirect radiance.

Indirect light is the second bounce: direct light strikes a red wall and tints the white floor beside
it. A forward renderer computes only direct lighting, so this bounce must be approximated separately.
The gathered radiance adds to the ambient term.

## How it works

Each pixel reconstructs its view-space position $p$ and normal $n$ from the
[thin G-buffer](../thin-gbuffer/), then builds a tangent basis $(t, b, n)$ to orient a cosine-weighted
hemisphere. It fires four rays, each a cosine sample so directions near the normal — which contribute
most to diffuse — are favored:

$$
\text{local} = \big(\sqrt{u_1}\cos\varphi,\ \sqrt{u_1}\sin\varphi,\ \sqrt{1 - u_1}\big), \qquad \varphi = 2\pi\,u_2
$$

The sample coordinates $(u_1, u_2)$ are seeded per pixel by interleaved gradient noise — a
low-discrepancy, blue-noise-like pattern — rotated each frame by the golden ratio, then stepped by the
golden angle across the four rays. Blue-noise error survives denoising; a white-noise hash would leave a
low-frequency residual the blur and temporal accumulation cannot remove (visible as crawling grain).

Each ray marches in view space, projecting to the screen and reading the stored depth at every step. A
hit registers the first time the ray dips just behind the stored surface, inside a thickness window
like the [contact shadow](../contact-shadows/) march. On a hit the ray gathers `prevColor`, the
previous frame's resolved linear-HDR color:

```hlsl
float diff = surfZ - sp.z;
if (diff > 0.02 && diff < radius * 0.5)
{
    indirect += prevColor.SampleLevel(suv, 0.0).rgb;
    break;
}
```

Reading last frame's image makes a screen-space bounce affordable: the hit surface's full lighting
(direct + ambient) is already computed and sitting in a texture. The cost is a one-frame lag and a
dependence on whatever was on screen last frame. The four rays are averaged, scaled by an intensity
knob, and stored in an `rgba16f` map.

### Denoising

Four rays per pixel is far too few to converge a diffuse integral, so the raw map is noisy. Two passes
clean it up, and both run whenever SSGI is enabled — independent of the final-image AA mode:

- A **depth-aware spatial blur** (`ssgi-blur`, a 5×5 bilateral weighted by view-Z) smooths within the
  frame without bleeding indirect light across depth edges.
- A **temporal accumulation** (`ssgi-accum`) reprojects the previous frame's resolved SSGI through the
  [motion vectors](../motion-vectors/), neighborhood-clamps the history to reject ghosting, and blends
  with an exponential moving average. This raises the effective sample count over many frames, so a
  matte surface converges to a smooth bounce instead of showing each frame's four sparse ray hits as
  drifting streaks.

SSGI owns this accumulation: its own ping-pong history pair and motion-vector dependency, so it
converges in every AA mode — not only when [TAA](../taa/) is the display anti-aliasing. The mesh then
samples this resolved, temporally stable map.

### Where the radiance lands

The mesh fragment shader treats the gathered radiance as extra incoming light on the diffuse albedo,
added into the ambient term and modulated by AO so occluded creases don't over-bounce:

```hlsl
if (globals.screenFlags.y != 0)
{
    float  ao = globals.counts.w != 0 ? aoMap.SampleLevel(screenUv, 0.0).r : 1.0;
    float3 gi = ssgiMap.SampleLevel(screenUv, 0.0).rgb;
    ambient += gi * albedo * (1.0 - metallic) * ao;
}
```

It adds to the indirect term, never the direct lights, and only for non-metals, since metals have no
diffuse response. The whole contribution is gated by `screenFlags.y`.

### Feeding the next frame

SSGI reads last frame's color, so each frame must save it before the in-place tonemap turns it
display-referred. A `ssgi-history` compute pass copies the scene's resolved linear-HDR color into
`prevColor` right after the scene pass, then a barrier-only pass restores `prevColor` to its resting
`ShaderReadOnly` layout for next frame. The renderer imports the `prevColor` handle once and tracks its
layout across both the read (this frame's gather) and the write (this frame's capture).

```mermaid
flowchart LR
    A[frame N scene] --> B[ssgi-history copy<br/>scene → prevColor]
    B --> C[prevColor rests<br/>ShaderReadOnly]
    C --> D[frame N+1 SSGI<br/>gathers prevColor]
    D --> E[frame N+1 scene<br/>adds gi to ambient]
```

## In the code

| What | File | Symbols |
|---|---|---|
| The gather | `ssgi.slang` | `computeMain`, cosine-hemisphere sampling, interleaved gradient noise |
| Denoise + accumulate | `ssgi_blur.slang`, `ssgi_accum.slang` | bilateral blur, motion-reprojected EMA + neighborhood clamp |
| Passes + prev-color import | `renderer.cppm` | `ssgi` / `ssgi-blur` / `ssgi-accum` passes, `ssgi-history` copy |
| Where GI is added | `mesh.slang` | `ssgiMap`, `screenFlags.y` |

> [!NOTE]
> SSGI sees only what is on screen. A bounce off a surface that is off-screen or hidden behind nearer
> geometry does not happen, because the gather can read only pixels the previous frame stored. This is
> the defining limit of any screen-space method, and the reason the lighting roadmap moves on to
> world-space [DDGI](../../global-illumination-and-raytracing/) for off-screen bounce.

## Related

- [G-buffer](../thin-gbuffer/) — the geometry the rays march against
- [Contact shadows](../contact-shadows/) — the same view-space march, different gather
- [GTAO](../gtao/) — the AO that modulates the bounce
- [Tonemapping](../tonemap-and-exposure/) — runs after the linear color is captured for history
