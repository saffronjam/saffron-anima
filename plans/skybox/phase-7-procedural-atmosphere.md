# Phase 7: Procedural Atmosphere

**Status:** NOT STARTED

## Goal

Replace the analytic horizon→zenith gradient that `ibl_skygen.slang` paints into `envCube`
with a physically based atmosphere (Hillaire 2020 style): a transmittance LUT, a
multiple-scattering LUT, a sky-view LUT, and an aerial-perspective volume, all driven by the
scene's directional light as the sun, with a sun disk. The key reuse is that the atmosphere
is a **richer environment SOURCE** — it produces the same `envCube` the existing IBL
convolution chain and the visible sky pass already consume, and it re-bakes through the
phase-3 sun-change machinery, so the visible sky, the IBL, and (once phase-5 lands) the DDGI
sky stay coherent without any new convolution or descriptor work.

First target: the LUT chain + a sky-view-fed `envCube` source, switched on through the same
re-bake path that already follows the directional light. Later increments (explicitly scoped
below, not first-target): aerial-perspective applied to scene color, and higher multi-scatter
precision.

## Post-lighting reality / Current Engine Fit

Everything the convolution + visible-sky side needs is already shipped. This phase only swaps
what fills `envCube` and adds the LUTs feeding it.

- **`ibl_skygen.slang` is the gradient this phase replaces.** It writes `proceduralSky(dir)`
  (a zenith/horizon/ground lerp + `pow(s, 1200)` sun disk) into the 6-layer `rgba16f`
  `RWTexture2DArray` `outCube` (set 0 binding 0), one invocation per texel, `tid.z` = cube
  face, `numthreads(8,8,1)` (`editor/assets/shaders/ibl_skygen.slang:5-72`). Its push constant
  is `struct SkyParams { float4 sunDir; float4 sunColor; }` (`:9-14`), where `sunDir.w` is the
  intensity scale. The atmosphere sky-gen shader keeps this exact output contract
  (storage-cube layoutA, same push struct shape) so `bakeEnvironment` binds it identically.

- **`bakeEnvironment(Renderer&, const SkygenParams& sky, bool firstBake)` owns the whole bake**
  (`engine/source/saffron/rendering/renderer_detail.cppm:3297`). On `firstBake` it allocates
  `envCube`/`irradianceCube`/`prefilteredCube`/`brdfLut` via `newCubeImage`/`newColorImage`
  (`:3302-3317`); a re-bake (`:3318-3323`) only `device.waitIdle()`s and reuses the same image
  memory. It builds a transient pool + layouts (`layoutA` = one storage image at `:3336-3345`;
  `layoutB` = sampler + storage at `:3347-3356`), creates the four compute pipelines —
  `skygenP` from `shaders/ibl_skygen.spv` with layoutA + the `SkyParams` push
  (`:3374-3376`), `irrP`/`preP`/`lutP` (`:3377-3381`) — then dispatches: skygen into `envCube`
  (`Undefined→General` barrier, bind, `pushConstants` the `SkyPush { vec4 sunDir(sunIntensity in
  .w), vec4 sunColor }` at `:3495-3497`, `dispatch(group(IblEnvSize), group(IblEnvSize), 6)`,
  `General→ShaderReadOnlyOptimal` at `:3499-3501`), then irradiance (`:3503-3512`), prefilter
  per-mip (`:3514-3530`), BRDF LUT (`:3532-3541`). On `firstBake` it writes the persistent set 3
  (irradiance/prefiltered/brdf, `:3561-3575`) and set 1 (sky `envCube`, `:3577-3587`); a re-bake
  reuses those sets unchanged. **The convolution chain reads `envCube` as an opaque
  `SamplerCube`, so it does not care that the contents now come from atmosphere LUTs instead of
  a gradient.**

