# Phase 5: Equirectangular IBL And DDGI Sky Routing

**Status:** COMPLETED

<!--
COMPLETED 2026-06-01, validation-clean. Both scopes shipped.
Scope A (panorama -> IBL): new editor/assets/shaders/ibl_equirect.slang (Sampler2D panorama at
binding(0,0), rgba16f RWTexture2DArray outCube at binding(1,0), push float4 {x=rotation,y=intensity},
dir->lat/long uv); EnvSource {Procedural,Equirect} + source/bakedSource/envPanorama on Ibl
(renderer_types.cppm); requestEnvBake (renderer_lighting.cpp) with a source/panorama-identity/
SkygenParams dirty compare, requestSkyBake kept as the Procedural wrapper; bakeEnvironment builds an
ibl_equirect compute pipeline in the transient block (reusing layoutB) and branches the envCube fill
on ibl.source before the convolution chain (missing panorama -> logWarn + Procedural fallback);
beginFrameGraph re-bake consume copies bakedSource alongside bakedParams; renderScene routes
SkyMode::Texture -> requestEnvBake(Equirect, pano) sharing the one loadTextureAsset Ref with the
visible-sky path. The panorama is sampled through the eRepeat linearSampler (NOT the eClampToEdge
ibl.sampler) so the wrapping meridian does not seam. The bake uses rotation=0/intensity=1 (the
visible-sky pass applies skyRotation/skyIntensity itself, to avoid double-counting) — so in Texture
mode the baked IBL ignores skyRotation/skyIntensity; decoupling those is a documented future seam.
Scope B (DDGI sky routing): setDdgiScene gained a trailing glm::vec3 skyColor (assigned to d.skyColor);
renderScene passes useSkyForAmbient ? ambientColor*ambientIntensity : {0.1,0.13,0.2}.
Teardown: destroyRenderer now resets ibl.envPanorama before vmaDestroyAllocator (a Ref<GpuTexture>
that would otherwise outlive the allocator when a texture sky is active at exit -> VMA abort).
Verified headless (weston): Texture-vs-Procedural sky = 100% viewport pixel delta; "ibl baked" fires
only on source/panorama/sky change (no per-frame churn); cross-process reload drives the float path;
validation-clean; clean exit. DDGI visual A/B was inconclusive on llvmpipe (tiny indirect term) but the
routing is wired + reviewed correct. Commit: see git log (skybox phase 5).
-->


## Goal

Two independent increments that share the same theme — make `SceneEnvironment` the
single source the environment systems read, instead of hardcoded defaults:

- **Scope A — user panorama → IBL.** Let a user equirectangular HDR (or LDR) panorama
  become the IBL environment, not only the procedural skygen. Add an equirect→cube
  compute prepass that fills `envCube`, then reuse the *existing* irradiance / prefilter
  / BRDF convolution chain and the phase-3 on-demand re-bake machinery verbatim. The
  visible sky pass (phase 2) already samples `envCube` for Procedural mode, so the
  background follows the same panorama for free.
- **Scope B — DDGI sky routing.** Route the scene-environment sun + sky color into the
  DDGI trace inputs instead of the struct-default `skyColor`, closing the phase-3
  deferred item.

The two scopes are orthogonal and may ship in either order; Scope A depends on phase 4
(HDR `.hdr` decode) only for *quality* — an LDR panorama already round-trips through the
same path, just dimmer.

## Post-lighting reality / Current Engine Fit

Almost everything Scope A needs already exists. The only genuinely new pieces are one
compute shader and an env-source switch.

Already shipped (reuse, do not rebuild):

- **The whole IBL bake** — `bakeEnvironment(Renderer&, const SkygenParams&, bool firstBake)`
  at `renderer_detail.cppm:3297-3595`. On `firstBake=true` it allocates `envCube` /
  `irradianceCube` / `prefilteredCube` / `brdfLut` (`:3302-3316`); it then dispatches
  `ibl_skygen` into `envCube` (`:3493-3498`), `ibl_irradiance` reading `envCube` (`:3507-3509`),
  `ibl_prefilter` per mip reading `envCube` (`:3518-3527`), and `ibl_brdf` (`:3536-3538`).
  The persistent descriptor sets (set 3 mesh IBL, set 1 visible sky) are written only on
  `firstBake` (`:3559-3588`). A re-bake (`firstBake=false`) just `waitIdle`s and overwrites
  the same images in place (`:3318-3323`), reusing every view + descriptor.
