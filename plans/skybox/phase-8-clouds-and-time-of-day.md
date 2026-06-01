# Phase 8: Clouds And Time Of Day

**Status:** NOT STARTED

## Goal

The furthest-out tail of the skybox roadmap. Two loosely-coupled systems:

- **Scope A — Clouds.** A sky-coupled cloud layer, kept as a *separate* system from the
  skybox (not a `SkyMode`, not the IBL `envCube`). Start with a cheap 2D procedural layer
  composited over the rendered scene; upgrade to a volumetric raymarch later. Lit by the sun
  direction + atmosphere transmittance, so this **depends on phase 7's** transmittance /
  sky-view LUTs.
- **Scope B — Time of day.** A scene-level system / editor tool (explicitly **not** baked into
  the renderer) that animates the directional light (rotation / intensity / color), atmosphere
  parameters, and exposure, and triggers the **on-demand IBL re-bake** (phase 3 machinery,
  `requestSkyBake`) plus the **DDGI sky update** (phase 5). ToD drives data the existing
  systems already consume; it adds almost no renderer code.

This phase is the long-horizon end of the plan. Several decisions are still open (see *Open
Questions*); ship the cheap, grounded slices first (2D clouds, an editor ToD animator) and
leave the expensive slices (volumetric clouds, a serialized runtime ToD component) explicitly
deferred.

## Dependencies

- **Phase 7 (procedural atmosphere)** is a hard prerequisite for cloud *lighting*: clouds read
  the transmittance LUT (sun extinction toward the cloud) + sky-view LUT (ambient/scattered
  sky color). Until phase 7 lands, a degraded cloud build can fall back to the procedural
  `SkygenParams` sun (`renderer_types.cppm:721-726`) — usable but not energy-coherent.
- **Phase 3 (on-demand IBL re-bake)** — ToD reuses `requestSkyBake` (`renderer_lighting.cpp:189-200`)
  and its consumption in `beginFrameGraph` (`renderer.cppm:667-678`). Already shipped; ToD only
  has to write the `DirectionalLightComponent` each frame and the existing dirty-compare re-bakes.
- **Phase 5 (equirect IBL + DDGI sky)** — ToD's atmosphere/sun changes must also refresh the
  DDGI sky inputs. Today `Ddgi.sunDir/sunColor/sunIntensity` are routed via `setDdgiScene`
  (`renderer_types.cppm:1130-1133`) and `Ddgi.skyColor` (`renderer_types.cppm:866`) has **no
  setter** (phase 5 adds the routing). ToD piggybacks on whatever phase 5 establishes.

Do not start this phase before phase 7 (for clouds) and at least phase 3 (for ToD); the ToD
slice alone is implementable as soon as phase 3 is done.

## Post-lighting reality / Current Engine Fit

Grounded anchors (verified against the current tree):

- **The visible sky and the IBL share one source.** `Scene.environment` (`scene.cppm:211-223`)
  drives a fullscreen sky pass added before the scene pass (`renderer.cppm:1242-1257`) and the
  procedural skygen bake. Clouds are a *third* thing layered on top of the rendered scene — not
  a sky mode and not the IBL cube.
- **The on-demand re-bake machinery exists.** `requestSkyBake` flags `ibl.rebakePending` only
  when sun inputs differ (`renderer_lighting.cpp:189-200`); `beginFrameGraph` consumes it at a
  GPU-idle point (`renderer.cppm:667-678`); `SkygenParams` is `{sunDir, sunIntensity, sunColor}`
  (`renderer_types.cppm:721-726`). `renderScene` already calls `requestSkyBake` from the first
  `DirectionalLightComponent` (`assets.cppm:618-627`). ToD changes the light; the existing
  compare re-bakes. **Reuse this; build nothing new for the re-bake path.**