- **The on-demand re-bake machinery already follows the sun (phase 3).** `requestSkyBake`
  (`engine/source/saffron/rendering/renderer_lighting.cpp:189-200`) sets `ibl.pendingParams` and
  flags `rebakePending` only when `sunDir`/`sunColor`/`sunIntensity` differ from `bakedParams`
  (exact compare, no float churn). `beginFrameGraph` consumes it at the GPU-idle frame start
  (`engine/source/saffron/rendering/renderer.cppm:667-678`): on `rebakePending` it calls
  `bakeEnvironment(renderer, pendingParams, false)`, copies `pendingParams→bakedParams`, clears
  the flag. `renderScene` arms it each frame from the directional light — `skyBake.sunDir =
  -lightDir`, `sunColor = lightColor`, `sunIntensity = lightIntensity`
  (`engine/source/saffron/assets/assets.cppm:618-627`). The initial bake runs at renderer init
  (`renderer.cppm:275`). **The atmosphere reuses this verbatim: whenever the sun moves, the LUTs
  + envCube re-bake and the visible sky / IBL re-tint together. No new trigger is needed beyond
  re-baking the LUTs inside `bakeEnvironment` before the sky-gen dispatch.**

- **`SkygenParams` is the bake input** (`engine/source/saffron/rendering/renderer_types.cppm:721-726`):
  `glm::vec3 sunDir` (direction TO sun), `f32 sunIntensity`, `glm::vec3 sunColor`. `Ibl`
  (`:732-747`) holds the four `Image`s, `prefilterMips`, the shared `sampler`, set-3 layout/set,
  `ready`, `useIbl`, and the `bakedParams`/`pendingParams`/`rebakePending` re-bake fields.

- **IBL sizing constants** (`engine/source/saffron/rendering/renderer_detail.cppm:1084-1089`):
  `IblColorFormat = eR16G16B16A16Sfloat`, `IblEnvSize = 128`, `IblIrradianceSize = 32`,
  `IblPrefilterSize = 128`, `IblPrefilterMips = 5`, `IblLutSize = 256`. The atmosphere LUT sizes
  go here too (new constants).

- **The visible sky already samples `envCube` by ray direction.** `sky.slang` mode 2 (Procedural)
  does `envCube.SampleLevel(dir, 0.0)` from set 1 binding 0
  (`editor/assets/shaders/sky.slang:15,71-73`); `recordSky` pushes `inverse(viewProj)` + params
  (`engine/source/saffron/rendering/renderer_drawlist.cpp:371-392`). Because the atmosphere bakes
  into that same `envCube`, **the visible sky becomes the atmosphere for free in mode 2** — no
  sky.slang change is required for the first target. A later increment can let sky.slang sample
  the sky-view LUT directly for a higher-fidelity horizon (see Step 6).

- **`newComputePipeline(renderer, shaderName, setLayout, pushConstantSize=0)`**
  (`engine/source/saffron/rendering/renderer_detail.cppm:1166-1168`) is the dispatch-pipeline
  factory; `newColorImage(renderer, w, h, format, storage=false, samples=e1)` (`:226-228`),
  `newImage3D(renderer, w, h, d, format)` (`:355`), and `newCubeImage` (`:706`) are the image
  factories. `light_cull` (`renderer.cppm:799-816`) is the minimal compute-pass template
  (`RgPass{ kind=Compute, accesses, execute }` + `addPass`); the DDGI 5-pass sequence
  (`renderer.cppm:1004-1138`) is the multi-pass-with-cross-dependencies template. The atmosphere
  LUTs are baked **inside `bakeEnvironment`** (a one-shot transient command buffer with manual
  barriers, like the existing irradiance/prefilter chain), NOT as per-frame render-graph passes —
  they only change on a sun/parameter change, exactly like the rest of the bake.

- **New `.slang` files need a CMake reconfigure.** `saffron_compile_shaders`
  (`cmake/CompileShaders.cmake:6-7`) globs `assets/shaders/*.slang` with `CONFIGURE_DEPENDS`, so a
  new shader compiles after `cmake --preset debug` reruns. No `CMakeLists.txt` edit. New `.cpp`
  TUs do require a CMakeLists edit, so put new C++ in the existing `renderer_detail.cppm` (bake)
  / `renderer_lighting.cpp` (request) / `scene.cppm` (data) / `control_commands_scene.cpp`
  (CLI) / `editor_panels.cpp` (UI) TUs.

