# Phase 6: Reflection Probes

**Status:** NOT STARTED

## Goal

Add a spatial `ReflectionProbeComponent` — an entity component (it owns a transform plus
an influence volume) that captures a *local* cubemap of its surroundings, prefilters that
cube exactly like the global environment, and is sampled by nearby meshes for specular
ambient. The existing global IBL (`prefilteredCube`/`irradianceCube`) is the **fallback
probe**: a mesh outside every probe's influence keeps sampling set 3 unchanged. Capture is
**on demand** (on placement/edit), never per frame, reusing the phase-3 "dirty + GPU-idle
re-bake" discipline.

This phase is the spatial counterpart to the shipped IBL. It does not need phase 4 (HDR
import) or phase 5 (equirect IBL); those only change the *global* env source, which probes
inherit as their fallback. Prerequisite: the shipped IBL chain (phases 1–3 of `plans/lighting/`
+ phase 2 of this folder).

## Post-lighting reality (what already exists — reuse it)

- **A full split-sum IBL bake already ships and is the template for probe prefiltering.**
  `bakeEnvironment` (`renderer_detail.cppm:3297`) allocates `envCube`/`irradianceCube`/
  `prefilteredCube`/`brdfLut` on `firstBake` (`:3302-3317`), dispatches `ibl_skygen`
  (`:3493-3498`) → `ibl_irradiance` (`:3504-3512`) → `ibl_prefilter` per mip (`:3514-3530`)
  → `ibl_brdf` (`:3532-3541`), then writes the persistent mesh set 3 + sky set 1 on
  `firstBake` only (`:3559-3588`). The convolution shaders read `envCube` as an opaque
  `SamplerCube` (`ibl_irradiance.slang`, `ibl_prefilter.slang`), so **they work on any cube
  regardless of how it was filled.** A probe reuses `ibl_irradiance`/`ibl_prefilter`
  verbatim, only swapping the source cube.
- **Cube image creation is a solved primitive.** `newCubeImage`
  (`renderer_detail.cppm:706`) makes a 6-layer sampled+storage cube (default `eCube` view,
  per-mip 2D-array storage views for compute) — used for every IBL cube. `newColorCubeImage`
  (`:765`) makes a color-attachable cube with the 6 single-layer face attachment views, and
  `pointShadowFaceMatrices` (`:833`) returns the 6 face world→clip matrices (fovy 90, aspect
  1, Y-flip folded in) — exactly what a probe capture pass needs to render the scene into 6
  faces. These already drive the point-light distance-shadow cube.
- **On-demand re-bake machinery is production-ready.** `requestSkyBake`
  (`renderer_lighting.cpp:189`) sets `pendingParams` + `rebakePending` only when inputs
  actually change (exact compare, no float churn); `beginFrameGraph` consumes the flag at a
  GPU-idle point (`renderer.cppm:667-678`) and calls `bakeEnvironment(..., firstBake=false)`,
  which `waitIdle`s (`renderer_detail.cppm:3322`) and overwrites the images in place. Probe
  capture reuses this dirty-flag + GPU-idle policy so a heavy capture only runs on edit.
- **The mesh fragment already samples a cube-based IBL set.** `mesh.slang:139-144` declares
  set 3 (`irradianceMap`/`prefilteredMap` `SamplerCube` + `brdfLut`), gated by
  `globals.counts.z`; `recordSceneDrawList` binds it once at `renderer_drawlist.cpp:336`.
  The IBL set layout + set are created at `renderer_detail.cppm:2753-2796`. A probe adds a
  **new descriptor set** beside set 3 (proposed set 8 — the highest current set is 7,
  `mesh.slang:161`) holding a probe-cube array + a probe metadata buffer; the fragment picks
  the nearest probe and blends its specular over the set-3 fallback.
- **The component pattern is one call.** `PointLightComponent` (`scene.cppm:86-91`) is the
  worked example: a plain struct, registered once via `registerComponent<C>`
  (`scene.cppm:450-489`) in `editor_components.cpp:227-247` with a draw lambda, a `toJson`,
  and a `fromJson` — that one call wires the inspector, JSON save/load, copy/clone, and
  add/remove with zero central edits. `ReflectionProbeComponent` follows it exactly.
