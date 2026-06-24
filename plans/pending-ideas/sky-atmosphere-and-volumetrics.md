# Sky atmosphere, volumetrics & time-of-day

**Status:** PENDING IDEA

> Inspiration backlog — not yet implementable as written. Needs a codebase pass (the LUT compute chain
> as render-graph passes, a "sun" directional-light tag, and sky-light re-bake into the existing IBL).

The biggest "AAA look" jump per unit of effort. The sky atmosphere is ~4 small compute passes the render
graph is built for, and once it is dynamic, **time-of-day falls out nearly free**. Volumetric fog reuses
the clustered lights, every shadow type, and TAA — all already present.

## What it is

A physically based sky + sun, volumetric fog and light shafts, optional volumetric clouds, and a
time-of-day driver that animates all of it.

- **UE5:** Sky Atmosphere + Volumetric Clouds + Exponential Height Fog + a Volumetric Fog flag on lights.
- **Unity:** Physically Based Sky + HDRP Volumetric Fog/Clouds.

## Core technique

**Sky (Hillaire 2020):** precompute a **Transmittance LUT** and a **Multiple-Scattering LUT**, build a
**Sky-View LUT** per frame, and fill an **Aerial-Perspective froxel** volume so distant geometry inherits
atmospheric scattering. Rayleigh (air) + Mie (haze) + ozone absorption. Because the LUTs rebuild cheaply
each frame, moving the sun = **dynamic time-of-day for free**.

**Volumetric fog (froxel):** a camera-frustum 3D grid — density pass → per-froxel light injection
(Henyey–Greenstein phase, reusing the clustered-light list and all shadow maps) → a ray-integration scan
→ temporal reprojection (reuse motion vectors + TAA history). Analytic exponential **height fog** is a far
cheaper subset.

**Volumetric clouds (Nubis-derived):** Perlin–Worley base shape + Worley erosion, ray-marched with a Beer
shadow map and ~16-frame temporal reconstruction.

## Build size

- **M** sky atmosphere (the 4-LUT chain).
- **S** analytic height fog; **M** froxel volumetrics; **M** local fog volumes.
- **S** time-of-day driver (controller + `sa` scrub command + sky-light re-capture) — near-free once the
  sky is dynamic.
- **L–XL** volumetric clouds (gated on sky atmosphere).

## Dependencies (do these first)

- **Sky atmosphere first** — fog and clouds and TOD all build on it.
- A **"sun" directional-light tag** + a **sky-light re-bake** from the LUT into the existing IBL/ReSTIR
  environment so GI follows the time of day. Voxel-GI / DDGI reconvergence already exists.
- *Local fog volumes* want **scene-graph parenting** (attach to moving entities).
- *Clouds:* transient 3D resources help (a known render-graph gap), not required.

## What we reuse / what's missing

**Reuse:** compute (the LUT chain is the render graph's sweet spot), bindless, clustered lighting + every
shadow type (fog light injection), motion vectors + TAA (volumetric reprojection), and the existing
IBL/ReSTIR pipeline that already consumes an environment.

**Missing:** the sun/sky-light tagging + a re-bake hook; a 1D curve-editor for TOD ramps (shared enabler);
density authoring for clouds (could be a "volume" material-graph domain).

## Notes & references

- Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique" (2020) — the LUT
  method everyone now uses.
- Schneider & Vos, "The Real-time Volumetric Cloudscapes of Horizon Zero Dawn" (Nubis) — clouds.
- Wronski, "Volumetric Fog" (Assassin's Creed 4) — the froxel injection/integration approach.