## Data Model

Add `AtmosphereSettings` to `engine/source/saffron/scene/scene.cppm`, next to `SceneEnvironment`
(`:211-223`), and embed it as a field. These are the Rayleigh/Mie/ozone params carried forward
from the old `plans/skybox/phase-4-atmosphere-and-ibl-roadmap.md:104-119`:

```cpp
struct AtmosphereSettings
{
    bool enabled = false;                       // false = keep the gradient ibl_skygen
    f32 planetRadius = 6360.0f;                 // km
    f32 atmosphereHeight = 100.0f;              // km (top - planet radius)
    glm::vec3 rayleighScattering{ 5.802f, 13.558f, 33.1f };  // 1/Mm, sea level
    f32 rayleighScaleHeight = 8.0f;             // km
    f32 mieScattering = 3.996f;                 // 1/Mm
    f32 mieScaleHeight = 1.2f;                  // km
    f32 mieAnisotropy = 0.8f;                   // Henyey-Greenstein g
    glm::vec3 ozoneAbsorption{ 0.650f, 1.881f, 0.085f };     // 1/Mm
    f32 sunDiskAngularRadius = 0.00465f;        // radians (~0.27 deg)
    f32 sunDiskIntensity = 20.0f;
};
```

Add to `SceneEnvironment` (`scene.cppm:211-223`):

```cpp
AtmosphereSettings atmosphere;
```

`AtmosphereSettings::enabled` is the source switch: when true, the atmosphere LUT chain fills
`envCube`; when false, the existing gradient `ibl_skygen` runs (current behavior). This is the
same switch phase-5 toward (Procedural-gradient vs Equirect vs Atmosphere); phase-5 should
extend the same renderer-side enum rather than add a parallel one — see "Env-source switch".

`skyMode` (`SkyMode::Procedural`) stays the visible-sky selector and continues to sample
`envCube`; `atmosphere.enabled` is orthogonal — it only changes what *fills* `envCube`. With
`skyMode == Procedural` + `atmosphere.enabled`, the visible sky and the IBL are both the
atmosphere.

## Renderer API

Carry the atmosphere parameters into the bake. The renderer does not import `Saffron.Scene`, so
mirror the params onto a renderer-side struct, next to `SkygenParams`
(`renderer_types.cppm:721-726`):

```cpp
// New, beside SkygenParams. The renderer-side mirror of Scene's AtmosphereSettings.
struct AtmosphereParams
{
    bool enabled = false;
    f32 planetRadius = 6360.0f;
    f32 atmosphereHeight = 100.0f;
    glm::vec3 rayleighScattering{ 5.802f, 13.558f, 33.1f };
    f32 rayleighScaleHeight = 8.0f;
    f32 mieScattering = 3.996f;
    f32 mieScaleHeight = 1.2f;
    f32 mieAnisotropy = 0.8f;
    glm::vec3 ozoneAbsorption{ 0.650f, 1.881f, 0.085f };
    f32 sunDiskAngularRadius = 0.00465f;
    f32 sunDiskIntensity = 20.0f;
};
```

Add `AtmosphereParams atmosphere;` to `SkygenParams` (so the existing re-bake plumbing carries
it through unchanged — `pendingParams`/`bakedParams` already round-trip a whole `SkygenParams`).
Extend `requestSkyBake`'s exact-compare (`renderer_lighting.cpp:194-196`) to also re-bake when any
`atmosphere` field changes (memberwise compare; `AtmosphereParams` is a plain aggregate). This is
the only new trigger.

Add the LUT images to the `Ibl` struct (`renderer_types.cppm:732-747`), beside `envCube`:

```cpp
Image transmittanceLut;     // 256x64 rgba16f  (mu = view-zenith cos, r = altitude)
Image multiScatterLut;      //  32x32  rgba16f
Image skyViewLut;           // 192x108 rgba16f (azimuth x elevation, non-linear up)
Image3D aerialPerspectiveLut;  // 32x32x32 rgba16f (later-target; allocate when used)
```

