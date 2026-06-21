+++
title = 'Transforms'
weight = 3
math = true
+++

# Transforms

A transform places an object in the world by combining a translation, a rotation, and a scale into a
single 4x4 model matrix. Every renderable and the camera share the same representation, so one
function builds the matrix they all use.

A `Transform` holds three vectors: translation, scale, and rotation stored as Euler XYZ angles in
radians. `transform_matrix` composes them into the local model matrix. The result is local to the
entity's parent; the [scene hierarchy](../scene-hierarchy/) composes the parent chain into the
cached world matrix that rendering, picking, and the gizmo actually consume.

## The composition

The composition follows the standard TRS order. Read right to left, a point is scaled, then rotated,
then translated:

$$
M = T \cdot R \cdot S
$$

```rust
pub fn transform_matrix(transform: &Transform) -> Mat4 {
    Mat4::from_translation(transform.translation)
        * Mat4::from_quat(quat_from_euler_xyz(transform.rotation))
        * Mat4::from_scale(transform.scale)
}
```

The Euler vector becomes a quaternion via `quat_from_euler_xyz`, the engine's own half-angle
product. The conversion is hand-rolled rather than delegated to `glam::Quat::from_euler`, whose
`EulerRot` conventions do not match the engine's for a generic rotation. This is the single place an
authored Euler becomes a rotation, so the convention here is load-bearing across the whole engine.

## Why rotation is stored as Euler radians

Rotation could be stored as a quaternion. Euler angles are a deliberate authoring choice: a
quaternion edited through a UI must be decomposed back to angles, and that decomposition is ambiguous
and clips at $\pm 90°$ on the middle axis. The inspector edits the stored Euler vector directly,
converting to degrees only for display, which avoids the clip.

The trade is a known one. Euler angles can gimbal-lock, but for hand-authored scene transforms the
clip-free editing is worth more. The conversion to a quaternion happens once, at matrix-build time,
away from the UI. The inverse, `quat_to_euler_zyx`, is the stable $R_z \cdot R_y \cdot R_x$ matrix
extraction the reparent rebase uses; `glam`'s `Quat::to_euler` is numerically unstable at yaw
$\pm 90°$, so the scene owns its own extraction.

## The camera is the same data, inverted

A camera entity has no separate orientation. `primary_camera` finds the first entity with a
`Transform` and a primary `Camera`, takes its cached **world** matrix, and inverts it to get the
view matrix:

```rust
Some(CameraView {
    view: self.world_matrix(entity).inverse(),
    fov: camera.fov,
    near_plane: camera.near_plane,
    far_plane: camera.far_plane,
})
```

A camera is positioned and aimed with the same transform component as any object, and a parented
camera views from its world placement. The view matrix is the inverse of that world matrix.

## The projection lives un-flipped

`camera_projection` returns a plain right-handed GL-clip perspective (`Mat4::perspective_rh_gl`).
The Vulkan Y-flip is not baked in, so the projection has one source of truth. The renderer and
[picking](../picking/) apply `proj.y_axis.y *= -1.0` where they build the actual draw / pick matrix;
the editor gizmo consumes the un-flipped matrix as-is, so it is not mirrored. A flip baked into
`camera_projection` would draw the gizmo backwards.

## In the code

| What | File | Symbols |
|---|---|---|
| The component | `scene/src/component.rs` | `Transform` |
| TRS composition | `scene/src/hierarchy.rs` | `transform_matrix`, `quat_from_euler_xyz` |
| Stable Euler extraction | `scene/src/hierarchy.rs` | `quat_to_euler_zyx`, `set_local_from_matrix` |
| Camera view from transform | `scene/src/hierarchy.rs` | `primary_camera`, `CameraView` |
| Un-flipped projection | `scene/src/hierarchy.rs` | `camera_projection` |
| Where the Y-flip is applied | `assets/src/render_scene.rs` | `render_scene`, `pick_entity` |
| Degree/radian edit | `editor/src/components/fieldRenderer.tsx` | `Transform.rotation`, `RAD_TO_DEG` |

## Related
- [Components](../built-in-components/) — the rest of the value structs
- [Picking](../picking/) — where the flipped projection is rebuilt for the ray
- [Inspector](../../ui-and-editor/inspector/) — the degrees-to-radians edit path
