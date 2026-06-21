//! The picking and bounds primitives: ray-triangle, ray-AABB slab, world-AABB
//! accumulation, and smooth-normal recompute.
//!
//! These are leaf math helpers with no ownership or I/O. The numeric epsilons are
//! load-bearing — they decide pick results.
//!
//! Hit tests return the payload through `Option` ("hit or miss, with data").
//! [`world_aabb_from_corners`] keeps *accumulate* semantics (it unions into
//! caller-seeded bounds), because callers union a joint palette across many calls.

use crate::types::{Mesh, Ray};
use glam::{Mat4, Vec3};

/// `|det|` below this rejects a parallel or edge-on triangle as degenerate.
const PARALLEL_EPS: f32 = 1e-8;
/// Hits at or behind the origin (`t <= FORWARD_EPS`) are rejected.
const FORWARD_EPS: f32 = 1e-5;
/// A zero-length accumulated normal (squared length at or below this) falls back
/// to world up.
const NORMAL_LENGTH_EPS: f32 = 1e-12;

/// Two-sided Möller–Trumbore ray-triangle intersection.
///
/// Returns `Some(t)` for a forward hit, where `t` is the ray parameter of the
/// intersection (the ray point is `ray.origin + t * ray.dir`), or `None` for a
/// miss. Two-sided: a back-facing strike still reports a forward hit. The forward
/// gate rejects hits at or behind the origin.
pub fn ray_triangle(ray: &Ray, v0: Vec3, v1: Vec3, v2: Vec3) -> Option<f32> {
    let edge1 = v1 - v0;
    let edge2 = v2 - v0;
    let pvec = ray.dir.cross(edge2);
    let det = edge1.dot(pvec);
    if det > -PARALLEL_EPS && det < PARALLEL_EPS {
        return None;
    }
    let inv_det = 1.0 / det;
    let tvec = ray.origin - v0;
    let u = tvec.dot(pvec) * inv_det;
    if !(0.0..=1.0).contains(&u) {
        return None;
    }
    let qvec = tvec.cross(edge1);
    let v = ray.dir.dot(qvec) * inv_det;
    if v < 0.0 || u + v > 1.0 {
        return None;
    }
    let t = edge2.dot(qvec) * inv_det;
    if t <= FORWARD_EPS {
        return None;
    }
    Some(t)
}

/// Ray-AABB slab intersection.
///
/// Returns `Some((t_enter, t_exit))` when the ray hits the box, or `None` for a
/// miss. `t_enter` may be negative when the origin is inside the box. An
/// axis-aligned ray component yields an infinite slab parameter, which the
/// `min`/`max` fold handles correctly.
pub fn ray_aabb_slab(ray: &Ray, box_min: Vec3, box_max: Vec3) -> Option<(f32, f32)> {
    let inv_dir = Vec3::ONE / ray.dir;
    let t0 = (box_min - ray.origin) * inv_dir;
    let t1 = (box_max - ray.origin) * inv_dir;
    let tlo = t0.min(t1);
    let thi = t0.max(t1);
    let t_enter = tlo.x.max(tlo.y).max(tlo.z);
    let t_exit = thi.x.min(thi.y).min(thi.z);
    if t_exit >= 0.0 && t_enter <= t_exit {
        Some((t_enter, t_exit))
    } else {
        None
    }
}

/// Transform the eight corners of the local box `[lo, hi]` by `model` and
/// *accumulate* their min/max into `out_min`/`out_max`.
///
/// This unions into the passed bounds rather than resetting them: callers seed
/// `out_min`/`out_max` to `+inf`/`-inf` and call this once per element to grow a
/// shared world AABB across many local boxes.
pub fn world_aabb_from_corners(
    model: &Mat4,
    lo: Vec3,
    hi: Vec3,
    out_min: &mut Vec3,
    out_max: &mut Vec3,
) {
    for corner in 0u32..8 {
        let mut p = lo;
        if corner & 1 != 0 {
            p.x = hi.x;
        }
        if corner & 2 != 0 {
            p.y = hi.y;
        }
        if corner & 4 != 0 {
            p.z = hi.z;
        }
        let world = model.transform_point3(p);
        *out_min = out_min.min(world);
        *out_max = out_max.max(world);
    }
}