- **`renderScene` reads only the first directional light** into locals `lightDir/lightColor/lightIntensity/lightAmbient`
  (`assets.cppm:418-434`). A ToD system that mutates that one `DirectionalLightComponent` in
  place before `renderScene` runs (the editor calls `renderScene` at `editor_app.cppm:305`)
  needs no renderer change — sun, sky bake, DDGI sun, and contact-shadow direction all follow.
- **Exposure is a renderer EV value, not the scene field.** `renderer.exposureEv`
  (`renderer_types.cppm:975`) is applied as `exp2(exposureEv)` in the tonemap pass
  (`renderer.cppm:1447-1449`); `setExposure`/`exposureEv` live at `renderer.cppm:1609-1616`; the
  `se set-exposure` command is at `control_commands_render.cpp:263`. The scene-side
  `SceneEnvironment.exposure` (`scene.cppm:218`) is **serialized but reserved/unused** (its
  comment says so). ToD should *either* start writing `setExposure(...)` from
  `environment.exposure` (finally wiring the reserved field) *or* drive `setExposure` directly —
  decide in *Open Questions*.
- **The DDGI struct already carries sun + sky color** (`renderer_types.cppm:847-866`); the trace
  push constants ship `sunDir`/`sunColor`/`sunIntensity` (set by `setDdgiScene`,
  `renderer_types.cppm:1130-1133`). `skyColor` has only a struct default and no setter (phase 5).
- **The render graph derives all barriers from declared usage.** `RgUsage`
  (`render_graph.cppm:23-33`), `RgPass`/`RgAttachment` (`render_graph.cppm:58-79`),
  import/add/execute API (`render_graph.cppm:105-126`). A cloud composite pass declares its
  usages and the barriers are free.
- **The composite ordering anchor.** Scene color lives in `renderer.graph.sceneColor` (the
  offscreen, `renderer.cppm:699`). The scene pass ends at `addPass(graph, std::move(scene))`
  (`renderer.cppm:1323`); FXAA (`:1327`), TAA (`:1348`), and SSGI-history (`:1390`) resolve into
  `sceneColor`; the mandatory tonemap pass is added **last** by `addTonemapPass`
  (`renderer.cppm:1423`, before app `onRenderGraph` hooks + UI). The cloud composite must run
  **after the AA resolve produces final-but-linear `sceneColor`** and **before tonemap** — i.e.
  it is added inside `beginFrameGraph` between the AA/SSGI-history block and `addTonemapPass`.
- **App-authored passes are possible but the wrong fit here.** `Layer.onRenderGraph`
  (`app.cppm:21`) runs *after* `beginFrameGraph` and *after* `addTonemapPass`, so an app pass
  cannot inject a composite *before* tonemap. Clouds therefore live in the engine
  `beginFrameGraph`, not in the editor layer.
- **Cube/render-to-face infra exists** if clouds ever need a cloud-shadow cubemap:
  `newCubeImage` (`renderer_detail.cppm:703-760`), `newColorCubeImage` (`renderer_detail.cppm:762-830`).
  Not needed for the 2D layer.

What this phase genuinely adds: a cloud simulation/shade compute pass + a composite graphics
pass, cloud scene state + serialization + controls, and a ToD scene system (an animator) that
writes existing component/renderer state. No new IBL, no new DDGI, no new sky pass.

---

## Scope A — Clouds

### Data Model

Clouds are global frame state like the sky, not a placed entity (no transform, no picking). Add
a nested block on `SceneEnvironment` (`scene.cppm:211-223`), beside the existing fields, rather
than a component:

```cpp
// NEW, in scene.cppm beside SceneEnvironment (scene.cppm:211-223)
enum class CloudMode
{
    Off,
    Layer2D,      // cheap procedural 2D layer (ship first)
    Volumetric,   // raymarched volume (deferred)
};

struct CloudSettings
{
    CloudMode mode = CloudMode::Off;
    f32 coverage = 0.4f;        // 0 clear .. 1 overcast
    f32 density = 1.0f;         // optical thickness scale
    f32 altitude = 2000.0f;     // layer base height (world units)
    f32 thickness = 600.0f;     // volumetric only
    f32 windSpeed = 0.02f;      // uv scroll / advection rate
    glm::vec2 windDir{ 1.0f, 0.0f };
    f32 scatter = 1.0f;         // forward-scatter / silver-lining strength
    glm::vec3 tint{ 1.0f };     // artistic albedo tint
};
```

