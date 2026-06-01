# Phase 4: Atmosphere And IBL Roadmap

**Status:** ROADMAP (genuinely future; not implemented with phases 1–3)

## Goal

Document the longer-term path beyond a visible skybox: HDR support, environment maps, reflection probes, physically based atmosphere, and clouds.

This phase should not block the first usable skybox.

> **Post-lighting note — most IBL infrastructure already exists.** The lighting roadmap built
> the cubemap/IBL stack this section once scoped as future work. **Already done:** `GpuCubemap`-
> equivalent cube `Image` creation (`newCubeImage`, cube-compatible + mip + `eCube` view),
> diffuse irradiance convolution (`ibl_irradiance.slang`), prefiltered specular mip chain
> (`ibl_prefilter.slang`), BRDF integration LUT (`ibl_brdf.slang`), and PBR roughness/metalness
> materials sampling them (`mesh.slang` set 3). The "Diffuse Irradiance", "Specular Reflections",
> and most of "Cubemap Conversion" items below are therefore **DONE** — for the *procedural*
> source. What remains genuinely future:
> - **HDR `.hdr` import** — decode is sRGB RGBA8 only today (stb_image, `geometry.cppm:568-603`);
>   needs `stb_image`'s `stbi_loadf` float path + `uploadTextureFloat` + a float bindless slot.
> - **User equirect→cubemap + IBL re-bake from a user panorama** — feed a loaded HDR equirect
>   into `bakeEnvironment` (an equirect→cube prepass) instead of the procedural skygen, then
>   re-run the existing irradiance/prefilter/BRDF passes. (Phase 3 already adds on-demand
>   re-bake for the procedural params; this extends the *source*.)
> - **Reflection probes, procedural atmosphere LUTs, clouds, time-of-day** — all still future,
>   as below.

## HDR Texture Support

True HDR skies need float textures.

Work items:

- Add `.hdr` decode through `stb_image` float loading.
- Consider `.exr` later if dependency cost is acceptable.
- Add `uploadTextureFloat` or generalized upload path.
- Use `vk::Format::eR16G16B16A16Sfloat` or `eR32G32B32A32Sfloat`.
- Add asset metadata for color space and texture usage.
- Avoid treating HDR sky textures as sRGB RGBA8.

## Cubemap Conversion

Most renderer IBL paths want cubemaps. Artist-friendly sources are often equirectangular panoramas.

Work items:

- Add compute shader: equirectangular 2D texture to cubemap.
- Add `GpuCubemap` resource type.
- Add cubemap image creation with 6 layers and cube-compatible flag.
- Add image view type `eCube`.
- Generate mipmaps.

## Diffuse Irradiance

Two viable approaches:

1. Spherical harmonics:
   - Compact.
   - Good for diffuse lighting.
   - Easy to upload in light/environment UBO.

2. Irradiance cubemap:
   - Straightforward shader sampling.
   - Larger resource.
   - More natural if cubemap pipeline already exists.

Recommendation: start with SH because the current material model is simple diffuse lighting.

## Specular Reflections

Needs material roughness/metalness first.

Work items:

- Add material properties: roughness, metallic, normal texture eventually.
- Add prefiltered environment cubemap.
- Add BRDF integration LUT.
- Extend mesh shader to PBR BRDF.
- Add reflection intensity in `SceneEnvironment`.

## Reflection Probes

Reflection probes are entities/components because they are spatial.

Future components:

```cpp
struct ReflectionProbeComponent
{
    f32 influenceRadius = 10.0f;
    f32 intensity = 1.0f;
    bool boxProjection = false;
};
```

Probe rendering should be separate from global sky. The global sky becomes the fallback probe.

## Procedural Atmosphere

Unreal-style and Frostbite-style atmosphere should be treated as environment rendering, not a mesh skybox.

Potential data:

```cpp
struct AtmosphereSettings
{
    bool enabled = false;
    f32 planetRadius = 6360.0f;
    f32 atmosphereHeight = 100.0f;
    glm::vec3 rayleighScattering;
    f32 rayleighScaleHeight;
    glm::vec3 mieScattering;
    f32 mieScaleHeight;
    f32 mieAnisotropy;
    glm::vec3 absorption;
    f32 absorptionScale;
    bool sunDisk = true;
};
```

Rendering approach:

- Start with fullscreen sky atmosphere shader.
- Use directional light as sun direction.
- Later add LUTs:
  - transmittance LUT,
  - multi-scattering LUT,
  - sky-view LUT,
  - aerial perspective LUT.

## Clouds

Clouds should be a separate system from skybox.

Potential path:

- 2D procedural cloud layer first.
- Volumetric cloud raymarch later.
- Cloud lighting should use sun direction and atmosphere transmittance.

## Time Of Day

Time-of-day should drive:

- directional light rotation/intensity/color,
- sky atmosphere parameters,
- exposure,
- cloud lighting,
- fog/aerial perspective.

This should be a scene system or editor tool, not baked into the renderer.

## Suggested Long-Term Order

1. Visible skybox from LDR equirectangular texture.
2. RGB ambient.
3. HDR texture import.
4. Equirectangular-to-cubemap conversion.
5. Diffuse SH irradiance.
6. PBR material model.
7. Specular IBL.
8. Reflection probes.
9. Procedural atmosphere.
10. Clouds and time of day.