These are small, persistent (allocated on `firstBake`, reused on re-bake exactly like
`envCube`), in `ShaderReadOnlyOptimal` after the bake. Add sizing constants beside the IBL ones
(`renderer_detail.cppm:1084-1089`): `AtmosTransmittanceW/H`, `AtmosMultiScatterSize`,
`AtmosSkyViewW/H`, `AtmosAerialSize`.

No new public renderer entry point is needed — the bake is reached through the existing
`bakeEnvironment` call sites (init `renderer.cppm:275`; re-bake `renderer.cppm:669`). `renderScene`
copies `scene.environment.atmosphere` into `skyBake.atmosphere` next to the existing sun fields
(`assets.cppm:622-626`).

## Shader Approach

Four new compute shaders under `editor/assets/shaders/` (each a self-contained module per the
repo convention, helpers inlined like `ibl_skygen.slang`). All target the Hillaire 2020 LUT
chain. The first three are the first target; the fourth is a later increment.

1. **`atmos_transmittance.slang`** — `numthreads(8,8,1)`, dispatched over the 2D LUT
   (`AtmosTransmittanceW/H`). Output `[[vk::image_format("rgba16f")]] RWTexture2D<float4>` (set 0
   binding 0 = layoutA). For each texel maps `(u,v) → (cos view-zenith, altitude)`, ray-marches
   to the atmosphere top accumulating Rayleigh + Mie + ozone extinction, writes
   `exp(-opticalDepth)`. Push constant: `AtmosphereParams` (matching the renderer struct layout,
   `float4`-aligned) + `float4 sunDir`. No input texture.

2. **`atmos_multiscatter.slang`** — `numthreads(8,8,1)` over `AtmosMultiScatterSize²`. Reads the
   transmittance LUT (set 0 binding 0 sampler + binding 1 storage out = layoutB). Computes the
   isotropic 2nd-order-plus multiple-scattering term per `(cos sun-zenith, altitude)` by sampling
   a small sphere of directions. First-target precision: a coarse fixed-sample sphere (e.g.
   `SQRT_SAMPLES = 8`) — adequate on llvmpipe; **higher sample counts / Hillaire's exact infinite
   series are a later-precision increment, noted below**.

3. **`atmos_skyview.slang`** — `numthreads(8,8,1)` over `AtmosSkyViewW × AtmosSkyViewH`. Reads
   transmittance + multi-scatter (layout with two samplers + one storage out). Ray-marches the
   in-scattering for each `(azimuth, elevation)` with a horizon-densified elevation mapping (more
   resolution near the horizon), applying Rayleigh + HG-phase Mie + transmittance + the
   multiscatter term. Push: `AtmosphereParams` + `float4 sunDir` + camera altitude.

4. **`atmos_skygen.slang`** — the replacement for `ibl_skygen` when `atmosphere.enabled`. Same
   output contract as `ibl_skygen` (storage cube, layoutA, `SkyParams`-shaped push): for each
   cube texel it computes the world dir via the same `cubeFaceDir` helper, samples the sky-view
   LUT by `(azimuth, elevation)` of that dir, and adds the sun disk (`smoothstep` on the angle to
   `sunDir` against `sunDiskAngularRadius`, scaled by `sunDiskIntensity` + transmittance toward
   the sun). Writes `float4(skyRadiance, 1.0)` into `outCube[tid]`. This keeps `envCube` the
   single coherent source for both the visible sky and the IBL convolutions.

5. **`atmos_aerial.slang`** *(later target)* — `numthreads(4,4,4)` over the 32³ aerial-perspective
   volume (`RWTexture3D<float4>`), storing per-froxel in-scattering + transmittance along view
   rays in view space (exponential Z slices like the froxel grid, `renderer_detail.cppm:1091`).
   Sampled by a post-scene composite pass (Render Graph Placement below).

A reference `numthreads(8,8,1)` storage-image dispatch is `ibl_skygen.slang:57-71`; the cube
write idiom (`outCube[tid] = ...`) carries over directly.

## Render Graph Placement

The LUT + sky-gen dispatches live **inside `bakeEnvironment`** (the one-shot transient command
buffer at `renderer_detail.cppm:3297-3550`), not in the per-frame graph — they only run on a
sun/parameter change. Ordering inside the bake, replacing the single `skygen` dispatch
(`:3489-3501`):