Add `CloudSettings clouds;` to `SceneEnvironment`. Keep it on the environment block (not a new
top-level scene key) so it round-trips through the existing `environmentToJson` /
`environmentFromJson`.

### Renderer API

New renderer-facing mirror beside `SkyRenderSettings` (`renderer_types.cppm:749+`, the visible-sky
struct that `submitSky` consumes):

```cpp
// NEW, in renderer_types.cppm beside SkyRenderSettings / submitSky
struct CloudRenderSettings
{
    CloudMode mode = CloudMode::Off;
    f32 coverage = 0.4f;
    f32 density = 1.0f;
    f32 altitude = 2000.0f;
    f32 thickness = 600.0f;
    f32 windOffset = 0.0f;      // accumulated scroll (advanced by dt on the renderer)
    glm::vec2 windDir{ 1.0f, 0.0f };
    f32 scatter = 1.0f;
    glm::vec3 tint{ 1.0f };
    glm::vec3 sunDir{ 0.5f, 1.0f, 0.3f };   // = SkygenParams.sunDir (TO the sun)
    glm::vec3 sunColor{ 1.0f };
    f32 sunIntensity = 1.0f;
};

void submitClouds(Renderer& renderer, const CloudRenderSettings& clouds);  // NEW
```

`submitClouds` stores the settings + a `cloudsReady`/`doClouds` flag on a new `Clouds` state
struct on `Renderer` (mirror the `Sky` struct that already carries pipeline + set + setLayout,
`renderer_types.cppm:749+`). `renderScene` resolves `scene.environment.clouds` + the same sun it
already derived for `requestSkyBake` (`assets.cppm:618-627`) into `CloudRenderSettings` and calls
`submitClouds`, right after the existing `submitSky` resolve (`assets.cppm:635-639`).

Implementation lives in **existing TUs to avoid CMakeLists edits**: `submitClouds` in
`renderer_lighting.cpp` (next to `submitSky`/`requestSkyBake`), the record helper `recordClouds`
in `renderer_drawlist.cpp` (next to `recordSky`), and pipeline creation in `renderer_detail.cppm`
(next to the sky/IBL bake code).

### Shader Approach

Two new `.slang` files in `editor/assets/shaders/` (the GLOB at `CompileShaders.cmake:7` picks
them up; **new shaders require a CMake reconfigure** because `CONFIGURE_DEPENDS` only re-globs at
configure time):

- **`cloud_layer.slang`** (ship first) — a 2D analytic/fBm layer. A compute pass writes a
  cloud RGBA scratch image (`rgb` = lit cloud radiance, `a` = coverage/alpha), or the composite
  graphics pass evaluates the layer inline (simpler; one pass). Reconstruct a world view ray
  from `inverse(viewProj)` exactly as `sky.slang` does (`recordSky` already pushes
  `inverse(sceneDrawList.viewProj)`); intersect the ray with a flat layer plane at `altitude`;
  sample animated fBm noise at the hit; shade with `sunDir`/`sunColor` modulated by phase-7's
  **transmittance LUT** (sun extinction from layer to space) and a Henyey-Greenstein phase term
  (`scatter`); ambient from the **sky-view LUT**. Alpha-over composite onto `sceneColor`. Output
  is **linear HDR** (the mandatory tonemap handles display — do not tonemap or gamma here).

