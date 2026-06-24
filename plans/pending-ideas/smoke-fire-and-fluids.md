# Smoke, fire & fluids

**Status:** PENDING IDEA

> Inspiration backlog — not yet implementable as written. Needs a codebase pass (ping-pong 3D-texture
> render-graph resources, the volumetric ray-march pass, and — for liquids — the GPU particle system).

Eulerian gas (smoke/fire) is a self-contained compute job the render graph is built for. Liquid (FLIP)
is a harder, later milestone that depends on the GPU particle system.

## What it is

- **Gas — smoke & fire:** a 3D grid solver that advects density/temperature, rises with buoyancy, and is
  rendered as a participating medium. Fire maps temperature through a blackbody color ramp.
- **Liquid — FLIP:** particle-based water/lava with a screen-space surface.

- **UE5:** Niagara Fluids (grid 3D gas + FLIP liquid plugins on top of Niagara).
- **Unity:** no first-party solver; the ecosystem uses Zibra Liquids / smoke assets.

## Core technique

**Gas (stable fluids, operator-split on 3D textures):** semi-Lagrangian advection (MacCormack/BFECC to
fight numerical diffusion) → add **buoyancy** (hot rises) and **vorticity confinement** (restore the
curl lost to advection) → **Jacobi pressure-projection** to make the field divergence-free. Fire drives a
**blackbody emission ramp** from the temperature field. Rendering is a single-scatter **ray-march**
(Beer–Lambert extinction + shadow rays toward the sun), and **TAA denoises** the result for free.

**Liquid (FLIP):** particle-to-grid (P2G) transfer → grid pressure solve → grid-to-particle (G2P) with a
PIC/FLIP blend; the surface is reconstructed in screen space (depth + thickness, bilateral blur, normal
from depth) — "screen-space fluid rendering" (SSFR).

## Build size

- **M** gas solver (advect/buoyancy/vorticity/pressure on 3D textures).
- **M** volumetric ray-march render; **S/M** authoring → shippable **smoke & fire**.
- **L** sparse / VDB domain (only sim where occupied — for large volumes).
- **L–XL** FLIP liquid; **M** the screen-space surface render.

## Dependencies (do these first)

- **Ping-pong 3D-texture render-graph resources** — works with persistent images now; transient/aliased
  images (a known render-graph gap) make it cheaper.
- **Gas needs nothing else** beyond compute + the render graph — it is independent of the particle system.
- **FLIP liquid depends on [gpu-particle-vfx](gpu-particle-vfx.md)** (particle buffers + spatial binning)
  — build the particle system first.

## What we reuse / what's missing

**Reuse:** compute, the render graph (ping-pong 3D images), TAA + motion vectors (denoise), all shadow
types + IBL (lighting the medium), tonemap, the node editor (force-field / source authoring), and the
control plane.

**Missing:** transient/aliased 3D images (optional optimization); for FLIP, the GPU particle system and a
spatial-binning structure.

## Notes & references

- Stam, "Stable Fluids" (SIGGRAPH 1999) — the operator-split foundation.
- Fedkiw et al., "Visual Simulation of Smoke" — buoyancy + vorticity confinement.
- "GPU Gems 3" ch. on real-time fluid; Zibra/Niagara Fluids talks for production tricks.
- Müller / NVIDIA FLIP references; van der Laan, "Screen Space Fluid Rendering" for the surface.
