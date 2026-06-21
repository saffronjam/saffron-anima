+++
title = 'Procedural sky'
weight = 3
math = true
+++

# Procedural sky

A procedural sky is an environment generated analytically from a direction function rather than loaded from an image. It produces a horizon-to-zenith gradient over a dim ground plane, plus a bright sun disk, written into the environment cube as HDR radiance.

Image-based lighting needs an environment to convolve. The procedural sky supplies one without an asset: the cube is filled in a compute pass, and the irradiance and prefilter passes then treat it as the light source. It is the default of the three [environment sources](../procedural-atmosphere/) (`EnvSource::Procedural`).

## What the sky function returns

`proceduralSky(dir)` takes a world direction and returns linear HDR radiance in three parts.

**The gradient.** Above the horizon ($\text{dir}.y \ge 0$) it blends horizon color into zenith color by a softened upward factor; below, it blends toward a dark ground color. The `pow(up, 0.6)` curve pushes the blend so most of the visible dome reads as sky rather than horizon, and a scale lifts the whole into a brighter exposure range. The ground side ramps quickly to a near-flat dark, a floor for downward-facing reflections.

**The sun disk.** The sun direction (from the bake push) drives two power lobes added on top:

```hlsl
float s = max(dot(normalize(dir), sunDir), 0.0);
col += pow(s, 1200.0) * float3(22.0, 20.0, 17.0);   // tight bright core
col += pow(s, 6.0)    * float3(0.30, 0.26, 0.20);    // soft surrounding glow
```

The $\cos^{1200}$ lobe is an extremely tight, very bright disk — values above 20, genuine HDR that the prefilter smears into bright specular reflections. The $\cos^6$ lobe is a wide, dim halo. The sun direction comes from `SkygenPush.sunDir` (= −lightDir), so IBL and the sun agree.

## Writing it into the cube

The compute shader runs one invocation per output texel, with `tid.z` selecting the cube face. It reconstructs the face direction, evaluates the sky, and stores the result:

```hlsl
float2 uv  = (float2(tid.xy) + 0.5) / float2(width, height) * 2.0 - 1.0;
float3 dir = cubeFaceDir(tid.z, uv);
outCube[tid] = float4(proceduralSky(dir), 1.0);
```

`cubeFaceDir` is the hardware cube convention shared by every IBL shader: a face index 0..5 and an in-face uv in $[-1, 1]$ map to the world direction that face represents. The `+ 0.5` keeps sampling aligned to texel centers. The result is bound as an HDR `RWTexture2DArray` (`outCube`).

## Why analytic, not a loaded HDR

An analytic sky has no asset dependency, costs nothing to ship, and is deterministic across platforms: the bake produces the same environment every run. It serves as the default source. The downstream pipeline does not depend on where the environment came from, so swapping in a loaded HDR equirect (`EnvSource::Equirect`, `ibl_equirect.slang`) or the [physically based atmosphere](../procedural-atmosphere/) only changes what fills the environment cube — the convolutions, the BRDF lookup, and the visible-sky pass are unchanged.

## In the code

| What | File | Symbols |
|---|---|---|
| Sky model + face direction | `engine/assets/shaders/ibl_skygen.slang` | `proceduralSky`, `sunDir`, `cubeFaceDir` |
| The write | `engine/assets/shaders/ibl_skygen.slang` | `computeMain`, `outCube` |
| Bake inputs + push | `engine/crates/rendering/src/ibl.rs` | `SkygenParams`, `SkygenPush`, `EnvSource::Procedural` |
| Dispatched once | `engine/crates/rendering/src/ibl.rs` | `Ibl::bake` — skygen dispatch over `IBL_ENV_SIZE` |

## Related

- [Procedural atmosphere](../procedural-atmosphere/) — the physically based source that replaces this gradient
- [Diffuse irradiance](../diffuse-irradiance/) — convolves this environment for diffuse
- [Specular prefilter](../specular-prefilter/) — blurs this environment per roughness
- [Baking](../ibl-bake-pass/) — runs skygen first, then the convolutions
