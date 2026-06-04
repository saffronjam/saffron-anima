# Skybox And Environment Plan

This folder tracks a thorough implementation plan for skyboxes and scene environment rendering in Saffron Engine.

## Status convention

Each phase file carries a `**Status:**` line (`NOT STARTED` / `IN PROGRESS` / `COMPLETED`).
Mark a phase `COMPLETED` when its work is done and validation-clean; delete a phase file
only *after* it is `COMPLETED` and merged.

## Post-lighting reality (READ FIRST — 2026-06-01)

This plan was authored **before** the 8-phase `plans/lighting/` roadmap landed, and lighting
rebuilt exactly the subsystems this plan targets. The grounded facts (from a recon over
`renderer_detail.cppm`, `mesh.slang`, `renderer.cppm`, `scene.cppm`, `assets.cppm`):

- **The engine already bakes a full IBL environment once at init from a *procedural* sky.**
  `bakeEnvironment` (`renderer_detail.cppm`) runs `ibl_skygen.slang` (hardcoded sun + zenith/
  horizon/ground gradient) into `envCube` (128² rgba16f), then convolves `irradianceCube`
  (32²), `prefilteredCube` (128², 5 mips), and `brdfLut` (256²). All four are bound into the
  mesh fragment at **set 3** and drive split-sum PBR ambient. So everything phase-3/4 calls
  "future work" — cubemap creation, irradiance, prefiltered specular, BRDF LUT, PBR — **is
  already shipped.**
- **`envCube` is a sampleable cubemap in `ShaderReadOnlyOptimal` after bake.** The visible sky
  pass (phase 2) should sample *that same cube* by view direction for its default/procedural
  mode — the background and the lighting environment become one coherent source, no new
  texture pipeline needed.
- **Ambient is now scalar `directionAmbient.w`, used only as the non-IBL fallback.** When IBL
  is on (`counts.z != 0`, the default) full split-sum replaces it. Phase-3's "add RGB ambient
  + tiny LightUbo" proposal is largely obsolete; the real `LightUbo` has ~14 fields.
- **HDR offscreen (`OffscreenColorFormat = eR16G16B16A16Sfloat`) + a mandatory tonemap pass
  already exist.** A visible sky writes linear HDR into the scene color target and tonemaps
  for free. No phase-2 blocker there.
- **Still genuinely missing (the real work of this plan):** scene-level `SceneEnvironment`
  state (phase 1), a *visible* sky background pass (phase 2), wiring `SceneEnvironment` to
  drive the procedural sky / ambient / DDGI sky color (phase 3 remnant), and HDR `.hdr` import
  + user equirect→cubemap IBL re-bake + atmosphere/clouds (phase 4 roadmap).
- **Texture decode is sRGB RGBA8 only** (stb_image, no `.hdr`). Phase-2 texture-mode sky is
  LDR for now (explicitly fine — see phase 2).

Net effect on scope: **phases 1 + 2 + the non-done slice of phase 3 are the implementable
work; phase 4 stays a roadmap.** Each phase file below has been updated in place to match.

> **PHASES 1–3 COMPLETED (2026-06-01), validation-clean.** Scene-level `SceneEnvironment`
> state + serialization (v1→v2 migration) + an Environment editor panel + `se` commands
> (phase 1, `94e2de7`); a visible fullscreen sky pass — Color / equirect Texture / Procedural
> (the baked IBL `envCube`, so background and lighting share one source) — integrated across
> all AA modes (phase 2, `211d0ae`); RGB scene-environment ambient for the non-IBL fallback
> (phase 3, `93117eb`); and the procedural skygen sun now follows the scene's directional light
> via an on-demand IBL re-bake (the phase-3 deferred increment, `b863151`) — the visible sky +
> IBL re-tint together when the light moves. **Phase 4 stays a roadmap** (HDR `.hdr` import, user
> equirect→cube IBL re-bake, reflection probes, procedural-atmosphere LUTs, clouds, time-of-day);
> routing the environment sky color into the off-by-default DDGI is also deferred there. Plan
> files are kept until merged (this branch is unpushed); delete after merge per the convention
> above. (Procedural-atmosphere LUTs now shipped in phase 7 — see the phase-7 note below.)
>
> **PHASE 6 COMPLETED (reflection probes, `d709c2f`).** A spatial `ReflectionProbeComponent`
> captures a *local* cubemap on demand, prefilters it with the shipped IBL convolution, and
> blends its specular over the global IBL fallback at mesh set 8 (zero probes = byte-identical
> output). Driven by `set-probes`/`recapture-probes`/`list-probes` + the `add-entity
> reflection-probe` preset; documented at `docs/.../image-based-lighting/reflection-probes.md`.
> See `phase-6-reflection-probes.md`.
>
> **PHASE 7 COMPLETED (procedural atmosphere).** The Hillaire-2020 LUT chain
> (`atmos_transmittance`/`atmos_multiscatter`/`atmos_skyview`/`atmos_skygen`) fills the env cube
> through an `EnvSource::Atmosphere` switch in `bakeEnvironment`, driven by `AtmosphereSettings`
> and re-baked with the sun via the existing exact-compare gate; `set-atmosphere` + the editor
> `Atmosphere` panel section drive it, documented at
> `docs/.../image-based-lighting/procedural-atmosphere.md`. See `phase-7-procedural-atmosphere.md`.
> Phase 8 (clouds, time-of-day) stays future in `phase-4-atmosphere-and-ibl-roadmap.md`.
>
> **Fixed a pre-existing teardown leak (commit `7243ca4`):** a VMA "allocations were not freed
> before destruction" assertion was aborting the process at *exit* (SIGABRT). Root cause (from
> the lighting work, not skybox): `destroyRenderer` cleared `frame.sceneDrawList` but not
> `renderer.rt.frameMeshes`, which `setRtScene` fills with `Ref<GpuMesh>` each frame — so the last
> frame's mesh (vertex + index + BLAS = 3 allocations / 2184 bytes) outlived `vmaDestroyAllocator`.
> The fix clears `rt.frameMeshes`/`frameModels` in `destroyRenderer`. The editor now exits cleanly
> (code 0) on both the frame-bounded and control-`quit` paths.

