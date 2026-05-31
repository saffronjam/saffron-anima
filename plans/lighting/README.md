# Dynamic Lighting Plan

This folder tracks the implementation plan for proper real-time, fully-dynamic,
entity-based lighting in SaffronEngine — from a real shading model through no-bake
global illumination to optional hardware ray tracing. It is the output of the
2026-05-31 lighting investigation (UE5 Lumen / MegaLights / ReSTIR, Vulkan KHR ray
tracing, DDGI, voxel/SDF GI, the baking question) cross-referenced against the
current renderer.

## Plan status convention

Each phase file carries a `**Status:**` line near the top:

- `NOT STARTED` — not begun.
- `IN PROGRESS` — partially implemented (note what is done inline).
- `COMPLETED` — the phase's *Done when* checklist passes, validation-clean, merged.

**Mark a plan `COMPLETED` when its work is done. Delete a plan file only *after* it
is `COMPLETED`** (so an in-flight or pending phase is never lost). When every phase
here is `COMPLETED` and deleted, delete the folder.

## The three questions this answers

1. **"Make all lights fully dynamic / entity-based."** Already true — the engine is
   clustered-forward (Forward+): `DirectionalLight`/`PointLight`/`SpotLight` ECS
   components are packed per-frame and a `light_cull.slang` compute pass feeds the
   mesh fragment shader. Nothing is baked. What is missing is a real shading *model*,
   *shadows*, and *global illumination* — not dynamic lights.
2. **"Is baking required?"** No. UE5-era engines ship fully-dynamic GI with zero
   lightmaps (Lumen is the default, bakes nothing). Baking is an optional perf
   strategy for static scenes. Every phase here is no-bake (per-mesh SDF and one-time
   IBL convolution are asset processing, not a lighting bake).
3. **"What is UE5 doing?"** Lumen (dynamic GI + reflections) + MegaLights (stochastic
   many-shadowed-light direct lighting via ReSTIR). Borrowable: screen-trace-first +
   SDF-fallback architecture (phase 4), software SDF GI with no RT cores (phase 6),
   probe radiance cache (phase 6). Not borrowable at this scale: Lumen's Surface Cache
   card system, MegaLights ReSTIR-at-scale.

## Phases (dependency order)

| # | Phase | File | Depends on | Runs on llvmpipe? |
|---|-------|------|-----------|-------------------|
| 1 | PBR BRDF + HDR offscreen ✅ COMPLETED | `phase-1-pbr-brdf-hdr.md` | — | ✅ full speed |
| 2 | Image-based lighting (IBL) ✅ COMPLETED | `phase-2-image-based-lighting.md` | 1 | ✅ bake-once + cache |
| 3 | Dynamic shadow maps 🚧 directional done | `phase-3-dynamic-shadow-maps.md` | 1 | ✅ |
| 4 | Screen-space GI / AO (GTAO, SSGI, SSR) | `phase-4-screen-space-gi-ao.md` | 1, 2, (5 for SSGI) | ✅ low budget |
| 5 | Temporal AA + history + motion vectors | `phase-5-temporal-aa-history.md` | 4's thin G-buffer | ✅ |
| 6 | DDGI probe GI (software SDF/voxel trace) | `phase-6-ddgi-probe-gi.md` | 1, 2, 5 | ✅ sample side; slow update |
| 7 | Ray-tracing foundation + ray-query shadows | `phase-7-raytracing-foundation.md` | 1–3 | ⚠️ builds+validates ~1 FPS |
| 8 | ReSTIR many-light + RT-GI capstone | `phase-8-restir-many-light.md` | 5, 7 | ⚠️ ~1 FPS |

**Ordering note:** the dependency-correct order is 1 → 3 → 2 → … (PBR, then shadows,
then IBL). Phases 2 and 3 both depend only on phase 1 and are independent of each
other; this plan numbers **IBL as phase 2** by request (do IBL right after PBR). Pick
either order after phase 1.

**The single highest-leverage next step is phase 1** — it is the load-bearing
prerequisite for every later tier (shadows, IBL, and all GI feed radiance *into* the
BRDF, and the current flat scalar ambient *is* the only "GI"). Do not start GI or ray
tracing before PBR + shadows are dialed in.