- **`renderScene` is the per-frame scene→renderer resolve.** `renderScene`
  (`assets.cppm:399`) reads the directional light via `forEach`
  (`assets.cppm:423-434`), arms the cluster cull, calls `requestSkyBake` (`:618-627`),
  and submits the draw list (`:633`). Probe scanning + dirty detection + the capture
  request live here, right after the light read.

## Data Model

New component, placed beside `PointLightComponent` in
`engine/source/saffron/scene/scene.cppm:86`:

```cpp
// A reflection probe at the entity's Transform translation. Captures a local cubemap of
// the scene, prefilters it like the global IBL, and supplies specular ambient to meshes
// inside its influence sphere (radius). Outside every probe, meshes fall back to the
// global IBL. boxProjection re-projects the prefiltered reflection ray against the
// influence box for parallax-correct local reflections (off = infinite-distance cube).
struct ReflectionProbeComponent
{
    f32 influenceRadius = 10.0f;   // sphere of effect around the probe origin
    f32 intensity = 1.0f;          // probe specular multiplier
    bool boxProjection = false;    // parallax-correct against the influence box
    glm::vec3 boxExtent{ 10.0f };  // half-extents for box projection (used when boxProjection)
    bool dirty = true;             // capture pending; set on add/edit, cleared after capture
};
```

`dirty` is the per-probe analogue of `Ibl.rebakePending`. The origin comes from the
entity's `TransformComponent.translation` (the same way `PointLightComponent` is positioned,
`assets.cppm:444-446`), so the probe carries no position field.

Register it once in `engine/source/saffron/editor/editor_components.cpp`, beside the
`registerComponent<PointLightComponent>` call (`:227-247`):

```cpp
registerComponent<ReflectionProbeComponent>(reg, "ReflectionProbe",
    [](Scene& s, Entity e)
{
        ReflectionProbeComponent& probe = getComponent<ReflectionProbeComponent>(s, e);
        if (ImGui::DragFloat("Influence Radius", &probe.influenceRadius, 0.1f, 0.1f, 500.0f)) { probe.dirty = true; }
        ImGui::DragFloat("Intensity", &probe.intensity, 0.01f, 0.0f, 8.0f);
        if (ImGui::Checkbox("Box Projection", &probe.boxProjection)) { probe.dirty = true; }
        if (probe.boxProjection)
        {
            if (ImGui::DragFloat3("Box Extent", &probe.boxExtent.x, 0.1f, 0.1f, 500.0f)) { probe.dirty = true; }
        }
        if (ImGui::Button("Recapture")) { probe.dirty = true; }
    },
    [](const ReflectionProbeComponent& c) -> nlohmann::json {
        return nlohmann::json{ { "influenceRadius", c.influenceRadius }, { "intensity", c.intensity },
                               { "boxProjection", c.boxProjection }, { "boxExtent", vec3ToJson(c.boxExtent) } };
    },
    [](ReflectionProbeComponent& c, const nlohmann::json& j) -> Result<void>
    {
        c.influenceRadius = jsonF32Or(j, "influenceRadius", 10.0f);
        c.intensity = jsonF32Or(j, "intensity", 1.0f);
        c.boxProjection = jsonBoolOr(j, "boxProjection", false);
        c.boxExtent = vec3FromJson(j.value("boxExtent", nlohmann::json::object()));
        c.dirty = true;  // loaded probes start dirty -> captured on first frame
    },
    true);
```

`vec3ToJson`/`vec3FromJson`/`jsonF32Or`/`jsonBoolOr` are the same helpers the light
components use (`editor_components.cpp:213-222`). `dirty` is intentionally **not**
serialized — it is runtime capture state; loaded probes start dirty and are captured on
first frame. The component is registered through `registerComponent` only, so save/load,
clone, inspector, and add/remove all come for free.

## Renderer Data

New renderer state, beside the `Ibl` struct in
`engine/source/saffron/rendering/renderer_types.cppm:732`:

