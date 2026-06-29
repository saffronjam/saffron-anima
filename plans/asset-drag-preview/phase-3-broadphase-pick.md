# Broadphase pick: sublinear placement raycast

**Status:** IMPLEMENTED (Route A — cached CPU BVH)

> **Done.** `MeshBvh` added to `geometry/src/picking.rs` (median-split binary BVH over a mesh's
> triangles in mesh-local space; `build` + `raycast`), exported from the geometry crate.
> `AssetServer` gained a `mesh_bvh_by_uuid` cache + `mesh_pick_bvh(sub_id, &GpuMesh)` accessor
> (`assets/src/{lib,load}.rs`, built lazily, cleared with the other GPU caches). `pick_scene_surface`
> (`assets/src/render_scene.rs`) now keeps the cheap world-AABB broadphase, then runs the BVH in
> mesh-local space (ray transformed by the inverse world matrix; hit point mapped back to world)
> instead of transforming + scanning every triangle. The skinned path still deform-skins + scans
> (a static BVH can't model a per-frame palette), as planned. `cargo build` + `clippy -D warnings`
> clean; the BVH↔brute-force parity test (`bvh_raycast_matches_brute_force`) and the empty-mesh
> guard (`bvh_build_rejects_empty`) pass, alongside the existing geometry/assets suites. The
> control-plane `picking.test.ts` e2e covers pick correctness end-to-end in CI.

**Scope:** `saffron-assets` (and optionally `saffron-rendering` if reusing the GPU pick).
**Scope:** `saffron-assets` (and optionally `saffron-rendering` if reusing the GPU pick).
**Depends on:** — (independent; most valuable after Phase 1, which makes the pick the dominant per-move
cost).

## Goal

`pick_scene_surface` (`engine/crates/assets/src/render_scene.rs:1085`) runs on **every** drag-over move
to find the drop point. It already has an AABB broadphase per mesh (`ray_aabb_slab`,
`render_scene.rs:1130`), but once the ray enters a mesh's box the narrowphase is brute force: it
allocates a fresh `Vec<Vec3>` of *all* the mesh's vertices in world space and tests *every* triangle
(`render_scene.rs:1133`). So dragging into a heavy scene lags regardless of the dragged object's size —
the cost is the scene's triangle count, which neither Phase 1 nor Phase 2 touches. Make the pick
sublinear in triangles.

This same brute-force pick backs gizmo hover/selection picking too, so the win is not preview-only.

## Design

Two viable routes; pick one.

### Route A — per-mesh BVH (CPU)

Build a bounding-volume hierarchy over each mesh's triangles **once**, cached on the `GpuMesh` (or a
sibling CPU-side `MeshPick` cached by sub-id next to `cpu_positions`/`cpu_indices`). The placement ray
walks the tree, descending only into child boxes it hits, reaching the few candidate triangles in
~log(N) instead of scanning all N. Build lazily on first pick of a mesh and cache for the asset's
lifetime (same lifetime as the vertex cache).

- Removes the per-pick `Vec<Vec3>` allocation: transform the ray into mesh-local space once
  (`model.inverse()`), traverse the local-space BVH, transform only the hit point back to world.
- Skinned meshes (`render_scene.rs:1150`) keep the current deform-then-test path (the palette changes
  per frame so a static BVH does not apply); the broadphase joint-union box already rejects most.

### Route B — reuse the GPU pick

The renderer already builds a per-mesh BLAS (a BVH) and ray-traces. If an ID/depth pick is already
recorded for gizmo selection, route placement through it: trace the cursor ray, read back the hit world
position. Avoids a second CPU structure entirely, at the cost of a GPU readback latency per move (which
the ~60/s throttle bounds). Prefer this only if a pick pass already exists; do not add a readback stall
that reintroduces the very latency this plan removes.

**Recommendation:** Route A — a cached CPU BVH is self-contained, has no readback latency, and the ghost
must be excluded from the pick (Phase 1) anyway, so this code is already in hand.

## Files

| What | File | Symbols |
|------|------|---------|
| Pick | `engine/crates/assets/src/render_scene.rs` | `pick_scene_surface`, `nearest_triangle`, `ray_aabb_slab` |
| Cached BVH (Route A) | `engine/crates/rendering/src/upload.rs` or `assets/src/load.rs` | new `MeshPick`/BVH cached by sub-id beside `cpu_positions` |
| GPU pick (Route B) | `engine/crates/rendering/src/rt.rs` | existing pick/readback, if any |

## Verification

- `just engine` + `just prepare-for-commit`.
- A unit test: BVH pick and the old brute-force pick return the **same** nearest hit (entity + point
  within epsilon) over a set of rays against a multi-mesh scene — correctness parity before deleting the
  brute-force narrowphase.
- Manual on the RTX 3070 Ti: dragging a small prop into a scene already full of high-tri geometry tracks
  the cursor smoothly (pick cost no longer scales with scene triangles).
