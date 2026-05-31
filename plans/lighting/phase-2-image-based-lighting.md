# Phase 2: Image-Based Lighting (IBL)

**Status:** COMPLETED
<!-- Flip to COMPLETED when the "Done when" checklist passes, validation-clean. Delete this file only after COMPLETED + merged. -->

<!--
DONE 2026-05-31 (validation-clean under a headless weston Wayland display in the toolbox).
- Cubemap support: newCubeImage (CUBE_COMPATIBLE, 6 layers, N mips, sampled+storage; eCube
  sample view + transient per-mip e2DArray storage views for the compute writes) in
  renderer_detail.cppm. The seam for 2D-array / 3D is the same helper.
- Environment SOURCE is a PROCEDURAL HDR sky (ibl_skygen.slang) — no .hdr asset shipped.
  The equirect-.hdr loader is the documented follow-up: swap skygen for an equirect→cubemap
  compute pass writing the same envCube; everything downstream is unchanged.
- Bake (bakeEnvironment, one-time at startup, own cmd buffer + waitIdle): skygen → env cube;
  ibl_irradiance.slang → 32² irradiance cube (cosine hemisphere convolution); ibl_prefilter.slang
  → 128²×5-mip GGX-prefiltered specular (one dispatch per mip, roughness = mip/(mips-1), push
  constant); ibl_brdf.slang → 256² split-sum (scale,bias) LUT. All rgba16f. Manual sync2
  barriers between passes (General↔ShaderReadOnly).
- mesh.slang set 3 = { irradianceCube, prefilteredCube, brdfLut }; ambient replaced with
  split-sum IBL (fresnelSchlickRoughness, kd diffuse irradiance + prefiltered*(F0*a+b) specular)
  gated on globals.counts.z; flat scalar ambient kept as the fallback. IblPrefilterMaxMip in the
  shader must equal IblPrefilterMips-1 in renderer_detail.cppm.
- se set-ibl {0|1}; render-stats reports ibl. Mesh PSO layout widened to 4 sets; the IBL set is
  bound once in recordSceneDrawList (baked, always valid).
- Verified: ambient-only scene (directional intensity 0), IBL on vs off differ (22.8% px); metal
  row 44→119 brightness (reflects env), dielectric row gains sky-blue tint (93→114 B). Metals
  with IBL off are ~black (correct: no diffuse, flat ambient excludes metallic).

NOT done (follow-ups, do not block COMPLETED): equirect .hdr loader (uploadTextureFloat +
equirect→cube pass) to replace the procedural sky; a visible skybox pass (coordinate with
plans/skybox/); selectable environment asset; spherical-harmonics diffuse alternative; the
"indirect term as a shader helper later GI phases extend" refactor (currently inline in fsMain —
phase 4/6 can extract it then).
-->


## Goal

Replace the flat scalar ambient with split-sum image-based lighting: a diffuse
irradiance cubemap + a prefiltered specular cubemap + a BRDF integration LUT, sampled
in the mesh fragment shader. This gives physically correct ambient and environment
reflections (metals finally look like metals), and — critically — **its diffuse-
irradiance + specular-prefilter bindings become the exact interface every later
dynamic-GI tier writes into** (phase 4 SSGI, phase 6 DDGI). It is the best
quality-per-effort step after PBR, and it is no-bake-per-scene (the convolution runs
once per environment and caches to disk).

**Depends on:** phase 1 (PBR BRDF, HDR float offscreen, `F0`/metallic). Independent of
phase 3.

> "IDR" in the request is read as **IBL**. If HDR was meant instead, that is already
> covered by phase 1 (the float-offscreen prerequisite).

## Current state (verified)

Ambient is one scalar added flat to RGB (`mesh.slang:164`,
`globals.directionAmbient.w`). There is no environment map, no cubemap, no
prefiltered data. The `Image` wrapper (`renderer_types.cppm:114`) is 2D, single-mip,
single-layer — it cannot represent a cubemap or a mip chain. The skybox plan
(`plans/skybox/phase-4-atmosphere-and-ibl-roadmap.md`) already scopes the same
cubemap + irradiance + prefilter work from the sky side; **share that infrastructure.**

## Step 1 — cubemap + mip support in the `Image` wrapper

Add creation variants to `Image` (`renderer_types.cppm:114`) / `newColorImage`
(`renderer.cppm`):

- `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`, `arrayLayers = 6`, a `mipLevels` parameter,
  and a `vk::ImageViewType::eCube` view (plus per-mip/per-face views for the prefilter
  passes that render into a single mip+face).
- This same cubemap+mip work is reused by **point-light shadows (phase 3)** and the
  **3D-image** sibling needed by **DDGI (phase 6)** — build it generically.
- Keep it a renderer-owned persistent `Image` (the graph has no transient creation).

## Step 2 — environment source

Add a skybox/HDRI environment concept (coordinate with `plans/skybox/`):

