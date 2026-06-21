+++
title = 'Cook-Torrance BRDF'
weight = 2
math = true
+++

# Cook-Torrance BRDF

The Cook-Torrance BRDF is a metallic-roughness shading model: a microfacet specular lobe
plus a Lambertian diffuse term, energy-conserving and evaluated in linear HDR. A BRDF
(bidirectional reflectance distribution function) gives the fraction of incoming light from
one direction that a surface reflects toward another.

Every direct light in the engine runs through the same model — the directional sun and each
punctual point or spot light. The reflected radiance toward the eye from one light is

$$
L_o = \big(\,f_\text{diffuse} + f_\text{specular}\,\big)\; L_i \;(n \cdot l)
$$

where $L_i$ is the light's incoming radiance (its color times intensity, times attenuation,
cone, and shadow for punctual lights) and $n\cdot l$ is the cosine falloff. Both terms are
computed in `brdf`; the caller supplies only $L_i$.

## The half vector and the angles

Given the surface normal $n$, the view direction $v$, and the light direction $l$, the
shader forms the half vector $h = \widehat{v + l}$ and four clamped cosines:

$$
n\cdot l,\quad n\cdot v,\quad n\cdot h,\quad h\cdot v
$$

`ndotv` is clamped to a small positive epsilon rather than zero, keeping the grazing-angle
terms finite.

## Specular: $D\,V\,F$

The specular lobe is the microfacet product

$$
f_\text{specular} = D \cdot V \cdot F
$$

evaluated with roughness remapped to $\alpha = \text{roughness}^2$ (the perceptual-to-physical
remap; in code, `a = roughness * roughness`).

**Normal distribution $D$** (Trowbridge-Reitz / GGX) counts how many microfacets point along $h$:

$$
D(h) = \frac{\alpha^2}{\pi\big((n\cdot h)^2(\alpha^2 - 1) + 1\big)^2}
$$

**Visibility $V$** (height-correlated Smith) accounts for the masking and shadowing of those
microfacets, with the $\tfrac{1}{4(n\cdot v)(n\cdot l)}$ denominator of the Cook-Torrance term
folded in, so no separate division is needed:

$$
V = \frac{0.5}{\,(n\cdot l)\sqrt{(n\cdot v)^2(1-\alpha^2)+\alpha^2}\;+\;(n\cdot v)\sqrt{(n\cdot l)^2(1-\alpha^2)+\alpha^2}\,}
$$

**Fresnel $F$** (Schlick) describes how reflectance rises toward grazing angles:

$$
F(h,v) = F_0 + (1 - F_0)\,(1 - h\cdot v)^5
$$

$F_0$ is the reflectance at normal incidence. The metallic workflow picks it by interpolation:
dielectrics take a flat $0.04$, metals take their base color, blended by the metallic value.

$$
F_0 = \operatorname{lerp}(0.04,\; \text{albedo},\; \text{metallic})
$$

## Diffuse: Lambertian, energy-balanced against specular

The diffuse term is Lambertian, scaled so it never adds energy the specular term already
reflected, and zeroed out for metals (which have no diffuse response):

$$
f_\text{diffuse} = k_d \, \frac{\text{albedo}}{\pi}, \qquad
k_d = (1 - F)\,(1 - \text{metallic})
$$

The $(1 - F)$ factor couples the two lobes: light reflected by the Fresnel term is removed
from what remains available to scatter diffusely.

## Putting it together

The body of `brdf` in `lighting.slang` is exactly these terms:

```hlsl
float3 h    = normalize(v + l);
float a     = roughness * roughness;          // α
float3 F0   = lerp(float3(0.04), albedo, metallic);
float3 F    = fresnelSchlick(max(dot(h, v), 0.0), F0);
float  D    = distributionGGX(ndoth, a);
float  Vis  = visibilitySmithGGX(ndotv, ndotl, a);
float3 spec = D * Vis * F;
float3 kd   = (1.0 - F) * (1.0 - metallic);
float3 diff = kd * albedo / PI;
return (diff + spec) * radiance * ndotl;       // radiance == L_i
```

The fragment shader calls this once for the directional light and once per punctual light,
summing the results into the outgoing radiance before adding the ambient/IBL term. The
[clustered](../clustered-forward/) and brute-force light loops share the same function, so the
two paths are pixel-identical by construction.

## Two clamps worth knowing

> [!TIP]
> Roughness is clamped to `[0.045, 1.0]` in the fragment shader before it reaches the BRDF.
> A roughness of exactly zero makes $\alpha^2 \to 0$ and the GGX denominator collapses into a
> specular singularity (an infinitely sharp, infinitely bright highlight). The floor keeps the
> highlight tight but finite. Metallic is saturated to `[0, 1]`.

## In the code

| What | File | Symbols |
|---|---|---|
| Combined BRDF | `engine/assets/shaders/lighting.slang` | `brdf` |
| Normal distribution | `engine/assets/shaders/lighting.slang` | `distributionGGX` |
| Visibility | `engine/assets/shaders/lighting.slang` | `visibilitySmithGGX` |
| Fresnel | `engine/assets/shaders/lighting.slang` | `fresnelSchlick`, `fresnelSchlickRoughness` |
| Where it's called | `engine/assets/shaders/lighting.slang` | `evalLighting`, `punctual` |
| Fragment entry | `engine/assets/shaders/mesh.slang` | `fragmentMain` |

## Related

- [Punctual lights and attenuation](../punctual-lights-and-attenuation/) — what fills in $L_i$
- [Clustered forward](../clustered-forward/) — which lights the fragment evaluates
- [HDR and exposure](../hdr-and-exposure/) — where this linear radiance ends up
- [Image-based lighting](../../image-based-lighting/) — the ambient term added on top