```cpp
inline constexpr u32 MaxReflectionProbes = 8;  // hard cap; excess probes ignored (logged once)

// One captured + prefiltered local reflection probe. Mirrors the Ibl cube layout but
// per-probe; baked on demand (capture pass renders the scene into 6 faces, then the
// shared ibl_irradiance/ibl_prefilter convolve into these). Sampled at mesh set 8.
struct ReflectionProbe
{
    Image envCube;          // captured local environment (newColorCubeImage: 6 face views)
    std::array<vk::ImageView, 6> faceViews{};  // per-face color attachment views
    Image irradianceCube;   // diffuse irradiance convolution (per-probe)
    Image prefilteredCube;  // GGX-prefiltered specular (per-probe, prefilterMips)
    glm::vec3 origin{ 0.0f };
    f32 influenceRadius = 10.0f;
    f32 intensity = 1.0f;
    bool boxProjection = false;
    glm::vec3 boxExtent{ 10.0f };
    u64 entity = 0;         // owning entity id (capture re-uses the slot when re-armed)
    bool valid = false;
};

struct ReflectionProbes
{
    std::array<ReflectionProbe, MaxReflectionProbes> probes;
    u32 count = 0;
    vk::DescriptorSetLayout meshLayout;  // mesh set 8: probe-cube array + probe metadata SSBO
    vk::DescriptorSet meshSet;
    vk::DescriptorSetLayout faceLayout;  // capture-pass per-face sampling (reuses scene sets)
    Ref<Buffer> metaBuffer;              // MaxReflectionProbes probe records (origin/radius/intensity/box)
    bool useProbes = true;
    bool capturePending = false;         // any probe dirty this frame -> capture in beginFrameGraph
};
```

Add `ReflectionProbes reflection;` to the `Renderer` struct beside `Ibl ibl;`.

Public renderer API (declare next to `requestSkyBake` in `renderer.cppm`'s exported block,
implement in `renderer_lighting.cpp` beside `requestSkyBake` at `:189`):

```cpp
// Sync the renderer's probe slots from the scene (called each frame from renderScene).
// Adds/updates/removes probe slots, allocates cubes on first sight, arms capture for any
// slot whose `dirty` is set or whose origin/radius changed, and uploads the metadata SSBO.
void submitReflectionProbes(Renderer& renderer, std::span<const ReflectionProbeUpload> probes);

void setReflectionProbes(Renderer& renderer, bool enabled);  // set-probes 0|1
```

`ReflectionProbeUpload` is a small POD (entity id, origin, influenceRadius, intensity,
boxProjection, boxExtent, dirty) defined beside `SkygenParams` in `renderer_types.cppm:721`
so `renderScene` (which imports `Saffron.Rendering`, not `Saffron.Scene` into the renderer)
can pass scene data without the renderer depending on the component type — the same
decoupling the `Sky.mode` int uses (`renderer_types.cppm:752-756`).

## Shader Approach

Two existing shaders are reused unchanged for the convolution; one is new for capture; one
is edited for sampling.

- **Capture** uses the existing scene mesh path (`mesh.slang`). The capture pass renders the
  scene 6 times (once per cube face) through `pointShadowFaceMatrices(probe.origin, far)`
  (`renderer_detail.cppm:833`) into the probe's `faceViews`. No new geometry shader — render
  each face as its own small graphics pass (matching the per-face point-shadow render at
  `point_shadow.slang`/`makeShadowPipeline`). This is the heavy part (see Cost).
- **Irradiance + prefilter** reuse `ibl_irradiance.slang` and `ibl_prefilter.slang`
  **verbatim** — they read `envCube` as a `SamplerCube` (set 0 binding 0) and write storage
  views, so pointing them at the probe's `envCube` + the probe's irradiance/prefiltered
  storage views produces identical convolutions. No shader change.
- **New `ibl_probe_capture.slang`** is *optional*: for a first cut, capture by re-recording
  the scene draw list into the 6 faces (reusing `recordSceneDrawList`,
  `renderer_drawlist.cpp:325`) with the face view-proj pushed in place of the camera's. A
  dedicated lightweight capture shader (albedo + sky + global IBL ambient only, no probes —
  to avoid recursion) is a refinement, not required for phase 6. Note any new `.slang`
  needs a CMake **reconfigure** (shaders are globbed by `saffron_compile_shaders` over
  `editor/assets/shaders`, `editor/CMakeLists.txt:11-13`; no CMakeLists edit).