1. transmittance LUT → `General`, dispatch, → `ShaderReadOnlyOptimal`.
2. multi-scatter LUT (reads transmittance) → dispatch → read-only.
3. sky-view LUT (reads transmittance + multi-scatter) → dispatch → read-only.
4. `atmos_skygen` into `envCube` (reads sky-view) — replacing the gradient `skygen` when
   `sky.atmosphere.enabled`, else keep the current `ibl_skygen` dispatch.
5. existing irradiance → prefilter → BRDF chain (`:3503-3541`), unchanged — it consumes the
   atmosphere-filled `envCube` identically.

Use the same `barrier(...)` lambda already defined in the bake (`:3485-3486`) and the `group`
helper (`:3487`). Build the LUT pipelines with `newComputePipeline` beside `skygenP`
(`:3374-3381`), gated by `sky.atmosphere.enabled` so a gradient bake skips them. Allocate the LUT
`Image`s under the `firstBake` block (`:3302-3317`); reuse on re-bake.

**Aerial perspective (later target)** is the one piece that *is* a per-frame graph pass, because
it composites onto scene color using the frame's depth. It goes **after the scene pass and before
tonemap** — the same band the post-process demonstrator + FXAA/TAA already occupy (scene pass
`addPass` at `renderer.cppm:1323`; FXAA/TAA/tonemap follow). A Compute pass reading
`aerialPerspectiveLut` (`SampledReadCompute`) + the depth target + scene color
(`StorageImageRWCompute` on `viewportColorResource()`), blending `scene*transmittance +
inscatter`. Use the `light_cull` compute-pass shape (`renderer.cppm:803-815`) with declared
`RgAccess`/`RgUsage` so the graph derives the barriers (`render_graph.cppm:23-33`,
`70-79`). It can also be authored from a layer via `onRenderGraph(frameGraph())`
(`app.cppm:14-22,162-170`) using `viewportColorResource()` (`renderer_types.cppm:995-998`) if we
prefer not to touch `beginFrameGraph`.

## Env-Source Switch (shared with phase-5)

Phase-5 (equirect IBL) introduces an env-source mode in `bakeEnvironment` (Procedural-gradient vs
Equirect). This phase adds a third source (Atmosphere). To avoid two parallel switches, the bake
should select the cube-fill dispatch from one renderer-side enum:

```cpp
// New, in Ibl or beside SkygenParams. Selects what fills envCube during a bake.
enum class EnvSource { ProceduralGradient, Equirect, Atmosphere };
```

`bakeEnvironment` branches on it at the cube-fill step (`renderer_detail.cppm:3489-3501`):
gradient `ibl_skygen` (today), equirect prepass (phase-5), or the `atmos_*` LUT chain +
`atmos_skygen` (this phase). The irradiance/prefilter/BRDF tail is source-agnostic. Resolution
order when both could apply: a user equirect panorama (phase-5) wins over Atmosphere wins over the
gradient — resolved in `renderScene` when it fills `skyBake` (`assets.cppm:622-626`), and any
change re-flags `rebakePending` through the existing exact-compare. If phase-5 has not landed yet,
introduce the enum here with just `{ ProceduralGradient, Atmosphere }` and leave `Equirect` for
phase-5 to add.

## Asset Loading

None. The atmosphere is fully procedural — no textures, no catalog entries. (Phase-5 handles HDR
panorama loading; this phase is independent of it.)

## Serialization

Extend the existing environment block (`scene.cppm`):

- `environmentToJson` (`:381-395`) writes a nested `"atmosphere"` object (the bool + floats; reuse
  `vec3ToJson` for the three `vec3`s — `:339-347`).
- `environmentFromJson` (`:399-417`) reads it with full defaults when the `"atmosphere"` key is
  absent (so every existing v2 scene migrates with `enabled = false` → current behavior). Because
  the block is purely additive with defaults, **no `SceneVersion` bump is required** (it stays 2;
  `:424`) — `environmentFromJson` already tolerates missing fields, matching the phase-1 migration
  philosophy. If a future phase makes any atmosphere field *required*, bump to 3 then.

