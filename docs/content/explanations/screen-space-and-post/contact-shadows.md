+++
title = 'Contact shadows'
weight = 3
math = true
+++

# Contact shadows

A contact shadow is a short-range shadow computed by marching a screen-space ray toward the light and
testing it against the depth buffer. It resolves the fine occlusion where one surface nearly touches
another, the thin gap a shadow map cannot resolve at its working resolution.

The technique supplements the directional shadow map rather than replacing it. The map handles the
coarse term — large geometry casting onto the ground — while the contact pass darkens the small
contacts the map misses. It reads the [thin G-buffer](../thin-gbuffer/), costs little, and affects
only the directional direct term.

## How it works

Each pixel reconstructs its view-space position $p$ and normal $n$, offsets along the normal to avoid
self-occlusion, and steps along the view-space light direction $l$:

$$
s_i = p + n\,\epsilon \;+\; l \cdot \text{rayLen} \cdot \frac{i}{\text{steps}}, \qquad i = 1 \dots \text{steps}
$$

At each step the pass projects the marched view position back to the screen, samples the stored depth
there, and compares. The stored surface lies between the pixel and the light when it is nearer than the
ray sample, but only when the gap falls inside a thickness window.

The thickness check is the crux of the method. View-space depth holds one value per pixel, so the
G-buffer cannot distinguish a thin object from an infinitely deep one. The window treats each stored
surface as a solid slab of a fixed thickness, and a hit counts only when the ray dips just behind it.
Without the window, every surface in front of the ray would shadow everything behind it. The march
stops early when a sample falls off-screen or behind the near plane, and skips background samples
(`surfZ > -1e-4`). The output is `r8` occlusion where 1 means lit.

The push constant supplies the projection (to project marched positions to screen), its inverse (to
reconstruct view positions), the light direction in view space, and the ray length, step count, and
thickness packed into a `params` vector.

### Combining with the shadow map

In the mesh fragment shader the directional shadow starts from the map (PCF) or a ray query, then the
contact factor multiplies in:

```hlsl
if (globals.screenFlags.x != 0)
{
    shadow *= contactMap.SampleLevel(screenUv, 0.0).r;
}
```

Multiplying means the two factors only darken, never brighten: a pixel the map already shadowed stays
shadowed, and a lit pixel can pick up a fine contact occlusion the map missed. The effect applies to
the directional light only, gated by `screenFlags.x`.

## In the code

| What | File | Symbols |
|---|---|---|
| The march | `contact.slang` | `computeMain`, `viewPosFromUv`, `diff`, `thickness` |
| Pass wiring + params | `renderer.cppm` | `contact-shadows` pass, `sunDirView`, the `params` push |
| Where it's applied | `mesh.slang` | `contactMap`, `screenFlags.x` |

> [!NOTE]
> The light direction is supplied in view space, not world space, because the whole march happens in
> view space against the G-buffer's view-space depth. The renderer transforms the sun direction once on
> the CPU (`sunDirView`) rather than the shader doing it per pixel.

## Related

- [G-buffer](../thin-gbuffer/) — the view-space depth it marches against
- [Directional shadows](../../shadows-and-culling/directional-shadows/) — the coarse term contact shadows refine
- [Cook-Torrance BRDF](../../lighting-and-brdf/cook-torrance-brdf/) — the direct term the shadow multiplies