- **`mesh.slang` edit (new set 8).** Add beside set 3 (`mesh.slang:141-143`):

```hlsl
// Set 8: spatial reflection probes. probeCubes[i] is probe i's prefiltered specular cube;
// probeMeta holds origin/radius/intensity/box per probe. Picks the nearest probe whose
// influence sphere contains the fragment and blends its specular over the global IBL
// (set 3) fallback. Gated by screenFlags-style probe count in globals (counts is full;
// add a probeCount field or reuse an unused slot).
[[vk::binding(0, 8)]] SamplerCube probeCubes[8];        // MaxReflectionProbes
[[vk::binding(1, 8)]] SamplerCube probeIrradiance[8];
[[vk::binding(2, 8)]] StructuredBuffer<ProbeMeta> probeMeta;
```

Probe selection in the fragment: iterate `probeMeta`, find the nearest probe whose
`distance(worldPos, origin) < influenceRadius`, sample its prefiltered cube by the
reflection vector (box-project the ray against `origin ± boxExtent` when
`boxProjection`), and lerp the specular IBL term toward it by an edge-soft weight (1 at
center, 0 at the influence boundary). Optional blend across the two nearest probes by
weighting the boundary falloffs. When no probe covers the fragment, the term is exactly the
existing set-3 `prefilteredMap` sample — so meshes outside all probes are pixel-identical to
today (the verification A/B).

Box projection math (standard parallax-corrected cubemap, `boxProjection` only):

```hlsl
float3 boxProject(float3 worldPos, float3 R, float3 origin, float3 extent)
{
    float3 invR = 1.0 / R;
    float3 tMax = (origin + extent - worldPos) * invR;
    float3 tMin = (origin - extent - worldPos) * invR;
    float3 t = max(tMax, tMin);
    float d = min(min(t.x, t.y), t.z);
    return (worldPos + R * d) - origin;  // sample direction relative to probe center
}
```

## Render Graph Placement

Capture is a **conditional, on-demand** sequence, not a per-frame pass. It runs at the same
GPU-idle point the IBL re-bake uses — the top of `beginFrameGraph` (`renderer.cppm:662-678`),
before the frame graph is built — so it never interleaves with the live frame's barriers:

```cpp
// in beginFrameGraph, right after the IBL rebake block (renderer.cppm:678)
if (renderer.reflection.capturePending && renderer.pipelines.cull /* PSOs ready */)
{
    for (ReflectionProbe& probe : renderer.reflection.probes ...)
    {
        if (!probe.dirty) { continue; }
        captureReflectionProbe(renderer, probe);   // 6-face scene render + convolve
        probe.dirty = false;
    }
    renderer.reflection.capturePending = false;
}
```

`captureReflectionProbe` (new, beside `bakeEnvironment` in `renderer_detail.cppm:3297`)
records into a one-shot command buffer exactly like `bakeEnvironment`:

1. For face `f` in 0..5: barrier the face to `eColorAttachmentOptimal`, begin dynamic
   rendering into `probe.faceViews[f]` (plus a transient depth), record the scene draw list
   with the face view-proj from `pointShadowFaceMatrices` (`:833`), end rendering.
2. Barrier `probe.envCube` (all 6 layers) → `eShaderReadOnlyOptimal`.
3. Dispatch `ibl_irradiance` reading `probe.envCube` into `probe.irradianceCube`, then
   `ibl_prefilter` per mip into `probe.prefilteredCube` — the same dispatch loop as
   `bakeEnvironment` (`:3504-3530`), just with the probe's images.
4. Submit + `waitIdle` (the IBL bake already does this, `:3548-3549`).
5. On first capture for a slot, write the probe's prefiltered + irradiance cubes into the
   `reflection.meshSet` array element for that slot (mirrors the set-3 write at `:3562-3574`).

The live mesh sets bind never changes structurally: `recordSceneDrawList`
(`renderer_drawlist.cpp:336`) binds set 3 (global IBL fallback) as today and additionally
binds `renderer.reflection.meshSet` at set 8 once per frame — always valid because every
array slot is seeded with the global `prefilteredCube`/`irradianceCube` until a real probe
overwrites it (so unused slots harmlessly resolve to the global env).