## Control + Editor

- **`se set-atmosphere`** (new) in `registerSceneCommands`
  (`engine/source/saffron/control/control_commands_scene.cpp:21`), modeled on `set-environment`
  (`:373-397`): merge/overlay named flags (`enabled`, `rayleighScaleHeight`, `mieAnisotropy`,
  `sunDiskIntensity`, …) or a `--json` blob over `environmentToJson(...).atmosphere`, then
  `environmentFromJson` back so unspecified fields are preserved. `get-environment` (`:364-368`)
  already returns the whole environment, so the atmosphere block surfaces there once serialized —
  no separate getter needed. Register via `registerCommand` (`control.cppm:50-51`,
  `control_server.cpp:32-38`); the `se` CLI (`tools/se/source/main.cpp`) needs no change (it
  forwards arbitrary flags via `buildParams`, `:72-110`).
- **Editor panel**: extend `environmentPanel` (`editor_panels.cpp:191-224`) with an `Atmosphere`
  collapsing header below the ambient controls — a `Checkbox("Enabled", &env.atmosphere.enabled)`
  plus `DragFloat`s for the scale heights, Mie anisotropy, and sun-disk intensity, and
  `ColorEdit3`/`DragFloat3` for the Rayleigh/ozone coefficients. Editing any field flows through
  `renderScene → requestSkyBake` and re-bakes next frame. The panel is already docked
  (`ui.cppm:574`); no layout change.

## Docs

Update `docs/content/` per the docs convention: extend the environment/skybox explanation page
(and its hub `_index.md` row) to describe the atmosphere LUT chain as a richer `envCube` source
that re-bakes with the sun and feeds the same IBL + visible-sky path. Math (LUT mappings, HG
phase) renders via KaTeX. Keep it an explanation page (Diátaxis), not a roadmap.

## Implementation Steps

1. Add `AtmosphereSettings` to `scene.cppm` beside `SceneEnvironment` (`:211`) and embed it as
   `SceneEnvironment::atmosphere`. Serialize it in `environmentToJson`/`environmentFromJson`
   (`:381-417`) with full defaults; no `SceneVersion` bump.
2. Add `AtmosphereParams` beside `SkygenParams` (`renderer_types.cppm:721`) and a field on
   `SkygenParams`; add the four LUT `Image`/`Image3D` handles to `Ibl` (`:732-747`); add the
   `EnvSource` enum (`{ ProceduralGradient, Atmosphere }`, extend for phase-5 later); add LUT
   sizing constants beside the IBL ones (`renderer_detail.cppm:1084`).
3. Write `atmos_transmittance.slang`, `atmos_multiscatter.slang`, `atmos_skyview.slang`,
   `atmos_skygen.slang` under `editor/assets/shaders/` (first target). Reconfigure
   (`cmake --preset debug`) so the glob picks them up (`CompileShaders.cmake:7`).
4. In `bakeEnvironment` (`renderer_detail.cppm:3297`): allocate the LUT images under `firstBake`
   (`:3302-3317`); build the LUT pipelines beside `skygenP` (`:3374-3381`), gated on
   `sky.atmosphere.enabled`; replace the single skygen dispatch (`:3489-3501`) with the
   source-switched chain (transmittance → multi-scatter → sky-view → `atmos_skygen`, else the
   existing `ibl_skygen`). Reuse the bake's `barrier`/`group` helpers (`:3485-3487`). Leave the
   irradiance/prefilter/BRDF tail untouched.
5. Extend `requestSkyBake` (`renderer_lighting.cpp:189-200`) to re-flag `rebakePending` on any
   `atmosphere` field change; copy `scene.environment.atmosphere → skyBake.atmosphere` in
   `renderScene` (`assets.cppm:622-626`).
6. *(First-target verification target met here.)* **Later increment:** let `sky.slang` mode 2
   optionally sample the sky-view LUT directly (set 1 second binding) for a higher-fidelity
   horizon than the 128² `envCube` round-trip; gate it so mode 2 still works without the LUT.