```hlsl
// cloud_layer.slang (sketch; composite variant)
// push: inverse(viewProj), sunDir.xyz+intensity.w, sunColor, coverage/density/altitude/windOffset...
float3 rayDir = normalize(worldRayFromInvViewProj(uv));   // same trick as sky.slang
float t = (push.altitude - cameraHeight) / rayDir.y;       // flat-layer hit
if (t <= 0) { /* below/parallel: no cloud */ }
float n = fbm(hitXZ * scale + push.windDir * push.windOffset);
float cover = saturate(n - (1.0 - push.coverage));
float3 sunT = sampleTransmittanceLUT(push.sunDir, push.altitude);  // phase-7 LUT
float3 sky  = sampleSkyViewLUT(rayDir);                             // phase-7 LUT
float3 lit  = (push.sunColor * push.sunIntensity * sunT * hg(dot(rayDir, push.sunDir), push.scatter)
              + sky) * push.tint;
float alpha = cover * push.density;
sceneColor.rgb = lerp(sceneColor.rgb, lit, alpha);   // alpha-over, linear HDR
```

- **`cloud_volume.slang`** (deferred — volumetric) — a raymarch through a `[altitude,
  altitude+thickness]` slab: density from 3D noise (Worley + Perlin), per-step transmittance,
  in-scattering toward the sun using the transmittance LUT, beer-powder term. Reads scene depth
  (`renderer.targets.depth`) to early-out behind geometry. Heavy; gate behind `CloudMode::Volumetric`
  and ship only after the 2D layer is proven.

### Render Graph Placement

A single new **graphics composite pass** (RMW on `sceneColor` is fine as a fullscreen graphics
draw with `loadOp=Load`), or a **compute pass** (`StorageImageRWCompute` on `sceneColor`,
matching the FXAA/tonemap pattern at `renderer.cppm:1333/1441`). Compute is the cleaner fit (no
attachment, reads phase-7 LUTs via `SampledReadCompute`):

Insert in `beginFrameGraph` **after** the AA/SSGI-history block (after `renderer.cppm:1418`) and
**before** `addTonemapPass(renderer, graph)` (`renderer.cppm:1423`):

```cpp
// NEW in beginFrameGraph, between the AA/SSGI-history block and addTonemapPass (~renderer.cppm:1419)
if (doClouds)  // renderer.clouds.ready && settings.mode != CloudMode::Off
{
    RgPass cloudsPass;
    cloudsPass.name = "clouds";
    cloudsPass.kind = RgPassKind::Compute;
    cloudsPass.accesses = {
        RgAccess{ renderer.graph.sceneColor, RgUsage::StorageImageRWCompute },
        RgAccess{ transmittanceLutRes, RgUsage::SampledReadCompute },   // phase-7
        RgAccess{ skyViewLutRes, RgUsage::SampledReadCompute },         // phase-7
        // volumetric only: RgAccess{ depthRes, RgUsage::SampledReadCompute }
    };
    cloudsPass.execute = [&renderer](vk::CommandBuffer cmd) { recordClouds(renderer, cmd); };
    addPass(graph, std::move(cloudsPass));
}
```

Why here, citing the ordering anchor: clouds composite over the *resolved, anti-aliased* scene
(`sceneColor` after FXAA/TAA, `renderer.cppm:1327-1384`) so they are not double-filtered, and
**before** the in-place tonemap (`addTonemapPass`, `renderer.cppm:1423`) so they share exposure
and write linear HDR. The graph derives the compute→compute barrier automatically from `RgUsage`
(`render_graph.cppm:23-33`). Wind advection: advance `renderer.clouds.windOffset += windSpeed * dt`
on the renderer (dt is available where the frame is built), so the sim animates without ToD.

> **Decision deferred (Open Questions):** whether clouds should also affect *lighting* (cloud
> shadows on the ground, overcast dimming of the sun → an IBL re-bake input). The 2D composite is
> appearance-only; coupling to lighting is a later increment.

### Controls (`se`)

