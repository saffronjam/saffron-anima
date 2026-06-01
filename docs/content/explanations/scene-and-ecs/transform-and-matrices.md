+++
title = 'Transforms'
weight = 3
math = true
+++

# Transforms

A `TransformComponent` is three vectors: translation, scale, and rotation as Euler XYZ in
radians. `transformMatrix` composes them into the model matrix every renderable and the camera
share.

## The composition

The order is the standard TRS. Read right to left, a point is scaled, then rotated, then
translated:

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
rotation block. Going through a quaternion keeps the intermediate orthonormal rather than
multiplying three separate axis matrices.

## Why Euler radians in storage

Rotation could have been a quaternion. Storing Euler angles is a deliberate authoring choice: a
quaternion edited through a UI needs decomposition back to angles, and that decomposition is
ambiguous and clips at $\pm 90°$ on the middle axis. The inspector edits the stored Euler vector
directly (converting to degrees only for display), which sidesteps that.

The trade is the usual one: Euler angles can gimbal-lock, but for hand-authored scene transforms
the clip-free editing wins. The conversion to a quaternion happens once, at matrix-build time,
where it does not fight the UI.

## The camera is the same data, inverted

A camera entity has no separate orientation. `primaryCamera` builds the camera's model matrix
from its `TransformComponent` — translation times rotation, no scale — and inverts it to get the
view matrix:

```cpp
const glm::mat4 model =
    glm::translate(glm::mat4(1.0f), transform.translation) * glm::mat4_cast(glm::quat(transform.rotation));
result.view = glm::inverse(model);
```

So a camera is positioned and aimed with the same transform component as any object; the view
matrix is where its model matrix lands when you invert it.

## The projection lives un-flipped

`cameraProjection` returns a plain `glm::perspective` (GL clip convention). The Vulkan Y-flip is
not baked here, so the projection has one source of truth. The renderer applies
`proj[1][1] *= -1.0f` where it samples for the actual draw and for [picking](../picking/); the
editor gizmo consumes the un-flipped matrix as-is so it is not mirrored. If the flip lived in
`cameraProjection`, the gizmo would draw backwards.

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