- **The convolution chain is source-agnostic.** `ibl_irradiance.slang` / `ibl_prefilter.slang`
  read `envCube` as a `SamplerCube` (set 0 binding 0); they do not care how `envCube` was
  filled. The bake binds `envCube.view` as the convolution source at `renderer_detail.cppm:3445`
  and `:3449`, after transitioning it to `eShaderReadOnlyOptimal` at `:3499-3501`. So if a new
  prepass fills `envCube` instead of `ibl_skygen`, the rest of the bake produces identical
  irradiance/prefilter/BRDF with zero changes.
- **On-demand re-bake machinery.** `requestSkyBake(Renderer&, const SkygenParams&)`
  (`renderer_lighting.cpp:189-200`) sets `ibl.pendingParams` and arms `ibl.rebakePending`
  only when the params differ from `ibl.bakedParams` (exact compare, no per-frame churn).
  `beginFrameGraph` (`renderer.cppm:667-678`) consumes the flag at a GPU-idle point, calls
  `bakeEnvironment(..., false)`, and copies `pendingParams → bakedParams`. The initial bake
  is `bakeEnvironment(..., true)` at `renderer.cppm:275`.
- **`SkygenParams`** (`renderer_types.cppm:721-726`): `sunDir` / `sunIntensity` / `sunColor`,
  the equality-compared key driving the re-bake. **`Ibl`** (`renderer_types.cppm:732-747`)
  holds `envCube` plus `bakedParams` / `pendingParams` / `rebakePending`.
- **Cube image creation** — `newCubeImage(Renderer&, u32 size, u32 mipLevels, vk::Format)`
  at `renderer_detail.cppm:706` creates a `CUBE_COMPATIBLE` sampled+storage image; `envCube`
  is `IblEnvSize`=128² `IblColorFormat`=`eR16G16B16A16Sfloat` (`renderer_detail.cppm:1084-1089`).
  No new resource type is needed.
- **Compute pipeline factory** — `newComputePipeline(Renderer&, std::string_view shaderName,
  vk::DescriptorSetLayout, u32 pushConstantSize = 0)` at `renderer_detail.cppm:1166-1168`,
  used for `ibl_skygen` at `:3374`.
- **Visible sky** — phase 2's `sky.slang` Procedural mode samples `envCube` via set 1
  (`renderer_types.cppm:754-766`, set written at `renderer_detail.cppm:3579-3587`). Any
  panorama that lands in `envCube` shows up as the background with no sky-pass change.
- **Scene env state + plumbing** — `SkyMode {Color, Texture, Procedural}`
  (`scene.cppm:201-206`), `SceneEnvironment` (`scene.cppm:211-223`), its JSON round-trip
  (`environmentToJson`/`environmentFromJson` `scene.cppm:381-417`, `skyModeName`/
  `skyModeFromName` `:361-379`), the Environment panel (`editor_panels.cpp:191-224`), and
  `se get-environment`/`set-environment` (`control_commands_scene.cpp:364-397`).
- **Panorama upload + bindless slot** — `loadTextureAsset(AssetServer&, Renderer&, Uuid)`
  (`assets.cppm:269`) returns a `Ref<GpuTexture>` with a `bindlessIndex`; `renderScene`
  already loads `env.skyTexture` for Texture-mode sky (`assets.cppm:646-657`).

Genuinely new (this phase):

- **`ibl_equirect.slang`** — a compute shader projecting a 2D equirectangular sampler into
  the `envCube` 6-layer storage image. Confirmed absent: a grep over `editor/assets/shaders/`
  finds only `ibl_brdf` / `ibl_irradiance` / `ibl_prefilter` / `ibl_skygen` / `sky` — no
  equirect/latlong/panorama compute shader.
