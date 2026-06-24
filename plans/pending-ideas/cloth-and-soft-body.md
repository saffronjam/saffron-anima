# Cloth & soft-body

**Status:** PENDING IDEA

> Inspiration backlog — not yet implementable as written. Needs a codebase pass (Jolt soft-body FFI,
> the compute-skinning / dynamic-vertex-buffer ingestion path, a vertex-paint editor tool).

Cheap because Jolt's soft-body system is the **same XPBD constraint family** Chaos Cloth uses, and the
render-side ingestion (compute skinning → dynamic vertex buffers → skinned-BLAS rebuild) already exists.
The bulk of the remaining cost is a vertex-paint editor tool.

## What it is

Simulated cloth (capes, flags, skirts, banners) and soft bodies (jiggle, deformable props) that collide
with the world, attach to skeletons, and react to wind.

- **UE5:** Chaos Cloth (panel-based, painted constraints) and ML Cloth (a trained approximation — defer).
- **Unity:** built-in `Cloth` (deprecated/limited) or the third-party Obi solver.

## Core technique

XPBD (extended position-based dynamics): predict particle positions, then iteratively **project
constraints** with per-constraint compliance — stretch (edge length), dihedral **bend**, and for soft
bodies **volume/pressure**. Skeletal attachment uses painted **max-distance / pin masks**, **LRA
tethers** (long-range attachments to stop over-stretch), and a **backstop** to keep cloth off the body.
Wind is per-face aerodynamic drag/lift. Jolt's soft-body system implements all of these constraints plus
`SkinVertices` (blend toward a skinned reference pose each frame) and rigid-body collision.

## How UE5 / Unity do it (notes worth keeping)

- Authoring is dominated by **vertex painting** — max-distance, backstop, stiffness masks. The editor
  cost is this brush tool, not the solver.
- Cloth attaches to the skeleton and reads the animated reference pose; Jolt's `SkinVertices` is the
  exact hook, and we already produce skinned vertex data on the GPU.
- Self-collision and volumetric soft bodies need a **tetrahedralizer** (volume mesh) — that is the one
  genuinely new offline tool, and only for the advanced milestone.

## Build size

- **S–M** cloth core via Jolt soft bodies.
- **M** skeletal attachment + paint masks (editor-UX cost dominates).
- **S** wind (per-face aero).
- **M–L** self-collision / volumetric soft bodies (needs a tetrahedralizer).
- **XL** ML-cloth — defer indefinitely.

## Dependencies (do these first)

- **cxx-FFI surface for Jolt soft bodies** (same proven pattern as the rigidbody seam).
- A **vertex-paint viewport tool** (also reusable for terrain splat / foliage masks later).
- *Volumetric only:* a **tetrahedralizer** to build the volume mesh.

## What we reuse / what's missing

**Reuse:** Jolt soft bodies (constraints + `SkinVertices` + collision), compute skinning + dynamic
vertex buffers + skinned-BLAS rebuild (the render ingestion is **already built** for skeletal animation),
the object-layer matrix, and the skeleton overlay for paint feedback.

**Missing:** a `ClothComponent` / `.scloth` asset, the vertex-paint viewport tool, and a tetrahedralizer
(volumetric soft bodies only).

## Notes & references

- Jolt soft-body docs/sample (jrouwe/JoltPhysics) — constraint types, `SkinVertices`, settings.
- Müller et al., "Position Based Dynamics" and the XPBD follow-up (compliance formulation).
- UE5 Chaos Cloth docs — for the paint-mask authoring model worth copying.
