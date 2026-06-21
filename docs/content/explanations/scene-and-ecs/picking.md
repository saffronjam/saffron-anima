+++
title = 'Picking'
weight = 7
+++

# Picking

Picking maps a point on screen to the scene entity beneath it. A left-click in the viewport casts a
ray from the camera through the cursor and selects the nearest entity its geometry actually
intersects. Each mesh is tested in two phases: a cheap world-AABB **broad phase** rejects meshes the
ray comes nowhere near, then a ray-vs-triangle **narrow phase** finds the true surface hit. The
narrow phase is what makes a click land on the silhouette and not on the empty air inside a loose
bounding box.

`pick_entity` lives in the assets crate because it needs the cached meshes the asset server owns. It
covers both static `Mesh` and skinned `SkinnedMesh` renderables, so rigged imports are selectable
too. The leaf intersection math (`ray_triangle`, `ray_aabb_slab`, `world_aabb_from_corners`) lives in
the geometry crate, with no ownership or I/O.

## From click to ray

The click arrives as a point in normalized device coordinates, `[-1, 1]`, already matching the
rendered image — y-down, like the flipped clip space the renderer draws with, so the top of the
viewport is `y = -1`. The `pick` command produces it from viewport UV (origin top-left) as
`(u*2-1, v*2-1)`. `pick_entity` rebuilds the same view-projection the renderer used — including the
Vulkan Y-flip that `camera_projection` leaves out — and inverts it to unproject the click:

```rust
let mut proj = camera_projection(camera, aspect);
proj.y_axis.y *= -1.0;                              // match the renderer's clip space
let inv_view_proj = (proj * camera.view).inverse();
let near_h = inv_view_proj * Vec4::new(ndc.x, ndc.y, 0.0, 1.0);  // 0..1 depth: near = 0
let far_h  = inv_view_proj * Vec4::new(ndc.x, ndc.y, 1.0, 1.0);
let origin = near_h.truncate() / near_h.w;
let ray = Ray { origin, dir: (far_h.truncate() / far_h.w - origin).normalize() };
```

Two clip-space points share the same xy: one on the near plane (depth 0, the renderer's `[0, 1]`
depth convention) and one on the far plane (depth 1). They unproject to a world-space origin and
direction. Reusing the renderer's flip and depth range is what makes the ray land where the pixel was
drawn.

## Broad phase: world AABB

For each candidate `pick_entity` builds a world-space AABB from the mesh's eight local-AABB corners
(`world_aabb_from_corners`) and runs the standard ray-AABB slab test (`ray_aabb_slab`). The same
corner helper feeds the renderer's scene-bounds fit, so there is one definition of "world AABB of a
mesh".

```rust
world_aabb_from_corners(&model, mesh_ref.bounds_min, mesh_ref.bounds_max,
                        &mut world_min, &mut world_max);
if ray_aabb_slab(&ray, world_min, world_max).is_none() {
    continue;                                       // ray misses the box → skip
}
```

The box is re-axis-aligned in world space, so a rotated mesh gets a fat, loose fit — a long diagonal
antenna inflates the box with empty air. The broad phase only *rejects*; it never decides the hit, so
that looseness costs nothing but a few extra narrow-phase tests.

## Narrow phase: ray vs. triangle

A mesh whose AABB the ray crosses is tested triangle by triangle. The mesh keeps a CPU copy of its
positions and indices (`GpuMesh::cpu_positions` / `cpu_indices`, filled once at upload), so picking
never reads back from the GPU. Each triangle's three vertices are transformed to world space and run
through a two-sided Möller–Trumbore test (`ray_triangle`), and `nearest_triangle` keeps the closest
forward hit. `ray_triangle` is winding-agnostic (a back face hit counts) and rejects hits at or
behind the origin, so a ray starting inside a mesh reports the surface ahead, not one behind it. The
nearest triangle hit across all meshes wins; a miss everywhere returns `Entity::NULL` and the caller
clears the selection.

## Skinned meshes

A second pass handles `SkinnedMesh`. Its vertices are deformed on the GPU, so picking reproduces the
same skin on the CPU: it builds the joint palette with `joint_matrices` (`world_matrix(bone) *
inverse_bind`) and blends each vertex, $\sum_k w_k \cdot (palette[joint_k] \cdot restPos)$, exactly
as `skin.slang` does. Because the joints already place the vertices in world space, the deformed
positions feed `ray_triangle` directly with no model matrix. The broad-phase AABB unions the
bind-pose box through every joint, mirroring the renderer's skinned scene-bounds fit. Without this
pass rigged models drew but could never be clicked.

> [!TIP]
> Picking depends on the AABB matching the flipped projection. The renderer applies `proj.y_axis.y *=
> -1.0` for drawing; `pick_entity` repeats it. The un-flipped
> [`camera_projection`](../transform-and-matrices/) exists so the gizmo is not mirrored, but picking
> must re-apply the flip or every click would land on the vertically-mirrored object.

## In the code

| What | File | Symbols |
|---|---|---|
| The pick (both phases, both mesh kinds) | `assets/src/render_scene.rs` | `pick_entity`, `nearest_triangle` |
| Ray + intersection math | `geometry/src/picking.rs` · `geometry/src/types.rs` | `Ray`, `ray_triangle`, `ray_aabb_slab`, `world_aabb_from_corners` |
| CPU pick geometry | `rendering/src/resources.rs` | `GpuMesh::cpu_positions`, `cpu_indices`, `cpu_skin`, `bounds_min`, `bounds_max` |
| Mesh resolve | `assets/src/load.rs` | `load_mesh_asset` |
| Skinned deform palette | `scene/src/hierarchy.rs` | `joint_matrices` |
| Matched projection | `scene/src/hierarchy.rs` | `camera_projection`, `CameraView` |
| The `pick` command | `control/src/commands_scene.rs` | `pick` |

## Related
- [Transforms](../transform-and-matrices/) — the un-flipped projection picking re-flips
- [Selection](../../ui-and-editor/selection/) — what consumes the picked entity
- [Editor camera](../../ui-and-editor/editor-camera/) — the eye the pick ray shoots from
