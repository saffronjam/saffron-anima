+++
title = 'G-buffer'
weight = 1
+++

# G-buffer

A G-buffer is a screen-resolution image that stores surface attributes per pixel — the geometry a
shading or sampling pass reads back instead of recomputing it. A *thin* G-buffer stores only the few
attributes its consumers actually need.

Screen-space effects must know, per pixel, what surface is present and how it faces the camera. In a
forward-shaded renderer there is no fat G-buffer to read, so a small prepass writes just enough
geometry: the view-space normal and view-space depth, packed into one `rgba16f` target. That single
image feeds GTAO, contact shadows, and SSGI.

## How it works

The prepass renders the instanced scene the same way the depth pre-pass does, but its fragment shader
writes the geometry buffer rather than only laying down depth. The push constant carries `viewProj`
(world → clip, for `SV_Position`) and `view` (world → view). The vertex stage transforms the normal
into view space and passes view-space Z down; the fragment stage writes one `float4`: normalized view
normal in `rgb`, view-space Z in `a`.

Everything downstream lives in view space, the natural frame for screen-space marching. Normals are
oriented relative to the camera, and Z is a linear distance consumers can compare and reconstruct
positions from. The target clears to zero, so a pixel with no geometry reads `viewZ == 0`. The camera
looks down −Z, so real surfaces store a negative Z, and consumers treat `viewZ > -1e-4` as background.

View-Z is the half of the buffer that lets a consumer rebuild the full view-space position of any
pixel. Given a UV and its stored Z, fire a ray through the pixel in clip space, divide by `w`, and
scale to the stored depth:

```hlsl
float3 viewPosFromUv(float2 uv, float viewZ)
{
    float2 ndc = uv * 2.0 - 1.0;
    float4 r   = mul(invProjection, float4(ndc, 1.0, 1.0));
    float3 ray = r.xyz / r.w;
    return ray * (viewZ / ray.z);
}
```

That helper is copied into `gtao.slang`, `contact.slang`, and `ssgi.slang` — the shared key that turns
the thin buffer back into positions. The prepass also writes a real depth attachment (`gDepth`), so
it is depth-tested like any geometry pass.

### Why thin, and why one target

A full deferred G-buffer stores albedo, metallic-roughness, world position, and motion across several
attachments. These effects need only orientation and distance, so normal + Z in one `rgba16f` is the
whole bill. The MRT machinery in the render graph would let it grow, but targets nothing reads cost
memory and bandwidth for no benefit. The prepass runs only when at least one screen-space effect is
enabled — the renderer gates it on `doScreen` (GTAO, contact, SSGI, or ReSTIR).

## In the code

| What | File | Symbols |
|---|---|---|
| Prepass shader | `gbuffer.slang` | `vertexMain`, `fragmentMain` |
| Position reconstruction | `gtao.slang`, `contact.slang`, `ssgi.slang` | `viewPosFromUv` |
| Pass declaration + gating | `renderer.cppm` | `gbuffer` pass, `doScreen`, `recordGbuffer` |
| Where it's sampled | `mesh.slang` | `aoMap`, `contactMap`, `ssgiMap` (set 4) |

> [!NOTE]
> The background test is a sign test on view-Z (`viewZ > -1e-4`), not a comparison against a far-plane
> constant. It works because the color target clears to `0` and real geometry is always at negative
> view-space Z. Change the clear value or the projection handedness and every consumer's background
> check has to change too.

## Related

- [GTAO](../gtao/) — its first consumer
- [Contact shadows](../contact-shadows/) — marches against the stored Z
- [SSGI](../ssgi/) — gathers along view-space rays
- [Passes and attachments](../../frame-and-render-graph/passes-and-attachments/) — the MRT machinery this could grow into