- **An env-source selector** on `Ibl` deciding whether `ibl_skygen` or `ibl_equirect` fills
  `envCube` before the convolution.
- **DDGI sky-color routing** — `setDdgiScene` (`renderer.cppm:1777-1811`) sets
  `ddgi.sunDir`/`sunColor`/`sunIntensity` but **never** `ddgi.skyColor`; that field
  (`renderer_types.cppm:866`) keeps its hardcoded default `{0.1, 0.13, 0.2}` and is what the
  DDGI trace push reads (`renderer.cppm:1034` → push at `:1072-1075`, consumed by
  `ddgi_trace.slang`). No setter exists today.

## Scope A — Data Model

Add an env-source enum and a panorama slot to `Ibl` (`renderer_types.cppm:732-747`), beside
`bakedParams`/`pendingParams`:

```cpp
enum class EnvSource
{
    Procedural,     // ibl_skygen.slang (current behavior, default)
    Equirect,       // ibl_equirect.slang reads a user panorama
};

struct Ibl
{
    // ... existing fields (envCube, irradianceCube, prefilteredCube, brdfLut, sampler,
    //     setLayout, set, ready, useIbl, bakedParams, pendingParams, rebakePending) ...
    EnvSource source = EnvSource::Procedural;        // which shader fills envCube
    EnvSource bakedSource = EnvSource::Procedural;   // source the current envCube was baked with
    Ref<GpuTexture> envPanorama;                     // Equirect source (held so its bindless GPU texture survives the bake)
    vk::DescriptorSetLayout equirectLayout;          // equirect set: panorama sampler + envCube storage
    vk::DescriptorSet equirectSet;                   // persistent; panorama view rewritten when it changes
};
```

`SkygenParams` is unchanged for Procedural. For Equirect, the *re-bake key* must also
notice a panorama change, so extend the compare in `requestSkyBake` (below) to consider
`source` + the panorama identity, not just the three `SkygenParams` floats.

`SceneEnvironment` (`scene.cppm:211-223`) does **not** need a new field for the common
case: reuse the existing `skyTexture` Uuid + `skyMode`. Define the contract:

- `skyMode == Texture` already loads `skyTexture` as a bindless panorama for the *visible*
  sky (`assets.cppm:646-657`). Extend it so the *same* panorama also drives IBL: set
  `Ibl.source = Equirect` and feed `skyTexture` into the bake.
- `skyMode == Procedural` (default) keeps `Ibl.source = Procedural`.
- `skyMode == Color` keeps Procedural IBL (flat color sky as background, neutral analytic
  IBL); no panorama.

If a future need arises to decouple the *lighting* panorama from the *background* panorama,
add a dedicated `Uuid iblPanorama` to `SceneEnvironment` (bump `SceneVersion` 2→3 with an
`environmentFromJson` default per the phase-1 migration pattern). Not required for v1 of
this phase — call it out as a seam.

## Scope A — Renderer API

`renderScene` (`assets.cppm:618-659`) currently always derives `SkygenParams` from the
directional light and calls `requestSkyBake`. Extend the same block so a Texture-mode
panorama re-routes the IBL source. Add one new renderer entry point next to `requestSkyBake`
in `renderer_lighting.cpp` (declared in `renderer_types.cppm` beside the other lighting API):

```cpp
// Selects the IBL environment source. Procedural uses ibl_skygen + SkygenParams; Equirect
// projects `panorama` into envCube. Arms a re-bake (consumed in beginFrameGraph) when the
// source OR the panorama OR the SkygenParams change. No-op if nothing changed.
void requestEnvBake(Renderer& renderer, EnvSource source, Ref<GpuTexture> panorama,
                    const SkygenParams& sky);
```

Fold the existing `requestSkyBake` compare into it:

