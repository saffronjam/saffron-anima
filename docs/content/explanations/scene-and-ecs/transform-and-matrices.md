+++
title = 'Transforms'
weight = 3
math = true
+++

# Transforms

A transform places an object in the world by combining a translation, a rotation, and a scale into
a single 4x4 model matrix. Every renderable and the camera share the same representation, so one
function builds the matrix they all use.

In Saffron a `TransformComponent` holds three vectors: translation, scale, and rotation stored as
Euler XYZ angles in radians. `transformMatrix` composes them into the model matrix.

## The composition

The composition follows the standard TRS order. Read right to left, a point is scaled, then
rotated, then translated:

$$
M = T \cdot R \cdot S
$$

```cpp
auto transformMatrix(const TransformComponent& transform) -> glm::mat4
{
    glm::mat4 translation = glm::translate(glm::mat4(1.0f), transform.translation);
    glm::mat4 rotation = glm::mat4_cast(glm::quat(transform.rotation));
    glm::mat4 scale = glm::scale(glm::mat4(1.0f), transform.scale);
    return translation * rotation * scale;
}
```

The Euler vector becomes a quaternion via `glm::quat`, and `glm::mat4_cast` turns that into the
rotation block. Routing through a quaternion keeps the intermediate orthonormal, rather than
multiplying three separate axis matrices.

## Why rotation is stored as Euler radians

Rotation could be stored as a quaternion. Euler angles are a deliberate authoring choice: a
quaternion edited through a UI must be decomposed back to angles, and that decomposition is
ambiguous and clips at $\pm 90°$ on the middle axis. The inspector edits the stored Euler vector
directly, converting to degrees only for display, which avoids the clip.

The trade is a known one. Euler angles can gimbal-lock, but for hand-authored scene transforms the
clip-free editing is worth more. The conversion to a quaternion happens once, at matrix-build time,
away from the UI.

## The camera is the same data, inverted

A camera entity has no separate orientation. `primaryCamera` builds the camera's model matrix from
its `TransformComponent` — translation times rotation, no scale — and inverts it to get the view
matrix:

```cpp
const glm::mat4 model =
    glm::translate(glm::mat4(1.0f), transform.translation) * glm::mat4_cast(glm::quat(transform.rotation));
result.view = glm::inverse(model);
```

A camera is positioned and aimed with the same transform component as any object. The view matrix
is the inverse of that model matrix.

## The projection lives un-flipped

`cameraProjection` returns a plain `glm::perspective` in the GL clip convention. The Vulkan Y-flip
is not baked in, so the projection has one source of truth. The renderer applies `proj[1][1] *=
-1.0f` where it samples for the actual draw and for [picking](../picking/); the editor gizmo
consumes the un-flipped matrix as-is, so it is not mirrored. A flip baked into `cameraProjection`
would draw the gizmo backwards.

## In the code

| What | File | Symbols |
|---|---|---|
| The component | `scene.cppm` | `TransformComponent` |
| TRS composition | `scene.cppm` | `transformMatrix` |
| Camera view from transform | `scene.cppm` | `primaryCamera`, `CameraView` |
| Un-flipped projection | `scene.cppm` | `cameraProjection` |
| Where the Y-flip is applied | `assets.cppm` | `renderScene`, `pickEntity` |
| Degree/radian edit | `editor_components.cpp` | Transform inspector closure |

## Related
- [Components](../built-in-components/) — the rest of the value structs
- [Picking](../picking/) — where the flipped projection is rebuilt for the ray
- [Inspector](../../ui-and-editor/inspector/) — the degrees-to-radians edit path