/// Recompute smooth vertex normals from the mesh triangles.
///
/// Zeroes every normal, accumulates each triangle's face normal
/// (`cross(b - a, c - a)`) into its three vertices — indexing through each
/// submesh's `first_index`/`vertex_offset` so the merged buffers are addressed
/// correctly — then normalizes. A vertex whose accumulated normal is
/// (near-)zero-length falls back to world up `(0, 1, 0)`. The importer uses this
/// when a source omits normals.
pub fn generate_normals(mesh: &mut Mesh) {
    for vertex in &mut mesh.vertices {
        vertex.normal = Vec3::ZERO;
    }
    for submesh in &mesh.submeshes {
        let mut i = 0u32;
        while i + 2 < submesh.index_count {
            let base = (submesh.first_index + i) as usize;
            let offset = submesh.vertex_offset as i64;
            let a = (offset + i64::from(mesh.indices[base])) as usize;
            let b = (offset + i64::from(mesh.indices[base + 1])) as usize;
            let c = (offset + i64::from(mesh.indices[base + 2])) as usize;
            let pos_a = mesh.vertices[a].position;
            let pos_b = mesh.vertices[b].position;
            let pos_c = mesh.vertices[c].position;
            let face_normal = (pos_b - pos_a).cross(pos_c - pos_a);
            mesh.vertices[a].normal += face_normal;
            mesh.vertices[b].normal += face_normal;
            mesh.vertices[c].normal += face_normal;
            i += 3;
        }
    }
    for vertex in &mut mesh.vertices {
        if vertex.normal.dot(vertex.normal) > NORMAL_LENGTH_EPS {
            vertex.normal = vertex.normal.normalize();
        } else {
            vertex.normal = Vec3::new(0.0, 1.0, 0.0);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::{Submesh, Vertex};
    use glam::{Vec2, Vec3};
    use saffron_test_support::close;

    /// The triangle corners and ray directions the pick-math tests share.
    const A: Vec3 = Vec3::new(0.0, 0.0, 0.0);
    const B: Vec3 = Vec3::new(1.0, 0.0, 0.0);
    const C: Vec3 = Vec3::new(0.0, 1.0, 0.0);
    const DOWN: Vec3 = Vec3::new(0.0, 0.0, -1.0);
    const UP: Vec3 = Vec3::new(0.0, 0.0, 1.0);

    #[test]
    fn ray_through_triangle_center_hits_at_one() {
        let ray = Ray {
            origin: Vec3::new(0.25, 0.25, 1.0),
            dir: DOWN,
        };
        let t = ray_triangle(&ray, A, B, C).expect("center ray must hit");
        assert!(close(t, 1.0, 1e-4), "t was {t}");
    }

    #[test]
    fn ray_through_corner_gap_misses() {
        // (0.9, 0.9) is inside the AABB but past the hypotenuse (u + v > 1): a miss.
        let ray = Ray {
            origin: Vec3::new(0.9, 0.9, 1.0),
            dir: DOWN,
        };
        assert!(ray_triangle(&ray, A, B, C).is_none());
    }

    #[test]
    fn triangle_behind_origin_misses() {
        // The whole triangle is behind the origin along the ray; the forward gate rejects it.
        let ray = Ray {
            origin: Vec3::new(0.25, 0.25, -1.0),
            dir: DOWN,
        };
        assert!(ray_triangle(&ray, A, B, C).is_none());
    }

    #[test]
    fn backface_strike_reports_forward_hit() {
        // Striking the back face from below is two-sided: still a forward hit at t ≈ 1.
        let ray = Ray {
            origin: Vec3::new(0.25, 0.25, -1.0),
            dir: UP,
        };
        let t = ray_triangle(&ray, A, B, C).expect("backface ray must hit (two-sided)");
        assert!(close(t, 1.0, 1e-4), "t was {t}");
    }

    /// Builds the 45°-about-Z rotation the AABB test uses.
    fn rot_z_45() -> Mat4 {
        let angle = std::f32::consts::FRAC_PI_4;
        let cs = angle.cos();
        let sn = angle.sin();
        // glam Mat4::from_cols takes columns in column-major order.
        Mat4::from_cols(
            glam::Vec4::new(cs, sn, 0.0, 0.0),
            glam::Vec4::new(-sn, cs, 0.0, 0.0),
            glam::Vec4::new(0.0, 0.0, 1.0, 0.0),
            glam::Vec4::new(0.0, 0.0, 0.0, 1.0),
        )
    }

    #[test]
    fn rotated_unit_box_world_aabb_grows_in_x_keeps_z() {
        let rot = rot_z_45();
        let mut lo = Vec3::splat(f32::MAX);
        let mut hi = Vec3::splat(f32::MIN);
        world_aabb_from_corners(&rot, Vec3::splat(-0.5), Vec3::splat(0.5), &mut lo, &mut hi);
        // A 45° spin grows the unit box's half-extent in x/y to √0.5; z is unchanged.
        assert!(close(hi.x, 0.5f32.sqrt(), 1e-3), "hi.x was {}", hi.x);
        assert!(close(hi.z, 0.5, 1e-3), "hi.z was {}", hi.z);
        // The accumulate is symmetric about the origin.
        assert!(close(lo.x, -0.5f32.sqrt(), 1e-3), "lo.x was {}", lo.x);
        assert!(close(lo.z, -0.5, 1e-3), "lo.z was {}", lo.z);
    }

    #[test]
    fn world_aabb_accumulates_rather_than_resets() {
        // Two boxes union into the same bounds: the second call grows, never clobbers.
        let identity = Mat4::IDENTITY;
        let mut lo = Vec3::splat(f32::MAX);
        let mut hi = Vec3::splat(f32::MIN);
        world_aabb_from_corners(&identity, Vec3::ZERO, Vec3::ONE, &mut lo, &mut hi);
        world_aabb_from_corners(
            &identity,
            Vec3::new(-2.0, -2.0, -2.0),
            Vec3::new(-1.0, -1.0, -1.0),
            &mut lo,
            &mut hi,
        );
        assert_eq!(lo, Vec3::new(-2.0, -2.0, -2.0));
        assert_eq!(hi, Vec3::ONE);
    }

    #[test]
    fn slab_hit_on_overhead_ray() {
        let rot = rot_z_45();
        let mut lo = Vec3::splat(f32::MAX);
        let mut hi = Vec3::splat(f32::MIN);
        world_aabb_from_corners(&rot, Vec3::splat(-0.5), Vec3::splat(0.5), &mut lo, &mut hi);
        let ray = Ray {
            origin: Vec3::new(0.0, 0.0, 2.0),
            dir: DOWN,
        };
        assert!(ray_aabb_slab(&ray, lo, hi).is_some());
    }

    #[test]
    fn slab_miss_when_ray_is_outside() {
        let rot = rot_z_45();
        let mut lo = Vec3::splat(f32::MAX);
        let mut hi = Vec3::splat(f32::MIN);
        world_aabb_from_corners(&rot, Vec3::splat(-0.5), Vec3::splat(0.5), &mut lo, &mut hi);
        let ray = Ray {
            origin: Vec3::new(5.0, 5.0, 2.0),
            dir: DOWN,
        };
        assert!(ray_aabb_slab(&ray, lo, hi).is_none());
    }

    #[test]
    fn slab_t_enter_negative_when_origin_inside() {
        // Origin inside a unit box: the near plane is behind the origin.
        let ray = Ray {
            origin: Vec3::ZERO,
            dir: Vec3::new(0.0, 0.0, -1.0),
        };
        let (t_enter, t_exit) =
            ray_aabb_slab(&ray, Vec3::splat(-1.0), Vec3::splat(1.0)).expect("inside box hits");
        assert!(t_enter < 0.0, "t_enter was {t_enter}");
        assert!(t_exit > 0.0, "t_exit was {t_exit}");
    }

    fn vertex_at(p: Vec3) -> Vertex {
        Vertex {
            position: p,
            normal: Vec3::ZERO,
            uv0: Vec2::ZERO,
        }
    }

    #[test]
    fn generate_normals_on_xy_quad_points_up() {
        // A flat quad in the z=0 plane, wound CCW, must yield unit +Z normals.
        let mut mesh = Mesh {
            vertices: vec![
                vertex_at(Vec3::new(0.0, 0.0, 0.0)),
                vertex_at(Vec3::new(1.0, 0.0, 0.0)),
                vertex_at(Vec3::new(1.0, 1.0, 0.0)),
                vertex_at(Vec3::new(0.0, 1.0, 0.0)),
            ],
            indices: vec![0, 1, 2, 0, 2, 3],
            submeshes: vec![Submesh {
                first_index: 0,
                index_count: 6,
                vertex_offset: 0,
                material_slot: 0,
            }],
        };
        generate_normals(&mut mesh);
        for (i, vertex) in mesh.vertices.iter().enumerate() {
            assert!(
                close(vertex.normal.length(), 1.0, 1e-5),
                "vertex {i} normal not unit length: {}",
                vertex.normal.length()
            );
            assert!(
                vertex.normal.abs_diff_eq(Vec3::new(0.0, 0.0, 1.0), 1e-5),
                "vertex {i} normal not +Z: {:?}",
                vertex.normal
            );
        }
    }

    #[test]
    fn generate_normals_respects_vertex_offset() {
        // Two vertices precede the quad; the submesh's vertex_offset must skip them.
        let mut mesh = Mesh {
            vertices: vec![
                vertex_at(Vec3::new(9.0, 9.0, 9.0)),
                vertex_at(Vec3::new(8.0, 8.0, 8.0)),
                vertex_at(Vec3::new(0.0, 0.0, 0.0)),
                vertex_at(Vec3::new(1.0, 0.0, 0.0)),
                vertex_at(Vec3::new(1.0, 1.0, 0.0)),
                vertex_at(Vec3::new(0.0, 1.0, 0.0)),
            ],
            indices: vec![0, 1, 2, 0, 2, 3],
            submeshes: vec![Submesh {
                first_index: 0,
                index_count: 6,
                vertex_offset: 2,
                material_slot: 0,
            }],
        };
        generate_normals(&mut mesh);
        // The two skipped leading vertices get no face contribution: the up fallback.
        assert!(
            mesh.vertices[0]
                .normal
                .abs_diff_eq(Vec3::new(0.0, 1.0, 0.0), 1e-5)
        );
        assert!(
            mesh.vertices[1]
                .normal
                .abs_diff_eq(Vec3::new(0.0, 1.0, 0.0), 1e-5)
        );
        // The quad vertices (offset 2..) point +Z.
        for vertex in &mesh.vertices[2..] {
            assert!(vertex.normal.abs_diff_eq(Vec3::new(0.0, 0.0, 1.0), 1e-5));
        }
    }

    #[test]
    fn degenerate_triangle_normal_falls_back_to_up() {
        // A zero-area triangle (three coincident points) accumulates a zero normal,
        // which the 1e-12 guard replaces with world up.
        let mut mesh = Mesh {
            vertices: vec![
                vertex_at(Vec3::ZERO),
                vertex_at(Vec3::ZERO),
                vertex_at(Vec3::ZERO),
            ],
            indices: vec![0, 1, 2],
            submeshes: vec![Submesh {
                first_index: 0,
                index_count: 3,
                vertex_offset: 0,
                material_slot: 0,
            }],
        };
        generate_normals(&mut mesh);
        for vertex in &mesh.vertices {
            assert_eq!(vertex.normal, Vec3::new(0.0, 1.0, 0.0));
        }
    }
}