Extend the existing `set-environment` merge command (`control_commands_scene.cpp:373-397`) — it
already overlays named fields onto `environmentToJson`/`environmentFromJson`, so once
`CloudSettings` serializes inside the environment block, add the cloud fields to the overlay list
(`cloudMode`, `cloudCoverage`, `cloudDensity`, `cloudAltitude`, `cloudWindSpeed`, etc.). No new
command needed; `get-environment` (`control_commands_scene.cpp:364-368`) reports them for free.
Add a `Clouds` section to `environmentPanel` (`editor_panels.cpp:191-224`, called at
`editor_app.cppm:344`): a mode combo + coverage/density/altitude/wind drag floats + tint color.

### Serialization

`CloudSettings` rides inside the environment block. Extend `environmentToJson`
(`scene.cppm:381-395`) and `environmentFromJson` (`scene.cppm:399-417`) to emit/read a nested
`"clouds"` object (reuse `vec3ToJson`/`vec2` helpers and the `jsonF32Or`/`jsonStringOr` default
pattern). A new `cloudModeName`/`cloudModeFromName` pair mirrors `skyModeName`/`skyModeFromName`
(`scene.cppm:361-379`). Because `environmentFromJson` defaults every missing field, **no scene
version bump is required** for a purely additive optional block (a v2 scene without `"clouds"`
loads as `CloudMode::Off`). Bump `SceneVersion` (`scene.cppm:424`) only if a future cloud field
must be *required*.

---

## Scope B — Time Of Day

### Design choice: animator vs runtime component vs both

Three shapes, decide explicitly:

1. **Editor-only animator (recommended first slice).** A non-serialized editor state (a `time`
   float + play/pause + speed) living on `EditorContext` (`editor_context.cppm:39-86`), advanced
   in `layer.onUpdate(TimeSpan)` (`app.cppm:18`) or at the top of `layer.onUi` before
   `renderScene` (`editor_app.cppm:289-305`). Each tick it computes the sun and writes the scene's
   `DirectionalLightComponent` + `SceneEnvironment` in place. **No serialization, no scene
   version bump, no runtime cost in a shipped game.** Matches the old phase-4 note: *"a scene
   system or editor tool, not baked into the renderer."*
2. **Runtime `TimeOfDayComponent` (later, optional).** A serialized component (registered via
   `registerComponent` in `editor_components.cpp` beside `DirectionalLightComponent`,
   `editor_components.cpp:202-225`) so a scene can carry a designed time + auto-advance at
   runtime. Needs a per-frame `updateTimeOfDay` system call and a scene version bump.
3. **Both.** The component stores the authored time; the editor animator scrubs it. Reasonable
   end state, but build (1) first and only add (2) when a non-editor runtime needs ToD.

Ship (1). Treat (2)/(3) as deferred (Open Questions).

### Data Model

```cpp
// NEW — editor animator state (non-serialized), on EditorContext (editor_context.cppm:39-86)
struct TimeOfDay
{
    f32 time = 12.0f;        // hours 0..24
    f32 speed = 1.0f;        // hours per real second when playing
    bool playing = false;
    f32 latitude = 45.0f;    // for the sun-elevation model (optional)
    glm::vec3 dayColor{ 1.0f, 0.96f, 0.9f };
    glm::vec3 duskColor{ 1.0f, 0.5f, 0.25f };
    glm::vec3 nightColor{ 0.1f, 0.12f, 0.2f };
    f32 noonIntensity = 4.0f;
};
```

If/when the runtime variant (2) is built, a parallel `TimeOfDayComponent` (the serialized
subset: `time`, `speed`, `playing`) goes in `scene.cppm` beside the light components
(`scene.cppm:76-103`) and registers in `editor_components.cpp`.

### System (the per-frame update)

A new free function `updateTimeOfDay(EditorContext&, TimeSpan dt)` (the cleanest home is a small
new section in an existing editor TU, e.g. alongside `updateEditorCamera`; do **not** add a new
module/TU). Called from `layer.onUpdate` or the top of `layer.onUi` *before*
`renderScene(...)` (`editor_app.cppm:305`) so the same frame renders with the updated sun. What
it writes, all into existing state:

1. **Advance** `time += playing ? speed * dt : 0`, wrap to `[0,24)`.
2. **Sun direction** from `time` (and optional `latitude`): map hour → elevation/azimuth →
   direction; write the first `DirectionalLightComponent.direction` (`scene.cppm:78`). Because
   `renderScene` reads that one light (`assets.cppm:418-434`) and calls `requestSkyBake` from it
   (`assets.cppm:618-627`), the **visible sky + IBL re-tint together via the existing on-demand
   re-bake** (`renderer.cppm:667-678`) — ToD writes nothing renderer-side for the sky/IBL.
3. **Sun color + intensity** from `time` (day/dusk/night gradient): write
   `DirectionalLightComponent.color` (`scene.cppm:79`) + `.intensity` (`scene.cppm:80`).
4. **Atmosphere parameters** (when phase 7 lands): write the `AtmosphereSettings` that phase 7
   adds to `SceneEnvironment` (sun-disk/turbidity vary with elevation) so the sky LUTs rebuild.
5. **Exposure**: drive `setExposure(renderer, ev)` (`renderer.cppm:1609`) from a ToD curve
   (brighter night EV). Reuse `SceneEnvironment.exposure` (`scene.cppm:218`, currently
   reserved) as the value ToD writes — i.e. ToD finally wires the reserved field by calling
   `setExposure(renderer, log2(environment.exposure))` once per frame. (Decide the units in Open
   Questions — `exposure` is a linear multiplier today; `exposureEv` is stops.)
6. **DDGI sky** (phase 5): ToD's sun/sky changes flow through whatever phase 5 sets — today the
   sun already routes via `setDdgiScene` (`renderer_types.cppm:1130-1133`) from `renderScene`'s
   `lightDir/lightColor/lightIntensity`, so a ToD-moved sun already reaches DDGI; the sky-color
   routing is phase 5's `Ddgi.skyColor` setter (`renderer_types.cppm:866`).

The re-bake guard already prevents churn: `requestSkyBake` only flags a re-bake when the sun
inputs actually change (`renderer_lighting.cpp:194-198`). While ToD is *playing*, the sun changes
every frame, so it re-bakes every frame (a `waitIdle` + a handful of dispatches each frame). That
is acceptable for editor scrubbing but is the main perf concern (Open Questions: throttle the
re-bake to N ms or to angular deltas while animating).

### Controls (`se`)

Add `set-time-of-day` to `registerSceneCommands` (`control_commands_scene.cpp:21`, beside
`set-environment` at `:373`):

```text
se set-time-of-day {hours}            # set time, recompute sun once
se set-time-of-day --play 1 --speed 2 # start the animator
```

It writes the editor `TimeOfDay` state (and triggers one `updateTimeOfDay`). Reuse the
`positionalOr` + named-flag overlay pattern from `set-environment`
(`control_commands_scene.cpp:377-396`). The `EngineContext` already exposes `editor` + `renderer`
(used by `set-environment` / `set-exposure`), so the command reaches both. Add an `se`
text formatter in `tools/se/source/main.cpp` only if a friendlier print is wanted (optional).

Editor UI: a ToD widget in `environmentPanel` (`editor_panels.cpp:191-224`) — a `time` slider
(0–24), play/pause button, speed drag — or a dedicated panel docked beside `Environment`
(`ui.cppm:558-577` seeds the default dock layout; add a `DockBuilderDockWindow` row there if a
new panel is introduced).

### Serialization

The editor-only animator (slice 1) is **not serialized** (transient editor state, like the
fly-camera). The day/dusk/night color curve constants can live as editor defaults. Only the
runtime `TimeOfDayComponent` (slice 2, deferred) serializes — through `registerComponent`'s
auto `toJson`/`fromJson` (`scene.cppm:426-489`), needing a `SceneVersion` bump
(`scene.cppm:424`) when added.

---

## Implementation Steps

Clouds (after phase 7):