```cpp
void requestEnvBake(Renderer& renderer, EnvSource source, Ref<GpuTexture> panorama,
                    const SkygenParams& sky)
{
    Ibl& ibl = renderer.ibl;
    const bool panoChanged =
        (source == EnvSource::Equirect) &&
        (ibl.envPanorama == nullptr || panorama == nullptr ||
         ibl.envPanorama->bindlessIndex != panorama->bindlessIndex);
    const bool srcChanged = source != ibl.bakedSource;
    const SkygenParams& baked = ibl.bakedParams;
    const bool skyChanged = sky.sunDir != baked.sunDir || sky.sunColor != baked.sunColor ||
                            sky.sunIntensity != baked.sunIntensity;
    if (srcChanged || panoChanged || (source == EnvSource::Procedural && skyChanged))
    {
        ibl.rebakePending = true;
    }
    ibl.source = source;
    ibl.envPanorama = std::move(panorama);  // keep the Ref alive across the bake
    ibl.pendingParams = sky;
}
```

Keep `requestSkyBake` as a thin wrapper (`requestEnvBake(r, EnvSource::Procedural, nullptr, sky)`)
so phase-3 callers stay valid, or replace its single call site in `renderScene`.

In `beginFrameGraph` the consume point (`renderer.cppm:667-678`) needs `bakedSource` updated
on success alongside `bakedParams`:

```cpp
if (renderer.ibl.rebakePending)
{
    if (Result<void> r = bakeEnvironment(renderer, renderer.ibl.pendingParams, false); r)
    {
        renderer.ibl.bakedParams = renderer.ibl.pendingParams;
        renderer.ibl.bakedSource = renderer.ibl.source;
    }
    else { logError(r.error()); }
    renderer.ibl.rebakePending = false;
}
```

## Scope A — Shader Approach

New `editor/assets/shaders/ibl_equirect.slang`, mirroring `ibl_skygen.slang`'s
structure (same set 0 binding 0 storage cube; same `cubeFaceDir`; same numthreads(8,8,1);
same per-face dispatch over 6 layers). The only differences: an added input panorama sampler
and a direction→lat/long mapping instead of the analytic gradient.

```hlsl
// Projects an equirectangular panorama into the IBL environment cube (6 layers of a 2D
// array view). One invocation per output texel; tid.z is the cube face.
[[vk::image_format("rgba16f")]]
[[vk::binding(1, 0)]] RWTexture2DArray<float4> outCube;     // envCube storage (matches ibl_skygen)
[[vk::binding(0, 0)]] Sampler2D<float4> panorama;           // user equirect (HDR or LDR)

struct EquirectParams { float4 params; };  // params.x = rotation (yaw radians), .y = intensity
[[vk::push_constant]] EquirectParams pc;

float3 cubeFaceDir(uint face, float2 uv) { /* identical to ibl_skygen.slang:17-27 */ }

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint w, h, layers; outCube.GetDimensions(w, h, layers);
    if (tid.x >= w || tid.y >= h || tid.z >= layers) { return; }
    float2 uv = (float2(tid.xy) + 0.5) / float2(w, h) * 2.0 - 1.0;
    float3 dir = normalize(cubeFaceDir(tid.z, uv));
    float yaw = atan2(dir.z, dir.x) + pc.params.x;
    float u = yaw / (2.0 * 3.14159265) + 0.5;
    float v = acos(clamp(dir.y, -1.0, 1.0)) / 3.14159265;     // 0 at +Y zenith
    float3 c = panorama.SampleLevel(float2(u, v), 0.0).rgb * pc.params.y;
    outCube[tid] = float4(c, 1.0);
}
```

Notes:

- Binding numbers are deliberate: `outCube` stays at the binding the skygen path uses
  (`ibl_skygen.slang:6` is binding 0; for equirect a 2-binding layout is needed, so keep the
  storage at binding 1 and put the sampler at binding 0, matching the new `equirectLayout`).
- Sampling at LOD 0 keeps the prepass simple; the convolution chain handles roughness from
  the *cube*, not the panorama, so panorama mips are unnecessary here.
- The panorama may be sampled either from the bindless array (set 0 in the runtime scene
  shaders) or via a dedicated combined-image-sampler bound for the bake. The bake uses a
  *transient* descriptor pool (`renderer_detail.cppm:3325-3334`) and is a one-shot submit on
  the graphics queue (`:3544-3550`); bind the panorama through a per-bake combined-image-sampler
  in that transient pool (the bindless set is a runtime descriptor and not the cleanest fit
  for the synchronous bake submit). The `envPanorama` `Ref<GpuTexture>` held on `Ibl` keeps the
  VMA image + view alive for the bake.

