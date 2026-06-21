+++
title = 'Punctual lights'
weight = 4
math = true
+++

# Punctual lights

A punctual light is a light source at a single world-space position: a point light radiating in
all directions, or a spot light confined to a cone. Both fall off with distance, and a spot also
cuts off by angle.

Each punctual light feeds the same [Cook-Torrance BRDF](../cook-torrance-brdf/) as the
[directional sun](../directional-light/). The difference is the incoming radiance, which a punctual
light builds from three multiplicative factors: distance attenuation, a spot cone, and shadow.

## One function per light

`punctual` evaluates a single light at a world-space surface point. It computes the light
direction, the attenuation, the cone, and the shadow, folds them into `radiance`, and passes that
to `brdf`:

```hlsl
float3 toLight = lt.positionRange.xyz - worldPos;
float dist = length(toLight);
if (dist > lt.positionRange.w)        // past range: contributes nothing
    return float3(0.0);
float3 l = toLight / max(dist, 0.0001);
float attenuation = distanceAttenuation(dist, lt.positionRange.w);
// ... cone, shadow ...
float3 radiance = lt.colorIntensity.rgb * lt.colorIntensity.a * attenuation * cone * shadow;
return brdf(n, v, l, albedo, metallic, roughness, radiance);
```

The early `dist > range` test rejects a light cheaply before any BRDF work. Everything after it
builds the `radiance` scalar the BRDF multiplies in.

## Distance attenuation

A physical point source falls off as $1/d^2$. A pure inverse-square never reaches zero, so it
touches every fragment in the scene and defeats culling. The engine uses the UE4-style windowed
inverse-square: the physical $1/d^2$ multiplied by a smooth factor that reaches exactly zero at
`range`.

$$
\text{att}(d, r) = \frac{1}{\max(d^2,\ 10^{-4})}\,\left[\operatorname{sat}\!\left(1 - \left(\tfrac{d}{r}\right)^4\right)\right]^2
$$

```hlsl
float distanceAttenuation(float dist, float range)
{
    float invSquare = 1.0 / max(dist * dist, 0.0001);
    float t = saturate(1.0 - pow(dist / range, 4.0));
    return invSquare * t * t;
}
```

The $\max(d^2, 10^{-4})$ floor keeps the value finite at the light's own position. The
$\bigl(1-(d/r)^4\bigr)^2$ window is $1$ near the light and tapers smoothly to $0$ at $d = r$, giving
the light compact support. That bounded support lets the [cluster cull](../clustered-forward/) treat
`range` as a hard bounding radius.

## The spot cone

A spot adds an angular cutoff on top of the distance falloff. The light's `directionType.w` flags
it as a spot. The shader compares the angle between the spot's aim and the direction to the
fragment against two pre-computed cosines, smoothstepped for a soft penumbra:

```hlsl
if (lt.directionType.w > 0.5)
{
    float3 spotDir = normalize(lt.directionType.xyz);
    float cosAngle = dot(spotDir, -l);
    cone = smoothstep(lt.spotCos.y, lt.spotCos.x, cosAngle);   // y = outer, x = inner
}
```

`smoothstep(outer, inner, cosAngle)` is $1$ inside the inner half-angle, $0$ outside the outer one,
and a smooth Hermite ramp between. Larger angles have smaller cosines, so the outer cosine is the
low edge and the inner is the high edge. A point light leaves `cone` at `1.0`.

## Shadow

The `shadow` factor is `1.0` unless this light is the one shadowed spot or point light, or RT
shadows are on:

- the shadowed **spot** projects through `spotShadowViewProj` and PCF-samples `spotShadowMap`;
- the shadowed **point** samples an omnidirectional cube of world distance-to-light
  (`pointShadow`), comparing the fragment's distance against the stored nearest occluder;
- with RT shadows enabled, every punctual light traces one ray toward the light instead.

In v1 the map paths shadow exactly one spot and one point light; the RT path shadows all of them.

## In the code

| What | File | Symbols |
|---|---|---|
| One light's contribution | `engine/assets/shaders/lighting.slang` | `punctual` |
| Distance falloff | `engine/assets/shaders/lighting.slang` | `distanceAttenuation` |
| Spot cone | `engine/assets/shaders/lighting.slang` | `punctual` — `smoothstep`, `spotCos` |
| Spot / point shadow | `engine/assets/shaders/lighting.slang` | `pcfShadow`, `pointShadow` |
| Cosines packed on the CPU | `engine/crates/assets/src/render_scene.rs` | `gather_punctual_lights` — `inner_angle.to_radians().cos()` into `spot_cos` |

> [!TIP]
> `range` is a hard cutoff, not just a falloff knob. The window forces attenuation to zero at
> `range`, which is what makes `range` usable as the light's bounding radius in the cluster cull.
> Too small and the visible falloff clips abruptly; too large and the light lands in more
> clusters than it needs.

## Related

- [Cook-Torrance BRDF](../cook-torrance-brdf/) — what `radiance` is multiplied into
- [Light components](../light-components/) — where `range`, `intensity`, and the cone angles come from
- [Clustered forward](../clustered-forward/) — how `range` becomes a culling radius
- [Directional light](../directional-light/) — the same BRDF with no falloff or cone