1. Add `CloudMode` + `CloudSettings` to `scene.cppm` (beside `SceneEnvironment`,
   `scene.cppm:201-223`); nest `clouds` in `environmentToJson`/`environmentFromJson`
   (`scene.cppm:381-417`); add `cloudModeName`/`cloudModeFromName` (mirror `scene.cppm:361-379`).
2. Add `CloudRenderSettings` + `submitClouds` (`renderer_types.cppm` beside `SkyRenderSettings`);
   add a `Clouds` renderer state struct (mirror `Sky`).
3. Add `cloud_layer.slang` to `editor/assets/shaders/`; **reconfigure CMake** (the GLOB,
   `CompileShaders.cmake:7`). Build the cloud pipeline in `renderer_detail.cppm`.
4. Implement `submitClouds` in `renderer_lighting.cpp` and `recordClouds` in
   `renderer_drawlist.cpp` (reuse the `recordSky` inverse-viewProj push pattern). Advance
   `windOffset` by dt on the renderer.
5. Resolve `scene.environment.clouds` + sun in `renderScene` (`assets.cppm:635-639`, after the
   sky resolve) → `submitClouds`.
6. Insert the `clouds` compute pass in `beginFrameGraph` after the AA/SSGI-history block
   (`renderer.cppm:1418`) and before `addTonemapPass` (`renderer.cppm:1423`); declare
   `StorageImageRWCompute` on `sceneColor` + `SampledReadCompute` on phase-7 LUTs.
7. Extend `set-environment` overlay + `environmentPanel` with cloud fields
   (`control_commands_scene.cpp:385-394`, `editor_panels.cpp:191-224`). Update `docs/`.
8. (Deferred) `cloud_volume.slang` raymarch + `CloudMode::Volumetric`; reads `targets.depth`.

Time of day (after phase 3; atmosphere/DDGI bits land with phases 7/5):

1. Add the `TimeOfDay` animator struct to `EditorContext` (`editor_context.cppm:39-86`).
2. Add `updateTimeOfDay(EditorContext&, TimeSpan)` in an existing editor TU (beside
   `updateEditorCamera`); call it before `renderScene` (`editor_app.cppm:305`).
3. Compute sun dir/color/intensity from `time`; write the first `DirectionalLightComponent`
   (`scene.cppm:76-82`) in place — the existing `requestSkyBake` path re-tints sky + IBL.
4. Drive exposure via `setExposure` (`renderer.cppm:1609`) from the ToD curve; wire
   `SceneEnvironment.exposure` (`scene.cppm:218`) as the source (decide units).
5. Add `set-time-of-day` to `registerSceneCommands` (`control_commands_scene.cpp:21`) + a ToD
   widget in `environmentPanel`. Update `docs/`.
6. (Deferred) Throttle the re-bake while animating. (Deferred) Add a serialized
   `TimeOfDayComponent` + `registerComponent` + `SceneVersion` bump for the runtime variant.

## Verification

Build only in the toolbox, `-j1` (`toolbox run -c saffron-build bash -lc 'cmake --build build/debug -j1'`);
reconfigure (`cmake --preset debug`) after adding `.slang` files. All headless runs:
`SAFFRON_EXIT_AFTER_FRAMES=N ./build/debug/bin/SaffronEditor`. Validation must stay clean
(`VAL=0`).

Clouds:

- `se set-environment --json '{"cloudMode":"layer2d","cloudCoverage":0.7}'` then
  `se screenshot viewport clouds.png`; an overcast scene shows a visibly cloudy sky band. `se
  get-environment` round-trips the cloud fields.
- A/B coverage: `cloudCoverage 0.1` vs `0.9` screenshots; PIL/numpy diff of the upper sky region
  shows a large changed-pixel fraction (more sky covered), while the lower-screen geometry pixels
  stay near-identical (clouds composite over sky, not over near geometry that occludes the layer).