## Scope A — Bake Integration

The cleanest insertion is in `bakeEnvironment` right before the procedural skygen dispatch
at `renderer_detail.cppm:3489-3498`. The `envCube → eGeneral` barrier (`:3490-3492`), the
post-dispatch `eGeneral → eShaderReadOnlyOptimal` barrier (`:3499-3501`), and everything
after are reused unchanged — only the dispatched pipeline + descriptor set differ.

1. In `bakeEnvironment`, after the `envCube → eGeneral` barrier and before the skygen
   `bindPipeline` at `:3493`, branch on `renderer.ibl.source`:
   - `Procedural`: existing skygen path (`:3493-3498`).
   - `Equirect`: bind a new `ibl_equirect` compute pipeline + a transient set that binds
     `renderer.ibl.envPanorama->view` (combined-image-sampler, binding 0) and `envStore`
     (storage, binding 1, already created at `:3397`); push `{ rotation, intensity }`;
     `cmd.dispatch(group(IblEnvSize), group(IblEnvSize), 6)`.
   The `ibl_equirect` pipeline is built next to `skygenP` (`:3374-3382`) in the same
   transient block via `newComputePipeline(renderer, "shaders/ibl_equirect.spv",
   equirectLayout, sizeof(glm::vec4))`. Its source descriptor needs a `layout` with a sampler
   at binding 0 + a storage image at binding 1 — that is exactly the existing `layoutB`
   (`:3347-3365`), so reuse `layoutB` rather than adding a new layout.
2. Guard the Equirect branch: if `source == Equirect && envPanorama == nullptr`, fall back to
   the Procedural dispatch (and `logWarn`) so a missing panorama degrades gracefully — mirror
   the visible-sky fallback at `assets.cppm:653-656`.
3. Nothing downstream changes: irradiance/prefilter/brdf, the persistent set writes on
   `firstBake` (set 3 + set 1), the layout fixups (`:3552-3555`) all run as-is. The visible
   sky (set 1, Procedural mode) now shows the panorama because it samples the same `envCube`.

`renderScene` (`assets.cppm:618-659`) is the only caller change. Replace the `requestSkyBake`
block (`:621-627`) and let Texture mode drive the IBL source:

```cpp
{
    SkygenParams skyBake;
    skyBake.sunDir = -lightDir;
    skyBake.sunColor = lightColor;
    skyBake.sunIntensity = lightIntensity;
    const SceneEnvironment& env = scene.environment;
    if (env.skyMode == SkyMode::Texture && env.skyTexture.value != 0)
    {
        Ref<GpuTexture> pano = loadTextureAsset(assets, renderer, env.skyTexture);
        if (pano) { requestEnvBake(renderer, EnvSource::Equirect, pano, skyBake); }
        else      { requestEnvBake(renderer, EnvSource::Procedural, nullptr, skyBake); }
    }
    else
    {
        requestEnvBake(renderer, EnvSource::Procedural, nullptr, skyBake);
    }
}
```

This shares the `loadTextureAsset` call with the visible-sky block at `:646-657` — capture the
`Ref` once and pass it to both, so a Texture-mode panorama becomes background *and* IBL from
one load.

## Scope A — Asset Loading

Reuse `loadTextureAsset` (`assets.cppm:269`) and the existing `AssetType::Texture` catalog
entry (`scene.cppm:116`). Phase 4 adds the `.hdr` float decode + a float upload path so the
panorama is `rgba16f` rather than sRGB RGBA8; this phase needs no asset-system change beyond
that. An LDR panorama works through the identical path (lower dynamic range, fine for testing
before phase 4 lands). Do **not** introduce a new asset type — a panorama is a texture.

## Scope B — DDGI Sky Routing

DDGI is `useDdgi=false` by default (`renderer_types.cppm:849`) and adds five compute passes
only when enabled, so this is a low-risk wiring change.