## Cross-cutting infrastructure (shared prerequisites)

Several phases need the same engine-level additions. Each phase plan re-states the
ones it needs, but they are listed once here so they are built deliberately, not
duplicated:

- **HDR float offscreen** — `OffscreenColorFormat` is `eR8G8B8A8Unorm`
  (`engine/source/saffron/rendering/renderer_types.cppm:33`); PBR/IBL/GI produce HDR
  radiance and need `eR16G16B16A16Sfloat`. Owned by **phase 1**. Rebuilds the PSO
  cache via the existing `setAa` clear-and-rebuild flow (`renderer.cppm:1378`).
- **`Image` wrapper variants** — the `Image` struct (`renderer_types.cppm:114`) is
  2D, single-mip, single-layer. IBL/point-shadows need **cubemap + mip**; DDGI needs
  **3D storage images**; CSM needs **2D arrays**. Add creation variants + view types.
- **MRT (thin G-buffer)** — `RgPass.color` is a single `std::optional<RgAttachment>`
  (`render_graph.cppm:75`); SSGI/SSR/DDGI/TAA want a normal (and later motion-vector)
  target as a second color attachment. Widen `RgPass.color` to a small vector and
  loop it in `executeRenderGraph` (`render_graph.cppm:293`, `:325`). Owned by **phase 4**.
- **Graph subresource range** — `applyAccess` hardcodes
  `{ aspect, 0, 1, 0, 1 }` (`render_graph.cppm:218`); shadow cascade arrays / mip
  chains need a per-access mip/layer range, or work around it with atlas tiles.
- **`RgUsage` extension** — the enum (`render_graph.cppm:23`) has no
  acceleration-structure case; **phase 7** adds AS-build-write / ray-trace-read.
- **Engine-internal pass authoring** — geometry-redrawing passes (shadows,
  voxelization, depth-from-light) must be added inside `beginFrameGraph`
  (`renderer.cppm:467`) where they can see the private `SceneDrawList` + depth, *not*
  from a layer's `onRenderGraph` (which only exposes the offscreen color + swap image).
- **No transient/aliased graph resources** — every new GI target is a persistent
  renderer-owned `Image` imported each frame, exactly like the offscreen/MSAA/FXAA
  targets (`renderer.cppm:486-509`).
- **`se` control command per phase** — per the project's "keep `se` current" rule,
  every phase ships a matching control command (`renderer.cppm` setter +
  `control.cppm` `registerCommand`, template at `control.cppm:231` `set-clustered`)
  so each tier is live-toggleable and screenshot-verifiable. This doubles as the
  correctness loop on llvmpipe (`SAFFRON_CAPTURE=path` PNG + `se screenshot`).

## The llvmpipe constraint (read before judging perf)

The dev GPU in the `saffron-build` toolbox is **llvmpipe** (Mesa software Vulkan).
Verified live during the investigation: it exposes the **full KHR ray-tracing stack**
(`acceleration_structure`, `ray_query`, `ray_tracing_pipeline`, `bufferDeviceAddress`
all true), minus a few host/indirect/capture-replay sub-features. So:

- **Phases 1–6 run correctly** on llvmpipe (core Vulkan 1.3 compute + graphics).
- **Phase 7–8 RT code builds and validates** on llvmpipe too — but at ~1 FPS.

The honest position: build all of this as the architectural goal, validate
correctness on llvmpipe (PNG captures, like clustered lighting was A/B-verified), and
treat *interactive framerates* for the GI/RT tiers as a "real RTX/RDNA2+ GPU in the
toolbox" milestone (already a follow-up in `AGENTS.md`).

## Relationship to the skybox plan

`plans/skybox/` overlaps deliberately. Its phase 3 (RGB ambient) and phase 4
(HDR textures, equirect→cubemap, diffuse irradiance, prefiltered specular) are the
*same* cubemap + IBL infrastructure this folder's **phase 2** needs. Coordinate:
build the `GpuCubemap` / cube `Image` variant once and let both the visible sky and
IBL share it. RGB ambient (skybox phase 3) is a natural sub-step of PBR phase 1 here.