## Asset Loading

No new asset type. A reflection probe captures runtime scene geometry; nothing is imported
or stored on disk — the cubes are GPU-only and regenerated on load (probes deserialize with
`dirty = true`, so the first frame after `loadProject` recaptures them). This is the same
discipline as the IBL bake (recomputed at init, never persisted).

## Editor + Control + Serialization

- **Editor (Create menu + inspector):** add `ReflectionProbe` to the Create menu beside the
  light entries (the menu spawns an entity + adds the component, same as
  `PointLightComponent`), and the inspector comes free from the `registerComponent` draw
  lambda above. A captured probe should show a small gizmo/sphere for its influence radius
  (optional, reuse the editor's debug-draw if present; not blocking).
- **Control (`se`):** keep the CLI current (project convention). Add, in
  `engine/source/saffron/control/control_commands_render.cpp` beside `set-ibl`
  (`:110`), reusing the `registerCommand` signature (`command.cppm:50`):
  - `set-probes {0|1}` → `setReflectionProbes(ctx.renderer, ...)` (global toggle, A/B gate).
  - `recapture-probes` → mark every `ReflectionProbeComponent` in `ctx.editor`'s scene
    `dirty = true` (forces a re-capture; useful headless).
  - `list-probes` → report each probe's origin/radius/intensity/captured-state from
    `ctx.renderer.reflection`.
  Reflection-probe *components* (add/remove/set fields) are already drivable through the
  generic `add-component`/`set-component` commands once registered, like the lights — no
  per-field command needed.
- **Serialization:** entirely automatic via `registerComponent` (the `toJson`/`fromJson`
  lambdas above). No `sceneToJson`/`sceneFromJson` edit and **no `SceneVersion` bump** — a
  new component adds an entry to an entity's component list, which old/new loaders already
  iterate generically (unlike phase 1's `SceneEnvironment`, which lived on `Scene` itself and
  needed the v1→v2 migration).
- **Docs (`docs/`):** add an explanation page under `docs/content/` for reflection probes
  (local cubemap capture + prefilter + nearest-probe blend over the global IBL fallback) and
  link it from the rendering hub `_index.md`, same as the IBL/sky pages (project convention).

## Cost And On-Demand Capture Policy

A probe capture renders the **entire scene 6 times** plus two convolution passes — far
heavier than the single-cube IBL bake (which dispatches a procedural skygen, not scene
geometry). Therefore:

- Capture is **strictly on demand**: only when a probe's `dirty` flag is set (add, inspector
  edit, `Recapture` button, `recapture-probes`, or first frame after load). It is **never**
  per frame. `submitReflectionProbes` flips `capturePending` only when a slot is dirty or its
  origin/radius actually changed (exact-compare, mirroring `requestSkyBake`'s drift guard,
  `renderer_lighting.cpp:193-198`).
- Capture runs at the GPU-idle top of `beginFrameGraph`, the same editor-time stall point as
  the IBL re-bake — acceptable because it is a user-driven event, not the hot path.
- Probe cubes are small (start at `IblEnvSize = 128²` rgba16f, `renderer_detail.cppm:1084`)
  and capped at `MaxReflectionProbes = 8`. Excess probes beyond the cap are ignored and
  logged once.
- A future refinement (note the seam, do not build): amortize by capturing **one face per
  frame** or **one probe per N frames** as a graph compute/graphics pass, the way DDGI
  ping-pongs probes (`renderer.cppm:1004-1138`). Phase 6 ships the simple synchronous capture
  first.

## Implementation Steps

1. **Component** — add `ReflectionProbeComponent` to `scene.cppm:86` (beside
   `PointLightComponent`); register it in `editor_components.cpp` beside the
   `registerComponent<PointLightComponent>` call (`:227`). Confirms save/load + inspector
   with no further edits.
2. **Renderer state** — add `ReflectionProbe`/`ReflectionProbes` + `MaxReflectionProbes` to
   `renderer_types.cppm:732` (beside `Ibl`); add `ReflectionProbes reflection;` to `Renderer`.
   Add `ReflectionProbeUpload` beside `SkygenParams` (`:721`).