Today `setDdgiScene` (`renderer.cppm:1777-1811`) sets `d.sunDir`/`d.sunColor`/`d.sunIntensity`
(`:1808-1810`) but leaves `d.skyColor` at its constructor default `{0.1, 0.13, 0.2}`
(`renderer_types.cppm:866`); the DDGI trace pass captures `d.skyColor` (`renderer.cppm:1034`)
into its push constant (`:1072-1075`). There is **no** skyColor setter.

Pick a sky color that represents the scene's environment ambient and route it:

1. Extend `setDdgiScene` (decl `renderer_types.cppm:1130-1133`, def `renderer.cppm:1777`) with
   a trailing `glm::vec3 skyColor` parameter and assign `d.skyColor = skyColor;` next to the
   sun fields at `:1810`. Keep it last with no default to force every caller to be explicit
   (only `renderScene` calls it — `assets.cppm:607-608`).
2. In `renderScene` (`assets.cppm:600-609`), derive the DDGI sky color from the same scene
   environment the fallback ambient already uses (`scene.environment.ambientColor *
   ambientIntensity`, computed at `:613-616` just below). Move that computation above the
   `setDdgiScene` call (or duplicate the cheap product) and pass it:

```cpp
const glm::vec3 ddgiSky = scene.environment.useSkyForAmbient
    ? scene.environment.ambientColor * scene.environment.ambientIntensity
    : glm::vec3{ 0.1f, 0.13f, 0.2f };
setDdgiScene(renderer, boxMins, boxMaxs, boxAlbedos, volMin, volExt,
             lightDir, lightColor, lightIntensity, ddgiSky);
```

This makes the off-by-default DDGI indirect bounce respect the scene's sky/ambient instead of
a hardcoded blue. The sun fields already track the directional light through this same call,
so the routing is now complete. (A richer source — e.g. a low-mip average of `irradianceCube`
when IBL is on — is a future refinement; the ambient product is the grounded v1.)

## Render Graph Placement

No render-graph change in either scope. Scope A runs entirely inside `bakeEnvironment`, a
synchronous one-shot submit outside the per-frame graph (it is triggered from the GPU-idle
re-bake point in `beginFrameGraph`, `renderer.cppm:667-678`, before any pass is recorded).
Scope B only changes a value pushed into the already-existing DDGI trace pass
(`renderer.cppm:1060-1078`). The frame graph (shadow→cull→…→DDGI→sky→scene→tonemap) is untouched.

## Control + Docs (part of "done")

- The `se set-environment` / `get-environment` commands (`control_commands_scene.cpp:364-397`)
  already cover `skyMode` + `skyTexture`, which is all Scope A needs to drive a panorama IBL
  from the CLI (set `skyMode:texture` + `skyTexture:<uuid>` and the bake re-routes). No new
  command is required for the common case. If a dedicated `iblPanorama` field is added later
  (the decoupling seam), extend the merge block at `:385-394` and `environmentToJson`/
  `environmentFromJson` (`scene.cppm:381-417`) following the existing field pattern.
- Scope B is internal renderer routing with no scene-facing field, so no new `se` command;
  it is observable via the existing `se set-ddgi 1` + a screenshot A/B.
- Update the matching `docs/content/` explanation page for IBL/environment (the
  environment/skybox concept page) in the same change: note that the IBL environment can now
  be sourced from a user equirectangular panorama, and that DDGI indirect light reads the
  scene sky color. Add the row to the hub `_index.md` if a new page is created. Per `docs/README.md`.

## Implementation Steps

Scope B first (smallest, no shader, no reconfigure):

1. Add the `skyColor` parameter to `setDdgiScene` (decl + def) and assign `d.skyColor`.
2. Compute `ddgiSky` from `scene.environment` in `renderScene` and pass it.

Scope A:

3. Add `EnvSource` enum + `source`/`bakedSource`/`envPanorama`/`equirectLayout`/`equirectSet`
   to `Ibl` (`renderer_types.cppm`).