- `cloudMode off` reproduces the pre-cloud sky pixel-identically (composite is a no-op).
- With wind > 0, two screenshots a few frames apart differ in the cloud band (advection), proving
  `windOffset` animates; with `windSpeed 0` they match.
- Save → reload `project.json`: cloud fields round-trip; a v2 scene with no `"clouds"` block
  loads as `Off` (additive, no version bump).

Time of day:

- `se set-time-of-day 6` (dawn) vs `set-time-of-day 12` (noon) vs `18` (dusk): screenshots show
  the sun moving (sky gradient + lit-side of meshes shift) and warm→neutral→warm sun color. The
  IBL re-tints with the sky (a shadowed/ambient region changes color between times), confirming
  the on-demand re-bake fired (one re-bake per time change; steady time never re-bakes — check
  via a log or by confirming no per-frame stall at fixed time).
- Exposure curve: night vs noon screenshots show the ToD-driven exposure change (or assert
  `se` round-trips the exposure value into `exposureEv`).
- `--play 1`: across `SAFFRON_EXIT_AFTER_FRAMES=30`, the sun advances frame-to-frame (compare
  early vs late capture). Confirm the editor still exits cleanly (code 0) — no teardown leak.
- DDGI on (`se set-ddgi 1`) + a ToD sun move: the indirect term tracks the sun (the existing
  `setDdgiScene` sun routing), and once phase 5 lands, the DDGI sky color tracks the sky too.

## Open Questions

- **Clouds: 2D layer vs volumetric first** — ship the 2D layer; volumetric is a known-heavy
  follow-up. Is a flat-plane 2D layer convincing enough, or is a thin slab raymarch
  (mid-complexity) the better first target?
- **Cloud lighting coupling** — appearance-only (composite) first. Do clouds eventually cast
  ground shadows / dim the sun for lighting (a re-bake input, or a sun-transmittance multiplier
  feeding `DirectionalLightComponent.intensity`)? That couples clouds back into the IBL/DDGI path.
- **Cloud pass: compute vs graphics** — compute RMW on `sceneColor` (matches FXAA/tonemap) vs a
  fullscreen graphics composite with `loadOp=Load`. Compute is recommended; confirm phase-7 LUTs
  are sampleable from a compute pass (they should be, via `SampledReadCompute`).
- **ToD shape** — editor animator only (slice 1) vs serialized `TimeOfDayComponent` (slice 2) vs
  both (slice 3). Ship slice 1; gate slice 2 on a non-editor runtime needing ToD.
- **Re-bake throttling while animating** — re-baking every frame during ToD playback is a
  `waitIdle` per frame. Throttle to an angular delta or a wall-clock interval, or make the bake
  async (it currently stalls on `waitIdle`, `renderer.cppm:667-678`). Quantify the cost on
  hardware (llvmpipe will be slow regardless).
- **Exposure source + units** — `SceneEnvironment.exposure` is a reserved *linear multiplier*
  (`scene.cppm:218`); `renderer.exposureEv` is *stops* (`renderer.cppm:1447`). Either redefine
  the scene field as EV (cleaner, possible scene-version bump) or have ToD convert
  (`log2(exposure)`). Decide before wiring step B-4.
- **Sun model fidelity** — a simple hour→elevation curve vs a real solar-position model
  (latitude/day-of-year). Start simple; the `latitude` field reserves the upgrade.
- **Where `updateTimeOfDay` runs** — `layer.onUpdate(TimeSpan)` (`app.cppm:18`, runs before UI)
  vs top of `layer.onUi` (`editor_app.cppm:289`, where the editor camera/dt is fresh). `onUpdate`
  is the conceptually correct home for a scene system; confirm dt + scene access are both
  available there.
- **DDGI sky-color routing** — owned by phase 5 (`Ddgi.skyColor` has no setter today,
  `renderer_types.cppm:866`). ToD assumes phase 5's setter exists; if phase 5 slips, ToD's sky
  color does not reach DDGI (the sun still does, via `setDdgiScene`).