7. **Later increment:** add `atmos_aerial.slang` + the 32³ `aerialPerspectiveLut`, baked after
   sky-view, plus a per-frame post-scene compute composite pass (after `renderer.cppm:1323`,
   before tonemap) reading the volume + depth, declared via `RgAccess`/`RgUsage` so barriers
   derive automatically. Can be authored from a layer through `onRenderGraph` if `beginFrameGraph`
   is to stay untouched.
8. **Later increment:** raise multi-scatter sample count / move to Hillaire's exact series once on
   hardware (the coarse first-target sphere is fine for correctness on llvmpipe).
9. Add `se set-atmosphere` (`control_commands_scene.cpp:21`, modeled on `set-environment` at
   `:373`) and the `Atmosphere` editor section in `environmentPanel`
   (`editor_panels.cpp:191-224`). Update `docs/content/`.

## Dependencies

- **Phases 1–3 (shipped):** `SceneEnvironment` + serialization + env panel + `se set-environment`
  (phase 1); the visible sky pass sampling `envCube` (phase 2); the on-demand IBL re-bake that
  follows the directional light (phase 3). This phase reuses all three directly.
- **Complements phase-5:** both add an `envCube` source. They must share the single `EnvSource`
  switch in `bakeEnvironment` (this phase introduces it; phase-5 adds the `Equirect` case). Neither
  blocks the other.
- Independent of phase-4 (HDR import), phase-6 (reflection probes), phase-8 (clouds + ToD). When
  phase-5's DDGI sky-color routing lands, the atmosphere sky color should feed
  `renderer.ddgi.skyColor` (`renderer_types.cppm:866`, currently a default with no setter) through
  the same path so DDGI stays coherent with the baked atmosphere — note for phase-5.

## Verification

- Build only in the toolbox, `-j1`: `toolbox run -c saffron-build bash -lc 'cd
  /var/home/saffronjam/repos/SaffronEngine && cmake --preset debug && cmake --build build/debug
  -j1'` (the reconfigure compiles the new `.slang` files).
- Headless A/B against the gradient: run the editor frame-bounded with validation on
  (`VAL=0` clean), drive via the `se` CLI, capture the viewport, diff.
  ```sh
  SAFFRON_EXIT_AFTER_FRAMES=8 ./build/debug/bin/SaffronEditor &
  se set-environment --json '{"skyMode":"procedural"}'   # gradient baseline
  se screenshot viewport /tmp/sky_gradient.png
  se set-atmosphere --json '{"enabled":true}'            # re-bakes LUTs + envCube + IBL
  se screenshot viewport /tmp/sky_atmos.png
  ```
  Confirm `sky_atmos.png` differs substantially from `sky_gradient.png` (atmosphere replaces the
  flat gradient — expect a blue Rayleigh sky + brighter horizon) via a numpy/PIL diff:
  `python3 -c "from PIL import Image, ImageChops; import numpy as np;
  a=np.asarray(Image.open('/tmp/sky_gradient.png')); b=np.asarray(Image.open('/tmp/sky_atmos.png'));
  print((np.abs(a.astype(int)-b.astype(int))>4).mean())"` (expect a large changed fraction in the
  sky region).
- Sun coherence: `se set-directional-light` to move the sun and re-screenshot; the horizon glow +
  sun disk in the visible sky must move, and a lit matte sphere's ambient tint must shift the same
  way (proving the same atmosphere `envCube` feeds both the sky and the IBL). The re-bake is
  exact-compare gated, so an unchanged sun must NOT re-bake (watch the `ibl baked …` log line at
  `renderer_detail.cppm:3592` fires only on change).
- `se set-atmosphere --json '{"enabled":false}'` returns the gradient bit-for-bit (no LUT
  pipelines built that bake) — proves the source switch is a clean no-op when off.
- Save the project, reload it, `se get-environment`: the `atmosphere` block round-trips; an old v2
  scene with no `atmosphere` key loads with `enabled=false` (unchanged render).
- Validation-clean (`VAL=0`) across all paths, including the AA modes (the visible sky already
  renders through them) and the optional aerial-perspective composite when that increment lands.