3. **Resources** — at renderer init (beside the set-3 creation, `renderer_detail.cppm:2753`),
   create the set-8 layout (probe cube array + irradiance array + metadata SSBO), allocate
   `meshSet`, and seed every array slot with the global `prefilteredCube`/`irradianceCube` so
   the bind is always valid. Allocate `MaxReflectionProbes` cubes lazily on first capture via
   `newColorCubeImage` (`:765`) for `envCube`/faces + `newCubeImage` (`:706`) for the
   per-probe irradiance/prefiltered cubes.
4. **Capture** — add `captureReflectionProbe` beside `bakeEnvironment`
   (`renderer_detail.cppm:3297`): 6-face scene render via `pointShadowFaceMatrices` (`:833`)
   + `recordSceneDrawList` (`renderer_drawlist.cpp:325`), then `ibl_irradiance`/`ibl_prefilter`
   dispatches (clone the loop at `:3504-3530`). Write the probe slot into `meshSet`.
5. **Dirty drive** — add `submitReflectionProbes`/`setReflectionProbes` in
   `renderer_lighting.cpp` beside `requestSkyBake` (`:189`); call `submitReflectionProbes`
   from `renderScene` (`assets.cppm`, right after the directional-light read at `:434`) by
   scanning `forEach<TransformComponent, ReflectionProbeComponent>` into uploads. Consume
   `capturePending` at the top of `beginFrameGraph` (`renderer.cppm:678`).
6. **Mesh shader** — add set 8 to `mesh.slang:143` (probe arrays + meta SSBO) + the
   nearest-probe blend + box projection; gate it so zero probes = today's output. Bind
   `reflection.meshSet` at set 8 in `recordSceneDrawList` (`renderer_drawlist.cpp:336`).
   Reconfigure CMake if any new `.slang` is added.
7. **Editor** — add `ReflectionProbe` to the Create menu beside the light entries.
8. **Control + docs** — add `set-probes`/`recapture-probes`/`list-probes` in
   `control_commands_render.cpp` beside `set-ibl` (`:110`); add the reflection-probe docs page.

## Verification

Build only in the toolbox, single-threaded: `toolbox run -c saffron-build bash -lc 'cmake
--preset debug && cmake --build build/debug -j1'` (reconfigure first if a new `.slang` was
added). Then, all with `VAL=0` (validation-clean) required:

- **No-probe identity (A/B):** scene with no `ReflectionProbeComponent`,
  `SAFFRON_EXIT_AFTER_FRAMES=5 ./build/debug/bin/SaffronEditor`, `se screenshot viewport
  /tmp/before.png`. Add the set-8 code path off (`se set-probes 0`) vs on (`se set-probes 1`)
  with zero probes present → the two PNGs must be **pixel-identical** (numpy/PIL diff == 0),
  proving the global-IBL fallback is unchanged.
- **Round-trip:** create a probe via the Create menu (or `add-component`), `se save-project`,
  restart, `se load-project`, `se list-probes` → the probe reappears with the same
  origin/radius/intensity, and is recaptured (dirty-on-load) on the first frame.
- **Influence:** place a probe near a reflective mesh, `se recapture-probes`, screenshot;
  move the mesh outside `influenceRadius`, screenshot → the in-influence shot shows the local
  cube's reflection, the out shot matches the global IBL (numpy diff localized to the mesh).
- **Box projection:** toggle `boxProjection` on a probe in a boxy room; planar surfaces show
  parallax-correct (non-infinite) reflections vs the off case (diff non-zero on flat walls).
- **On-demand only:** run `SAFFRON_EXIT_AFTER_FRAMES=120` with a static probe; confirm
  `captureReflectionProbe` logs exactly once (first frame), not per frame — capture must not
  appear in the steady-state frame.
- **Clean teardown:** the editor exits code 0 on both the frame-bounded and `se quit` paths
  (probe `Ref`s/`Image`s released before `vmaDestroyAllocator`, the meta-layer teardown
  contract; clear probe slots in `destroyRenderer` like `rt.frameMeshes`).
