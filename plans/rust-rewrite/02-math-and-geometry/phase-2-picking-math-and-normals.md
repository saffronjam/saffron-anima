# Phase 2 — Picking math: ray-triangle, ray-AABB slab, world-AABB, generate-normals

**Status:** COMPLETED

**Depends on:** phase-1 (the `Vertex`/`Mesh`/`Ray` types and the crate `Error`/`Result` exist).

## Goal

Port the four pure-math helpers: `ray_triangle` (two-sided Möller–Trumbore), `ray_aabb_slab` (slab
test), `world_aabb_from_corners` (transform the 8 corners and accumulate min/max), and `generate_normals`
(recompute smooth normals from the triangles). These are the picking/bounds primitives the scene
picking path, the asset AABB auto-fit, and the importer normal-fallback all use.

## Why this shape (NO LEGACY)

These are leaf math functions with no ownership, no I/O, no FFI — a near-verbatim translation that comes
out the same length in Rust. They are free functions in C++ and stay free functions (or `Mesh::generate_normals`
as a method — the idiom rules permit a method where it reads naturally; the ray/AABB tests stay free
functions since they take loose `Vec3`s, not a receiver). The numeric epsilons are *load-bearing* and
carried verbatim: `kParallelEps = 1e-8`, `kForwardEps = 1e-5` in `ray_triangle`; the `1e-12` zero-length
normal guard in `generate_normals`. Changing them shifts pick results, so they are copied, not
"tidied".

The C++ uses out-parameters (`f32& tEnter`, `f32& tExit`, `f32& outT`) because that was the engine
style; the Rust ports return the data instead (`Option<f32>` for the single-`t` ray-triangle,
`Option<(f32, f32)>` for the slab's enter/exit), because `?`/`Option` is the idiomatic way to say
"hit or miss with a payload" — there is one way to ask "did it hit", the `Option`. `world_aabb_from_corners`
keeps its **accumulate** semantics (unions into a passed `&mut (Vec3, Vec3)` seeded by the caller to
±inf), because callers union across a joint palette across many calls (geometry.cppm:360).

## Grounding (real files/symbols)

- `engine-old/source/saffron/geometry/geometry.cppm`:
  - `rayTriangle` (612-644): edge1/edge2, `pvec = cross(dir, edge2)`, `det`, parallel reject
    (`-kParallelEps..kParallelEps`), barycentric `u`/`v` bounds, forward gate `t <= kForwardEps`.
  - `rayAabbSlab` (600-610): `invDir = 1/dir` (inf on axis-aligned is intentional), `t0`/`t1`,
    `tEnter = max(min...)`, `tExit = min(max...)`, hit iff `tExit >= 0 && tEnter <= tExit`.
  - `worldAabbFromCorners` (577-598): iterate the 8 corners by the `corner & 1/2/4` bit pattern,
    transform by `model`, `min`/`max` accumulate.
  - `generateNormals` (543-575): zero normals, accumulate face normals (`cross(b-a, c-a)`) per submesh
    respecting `vertexOffset`+`firstIndex`, then normalize with the `1e-12` guard (fallback `(0,1,0)`).
  - the picking self-test `runPickMathSelfTest` (2137-2184) — the test oracle for this phase.

## Plan

1. `pub fn ray_triangle(ray: &Ray, v0: Vec3, v1: Vec3, v2: Vec3) -> Option<f32>` — returns
   `Some(t)` on a forward two-sided hit, `None` otherwise. Epsilons as `const`s.
2. `pub fn ray_aabb_slab(ray: &Ray, box_min: Vec3, box_max: Vec3) -> Option<(f32, f32)>` — returns
   `Some((t_enter, t_exit))` on a hit (`t_enter` may be negative when the origin is inside).
3. `pub fn world_aabb_from_corners(model: &Mat4, lo: Vec3, hi: Vec3, out_min: &mut Vec3, out_max: &mut Vec3)`
   — accumulate (do not reset). Use `Vec3::min`/`max` and `model.transform_point3` for the corner
   transform.
4. `generate_normals(mesh: &mut Mesh)` (free fn or `Mesh` method) — the per-submesh face-normal
   accumulation with the `vertex_offset`/`first_index` indexing and the zero-length fallback.

## Acceptance gate

- `cargo build -p saffron-geometry` + `cargo build --workspace` compile.
- `#[cfg(test)] mod tests` reproduces every assertion from `runPickMathSelfTest` (geometry.cppm:2137):
  center-of-triangle hit at `t≈1`, the `(0.9,0.9)` corner-gap miss, the behind-origin miss, the
  two-sided backface hit, the 45°-rotated unit box growing its world AABB to `≈√0.5` in x with z
  unchanged, and the slab hit/miss cases. All pass with the verbatim epsilons.
- A `generate_normals` `#[test]`: a quad with zeroed normals produces unit-length normals matching the
  face direction; a degenerate triangle yields the `(0,1,0)` fallback.
- `cargo clippy` warning-clean; no `unsafe`.