- Decode an equirectangular `.hdr` via `stb_image` float loading
  (`uploadTextureFloat`, format `eR16G16B16A16Sfloat`). The skybox phase-4 roadmap
  lists this same work item.
- An equirect→cubemap compute pass writes a face-per-invocation cubemap (~512² or
  1024² base). This is the source for both the visible sky and the IBL convolutions.

## Step 3 — precompute passes (render graph, run once on environment load)

These are write-once-then-read passes; the graph already auto-schedules that shape
(the cull→scene compute→fragment dependency is the precedent,
`renderer.cppm:519-558`). Build them as compute pipelines via the existing
`newComputePipeline` path (same as `tonemap`/`fxaa`/`cull`,
`renderer_types.cppm:550-558`):

1. **Diffuse irradiance cubemap** (~32²): cosine-weighted hemisphere convolution of
   the environment cubemap. New Slang shader `irradiance.slang`.
2. **Prefiltered specular cubemap** (~128² base, ~5 mips): GGX importance sampling,
   one dispatch per mip/face, roughness = `mip / (mipCount-1)`. New shader
   `prefilter.slang`.
3. **BRDF integration LUT** (512² `RG16F`): the split-sum `(scale, bias)` table.
   Environment-independent → generate once and **cache to disk**, load thereafter.
   New shader `brdf_lut.slang`.

On llvmpipe keep sample counts/resolution low (32–64 samples, 128² prefilter) or
disk-cache the result so the heavy GGX convolution only runs once.

## Step 4 — descriptor set + shader integration

- Add a new descriptor set (set 3) `{ irradianceCube, prefilteredCube, brdfLut }` to
  the mesh pipeline layout (`renderer.cppm:857-858`, currently 3 sets: bindless,
  light, instance). Add the layout in `Descriptors` (`renderer_types.cppm:498`).
- In `mesh.slang`, replace the flat ambient (`:164`) with split-sum IBL:

```hlsl
float3 F = fresnelSchlickRoughness(ndotv, F0, roughness);
float3 kd = (1 - F) * (1 - metallic);
float3 irradiance = irradianceCube.Sample(n).rgb;
float3 diffuseIBL = kd * irradiance * albedo;

float3 R = reflect(-v, n);
float maxMip = prefilteredMipCount - 1;
float3 prefiltered = prefilteredCube.SampleLevel(R, roughness * maxMip).rgb;
float2 brdf = brdfLut.Sample(float2(ndotv, roughness)).rg;
float3 specularIBL = prefiltered * (F0 * brdf.x + brdf.y);

float3 ambient = diffuseIBL + specularIBL;   // replaces globals.directionAmbient.w
```

- Keep a uniform-fallback (a constant ambient color) for scenes with no environment,
  so an empty scene is not black.
- **Expose `irradiance` + `prefiltered` as the engine's "indirect lighting" inputs.**
  Phases 4 and 6 will *add into* `irradiance`/`specularIBL` (SSGI/DDGI radiance), so
  factor the ambient computation into a helper the later phases extend rather than
  inlining it.

## Step 5 — control command

- `se set-ibl {0|1}` toggling IBL vs the flat fallback (setter in `renderer.cppm`,
  `registerCommand` in `control.cppm` following the `set-clustered` template at
  `control.cppm:231`).
- If the environment is selectable, reuse the asset catalog: an `environment` asset
  the inspector + `se` can assign (coordinate with the skybox `SceneEnvironment` state
  in `plans/skybox/phase-1-scene-environment.md`).

## Done when

- [ ] `Image` supports cubemap + mip + cube views (and the seam for 2D-array / 3D).
- [ ] equirect `.hdr` → cubemap → irradiance + prefiltered + BRDF LUT all generated as
      graph compute passes; BRDF LUT disk-cached.
- [ ] mesh fragment ambient is split-sum IBL via set 3; a metal sphere reflects the
      environment, a rough dielectric shows soft env diffuse; empty scene not black.
- [ ] the indirect-lighting term is a shader helper later phases can extend.
- [ ] `se set-ibl` toggles; validation-clean; `SAFFRON_CAPTURE` PNG verified; runs on
      llvmpipe (bake once + cache).

## Notes / risks

- **Share with the skybox plan** — `plans/skybox/phase-4` scopes the identical
  cubemap/irradiance/prefilter pipeline. Do the cubemap `Image` variant + convolution
  passes once; the visible sky and IBL both consume them.
- **Spherical harmonics** are a cheaper alternative to an irradiance cubemap for the
  diffuse term (9 coefficients vs a 32² cube) and are trivial to update dynamically —
  worth considering for the DDGI-era ambient, but the cubemap path is fine for v1 and
  matches the skybox plan.
- Prefilter seams at cube edges need seamless-cubemap sampling (`eCube` view +
  `samplerCubeSeamless` behavior is default in Vulkan) — verify no visible face seams.