4. Write `editor/assets/shaders/ibl_equirect.slang`; **run a CMake reconfigure** so the GLOB
   (`cmake/CompileShaders.cmake:7`) picks up the new `.slang` and compiles it to
   `bin/shaders/ibl_equirect.spv`.
5. Add `requestEnvBake` in `renderer_lighting.cpp` (declared in `renderer_types.cppm` beside
   the lighting API); make `requestSkyBake` a wrapper or replace its call site.
6. In `bakeEnvironment` (`renderer_detail.cppm`), build the `ibl_equirect` compute pipeline in
   the transient block (reuse `layoutB`) and branch the `envCube` fill on `ibl.source` before
   the skygen dispatch (`:3489-3498`); add the missing-panorama → Procedural fallback.
7. Update the `beginFrameGraph` re-bake consume (`renderer.cppm:667-678`) to copy `bakedSource`.
8. In `renderScene` (`assets.cppm:618-659`), route Texture mode to `requestEnvBake(Equirect, …)`
   and share the single `loadTextureAsset` with the visible-sky block.
9. Update the env/IBL docs page.

Place all C++ in existing TUs (`renderer_types.cppm`, `renderer_lighting.cpp`,
`renderer_detail.cppm`, `renderer.cppm`, `assets.cppm`) — no new translation unit, no
`CMakeLists.txt` edit (only the reconfigure to glob the new shader). Build only in the
`saffron-build` toolbox with `-j1`. Errors stay `std::expected<T,std::string>`; GPU resources
stay `Ref<T>`.

## Verification

Build + headless run in the toolbox:

```sh
toolbox run -c saffron-build bash -lc '
  cd /var/home/saffronjam/repos/SaffronEngine
  cmake --preset debug                 # reconfigure: globs ibl_equirect.slang
  cmake --build build/debug -j1
  VK_LOADER_DEBUG=none SAFFRON_EXIT_AFTER_FRAMES=5 ./build/debug/bin/SaffronEditor
'
```

Scope A — panorama IBL:

- Run the editor; in another shell `se import-texture <path/to/panorama.hdr>` (or a `.png`
  pre-phase-4) then `se set-environment skyMode:texture skyTexture:<uuid>`.
- `se screenshot viewport /tmp/equirect.png` — the visible sky must show the panorama (it
  samples `envCube`, now filled by `ibl_equirect`).
- Place a metallic/low-roughness `MeshComponent` cube (`se create` + `se set-material`);
  confirm its specular reflection shows the panorama (mesh set 3 prefilter cube), proving the
  convolution chain re-baked from the equirect source, not just the background.
- Toggle back: `se set-environment skyMode:procedural` and screenshot — the analytic sky +
  neutral IBL must return (re-bake fired because `source` changed). Diff the two PNGs with
  numpy/PIL to confirm a large pixel delta on both sky and the reflective mesh:
  `python3 -c "from PIL import Image; import numpy as np; a=np.asarray(Image.open('/tmp/a.png')); b=np.asarray(Image.open('/tmp/b.png')); print((a!=b).mean())"`.
- No re-bake churn: hold the panorama fixed across frames; confirm `rebakePending` does not
  re-fire every frame (the equality compare in `requestEnvBake` gates it) — check the
  `"ibl baked"` log line (`renderer_detail.cppm:3592`) appears only on change.

Scope B — DDGI sky routing:

- `se set-ddgi 1`; with a default (procedural) environment, screenshot
  `/tmp/ddgi_default.png`.
- `se set-environment ambientColor:{x:0.6,y:0.1,z:0.1} ambientIntensity:1.0`; screenshot
  `/tmp/ddgi_warm.png`. Indirect-lit surfaces in shadow must shift toward the new sky color
  (numpy mean delta > 0; the lit-side direct term is unchanged so the delta concentrates in
  shadowed/indirect regions). With the routing absent this delta is ~0.

Both scopes:

- Run with `SAFFRON_VALIDATION=1` (or the project's `VAL=0`/validation-clean convention) and
  confirm zero validation errors — the equirect prepass reuses the existing barriers, so the
  bake stays clean; the DDGI change touches no GPU layout.
- Confirm clean exit (code 0) on the frame-bounded run and a `se quit`.
