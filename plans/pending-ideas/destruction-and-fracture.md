# Destruction & fracture

**Status:** PENDING IDEA

> Inspiration backlog — not yet implementable as written. Needs a codebase pass (a Voronoi/convex-clip
> geometry kernel, a `.sfrac` asset model, and the strain-graph runtime over Jolt compound bodies).

Breakable geometry: pre-fractured meshes that shatter under impact, with structural integrity so
unsupported pieces fall. The physics side rides on Jolt; the one genuinely new primitive is a
**convex-clipping / Voronoi geometry kernel**.

## What it is

- **UE5:** Chaos Destruction — geometry collections, cluster hierarchies, and Fields (force/strain
  volumes).
- **Unity:** no first-party system; the ecosystem uses RayFire.

## Core technique

**Offline fracture:** scatter seed points, build a 3D **Voronoi** diagram, **convex-clip** the source
mesh against each cell, and cap the new interior faces with an inner material. This produces a `.sfrac`
asset: the cell meshes + a **proximity/adjacency graph** + multi-level **cluster** groupings.

**Runtime:** a **connection-graph strain solver** — each frame, sever graph edges whose accumulated
impulse exceeds a limit, **flood-fill** the now-disconnected islands, and promote each island to its own
Jolt body. UE's **Fields** are simply strain/impulse volumes that seed the breaking. Debris uses
instancing; dust/break VFX come from the particle system.

## Build size

- **L** the Voronoi/convex-clip kernel (the one new primitive).
- **M** the `.sfrac` asset model.
- **L** the strain runtime (sever → flood-fill → promote to bodies).
- **M** precached runtime-fracture (fracture on first hit).
- **S/M** fields, debris, interior materials.
- **XL** true real-time mesh cutting — defer.

## Dependencies (do these first)

- A **convex-clipping / Voronoi geometry kernel** — the genuinely new code.
- **Scene-graph parenting** (a known gap) — cluster hierarchies are a parent/child tree.
- **[gpu-particle-vfx](gpu-particle-vfx.md)** — for dust/debris VFX on break.

## What we reuse / what's missing

**Reuse:** Jolt compound bodies + breakable constraints, the contact-event ring (impacts drive breaks),
instancing (debris), the `.smat` material + node-graph (interior faces), the signal/slot system (break
events to gameplay), and Luau/`sa`.

**Missing:** the Voronoi/convex-clip kernel, the `.sfrac` asset, the strain-graph runtime, and
scene-graph parenting for clusters.

## Notes & references

- UE5 Chaos Destruction docs — geometry collections, clusters, Fields.
- Voronoi shatter references (e.g. Houdini/Blender cell-fracture algorithms) for the offline kernel.
- Müller et al. on real-time fracture for the strain/connection-graph model.