## Recommendation

The sky should be modeled as scene environment state, not as a normal mesh entity.

Use entity components for things that have a meaningful transform, multiplicity, selection, picking, and local behavior: meshes, cameras, point lights, spot lights, and directional lights. A skybox is global frame state: it controls the background, ambient lighting input, future reflection probes, and eventually atmosphere. Treating the default sky as a giant unlit mesh would make the first image easy, but it would fight depth prepass, picking, batching stats, editor hierarchy semantics, lighting, and future image-based lighting.

The recommended shape is:

```cpp
struct SceneEnvironment
{
    SkyMode skyMode = SkyMode::Color;
    glm::vec3 clearColor{ 0.05f, 0.06f, 0.08f };
    Uuid skyTexture;
    f32 skyIntensity = 1.0f;
    f32 skyRotation = 0.0f;
    f32 exposure = 1.0f;
    bool visible = true;
    bool useSkyForAmbient = true;
    glm::vec3 ambientColor{ 0.15f };
    f32 ambientIntensity = 1.0f;
};

struct Scene
{
    entt::registry registry;
    SceneEnvironment environment;
    const AssetCatalog* catalog = nullptr;
};
```

Add specialized components later only where they represent actual placed objects or volumes, for example `SkyAtmosphereComponent`, `CloudLayerComponent`, or `ReflectionProbeComponent`.

## Current Engine Fit

Relevant current boundaries:

- `engine/source/saffron/scene/scene.cppm` owns components, scene serialization, and `Scene`.
- `engine/source/saffron/assets/assets.cppm::renderScene` resolves scene data into renderer inputs each frame.
- `engine/source/saffron/rendering/renderer.cppm::beginFrameGraph` builds the clustered light pass, optional depth prepass, scene pass, FXAA, app-authored post-process, and UI pass.
- `engine/source/saffron/rendering/renderer_types.cppm` owns renderer frame state and public renderer APIs.
- `editor/source/main.cpp` calls `renderScene` from the editor viewport path.

The integration point should be:

```text
Scene.environment
  -> renderScene(...)
  -> submitSky(...) / setSceneEnvironment(...)
  -> render graph sky pass + lighting constants
```

## External References

Unreal separates sky appearance, ambient/reflection lighting, and atmospheric simulation:

- Sky Light captures distant scene/sky or uses a cubemap for lighting/reflections:
  https://dev.epicgames.com/documentation/it-it/unreal-engine/sky-lights-in-unreal-engine
- Sky Atmosphere is a physically based atmosphere system:
  https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-atmosphere?application_version=4.27
- HDRI Backdrop combines a visible backdrop, cubemap lighting, and projection workflow:
  https://dev.epicgames.com/documentation/unreal-engine/hdri-backdrop-visualization-tool-in-unreal-engine

Frostbite treats sky, atmosphere, and clouds as physically based lighting systems integrated with PBR and time of day:

- https://www.ea.com/news/physically-based-sky-atmosphere-and-cloud-rendering
- https://www.ea.com/news/moving-frostbite-to-pb

## Document Map

- `phase-0-research-and-architecture.md`: final architecture choices and rejected alternatives.
- `phase-1-scene-environment.md`: scene data model, serialization, and editor-facing settings.
- `phase-2-visible-skybox.md`: renderer API, shaders, pipeline, and render graph integration.
- `phase-3-lighting-integration.md`: ambient color, sky-driven lighting, and first IBL steps.
- `phase-4-atmosphere-and-ibl-roadmap.md`: longer-term roadmap — clouds and time-of-day remain future (reflection probes shipped in phase 6, procedural atmosphere in phase 7).
- `phase-6-reflection-probes.md`: spatial reflection probes — local cubemap capture, prefilter, and nearest-probe blend over the global IBL fallback (COMPLETED).
- `phase-7-procedural-atmosphere.md`: physically based atmosphere (Hillaire 2020) — a transmittance/multi-scatter/sky-view LUT chain that fills the env cube as a richer source than the gradient, re-baked with the sun (COMPLETED).

