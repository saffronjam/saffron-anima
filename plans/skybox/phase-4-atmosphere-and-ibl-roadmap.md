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
> source. The two HDR items this note once listed as future have since shipped as their own
> phases:
> - **HDR `.hdr` import** — DONE, see `phase-4-hdr-textures.md` (COMPLETED): `decodeImageHdr`
>   (`stbi_loadf`), `uploadTextureFloat` (rgba16f into the shared bindless array), and the
>   catalog `AssetEntry.hdr` flag. The "HDR Texture Support" section below is satisfied except
>   `.exr` (still deferred on dependency cost) and richer usage metadata (only the `hdr` bool
>   exists; the `TextureUsage` design is sketched in that section below).
> - **User equirect→cubemap + IBL re-bake from a user panorama** — DONE, see
>   `phase-5-equirect-ibl-and-ddgi-sky.md` (COMPLETED): `ibl_equirect.slang` projects the
>   panorama into the env cube via `EnvSource::Equirect` / `requestEnvBake`, then the existing
>   irradiance/prefilter/BRDF chain runs; sky color routed into DDGI.
>
> What remains genuinely future: **reflection probes, procedural atmosphere LUTs, clouds,
> time-of-day** (phases 6–8), plus the `.exr` / metadata leftovers noted above.

## HDR Texture Support

True HDR skies need float textures.

Work items:

- Add `.hdr` decode through `stb_image` float loading. — DONE (phase-4-hdr-textures)
- Consider `.exr` later if dependency cost is acceptable. — still deferred
- Add `uploadTextureFloat` or generalized upload path. — DONE (phase-4-hdr-textures)
- Use `vk::Format::eR16G16B16A16Sfloat` or `eR32G32B32A32Sfloat`. — DONE (rgba16f)
- Add asset metadata for color space and texture usage. — partially done (`AssetEntry.hdr`
  bool); the full design is sketched below.
- Avoid treating HDR sky textures as sRGB RGBA8. — DONE

### Texture usage metadata (design sketch, researched 2026-06)

The `hdr` bool collapses color space and bit depth into one flag. That holds while the only
textures are albedo (sRGB) and HDR panoramas (linear float), and breaks the moment normal or
data maps arrive: a normal map is LDR *and* linear, so `hdr = false` would wrongly route it
through the sRGB path and the sampler would de-gamma the vectors.

Every major engine converges on the same shape — a per-texture *usage* classifier from which
color space, format, and sampling derive, with usage auto-detected at import:

- **Unreal:** `TextureCompressionSettings` (`TC_Default`/`TC_Normalmap`/`TC_Masks`/`TC_HDR`/…)
  picks the format and constrains the separate `sRGB` flag (Masks/HDR/Alpha force it off);
  usage auto-detected from filename suffixes (`_N` → Normalmap).
- **Unity:** `TextureImporter.textureType` (`Default`/`NormalMap`/`Lightmap`/…) +
  `sRGBTexture`, where the type drives the defaults and sRGB is only meaningful for
  color-class textures.
- **Godot 4:** infers usage (`Normal Map: Detect`, `Detect 3D`) and reimports accordingly.
- **glTF 2.0:** stores *no* per-image color-space field at all — the material slot is the
  usage (baseColor/emissive MUST be sRGB; normal/metallicRoughness/occlusion MUST be linear).

Orthogonal `colorSpace` × `usage` fields create unrepresentable-nonsense combinations (sRGB
float, sRGB normal map); no surveyed engine keeps them independent. The right shape here is a
single enum replacing the bool:

```cpp
enum class TextureUsage { Color, Normal, Data, Hdr };
// AssetEntry: replace `bool hdr` with `TextureUsage usage = TextureUsage::Color;`
```

| Usage | Decode | Upload format | Note |
|-------|--------|---------------|------|
| `Color` | `stbi_load` | `eR8G8B8A8Srgb` | today's albedo path |
| `Normal` | `stbi_load` | `eR8G8B8A8Unorm` | linear — sRGB corrupts the vectors |
| `Data` | `stbi_load` | `eR8G8B8A8Unorm` | roughness/metallic/AO |
| `Hdr` | `stbi_loadf` | `eR16G16B16A16Sfloat` | today's `hdr = true` path, unchanged |

- Serialize `"usage"` as a string with default `Color`; map a legacy `"hdr": true` to `Hdr` on
  read — the same no-version-bump trick the bool itself used.
- Detection: `.hdr` extension → `Hdr` (already in place); glTF material slot → usage when
  material-texture import lands (the glTF way); optionally a `_n`/`_normal` filename sniff for
  loose imports (the Unreal way).
- No descriptor work: all four usages land in the same bindless array — the same reason the
  bool was structurally free.
- Out of scope until proven needed: block compression (BC5/BC7), platform overrides, and an
  sRGB *override* flag (Godot's `HDR as sRGB` exists only because mislabeled files exist).

This belongs to whichever plan first imports non-albedo textures (a material-textures plan),
not to the skybox work — it is recorded here because the `hdr` bool is where it grafts on.

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

1. Visible skybox from LDR equirectangular texture. — DONE (phase 2)
2. RGB ambient. — DONE (phase 3)
3. HDR texture import. — DONE (phase-4-hdr-textures)
4. Equirectangular-to-cubemap conversion. — DONE (phase 5, `ibl_equirect.slang`)
5. Diffuse SH irradiance. — DONE as an irradiance cubemap instead (`ibl_irradiance.slang`)
6. PBR material model. — DONE (lighting plan phase 1)
7. Specular IBL. — DONE (lighting plan phase 2)
8. Reflection probes. — future (phase 6)
9. Procedural atmosphere. — future (phase 7)
10. Clouds and time of day. — future (phase 8)

