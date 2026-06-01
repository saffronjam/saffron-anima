+++
title = 'Transform gizmo'
weight = 4
+++

# Transform gizmo

The selected entity gets an in-viewport translate/rotate/scale gizmo, drawn with ImGuizmo; W, E, and R cycle the operation. Three things make it behave: where the gizmo draws (into the viewport window, not over the whole screen), which projection it gets (the un-flipped one), and how it writes a drag back onto a Euler-angle transform without snapping.

## Drawn into the viewport window

A screen-wide gizmo overlay would not clip to the dockable viewport, and its mouse hit-testing would fight the rest of the UI. Instead the gizmo draws into the "Viewport" window's draw list and is scoped to the image's rectangle:

```cpp
ImGui::Begin("Viewport");
ImGuizmo::SetOrthographic(false);
ImGuizmo::SetDrawlist();
ImGuizmo::SetRect(imagePos.x, imagePos.y, imageSize.x, imageSize.y);
```

`imagePos` / `imageSize` are the viewport image's screen rect, captured by [the viewport panel](../viewport-panel/). Living in that window's draw list, the gizmo clips to the panel and consumes mouse input there like any other widget.

## Un-flipped projection

The renderer flips the projection's Y to match Vulkan's clip space, but that flip stays local to the renderer. The gizmo is handed the un-flipped projection — the same camera draws the scene with the flip and feeds the gizmo without it, and they still line up on screen.

> [!WARNING]
> Pass the un-flipped projection. ImGuizmo expects a standard projection, so handing it the renderer's Y-flipped matrix mirrors the gizmo vertically and inverts every drag. The caller builds `cameraProjection(cam, aspect)` without the `proj[1][1] *= -1` the renderer applies.

## Cycling the operation

W/E/R switch translate/rotate/scale, but only when it's safe to read those keys as shortcuts:

```cpp
if (hovered && !ImGuizmo::IsUsing() && !ImGui::IsAnyItemActive() &&
    !ImGui::IsMouseDown(ImGuiMouseButton_Right))
{
    if (ImGui::IsKeyPressed(ImGuiKey_W)) { ctx.gizmoOp = ImGuizmo::TRANSLATE; }
    if (ImGui::IsKeyPressed(ImGuiKey_E)) { ctx.gizmoOp = ImGuizmo::ROTATE; }
    if (ImGui::IsKeyPressed(ImGuiKey_R)) { ctx.gizmoOp = ImGuizmo::SCALE; }
}
```

The right-mouse guard is the link to [the editor camera](../editor-camera/): while you fly with RMB+WASD, W means "move forward", not "switch to translate". The guards also skip the shortcut while a gizmo drag is in progress or another widget is being edited.

## Writing the drag back without snapping

ImGuizmo manipulates a `glm::mat4` model matrix. Writing it back into the entity's `TransformComponent` means decomposing the matrix into translation, rotation, and scale. The catch: the transform stores rotation as Euler XYZ radians (so the inspector can edit past 90° without gimbal clipping), and a round-trip through a quaternion and back to Euler can pick a different but equivalent angle set, which would make a pure translate drag visibly snap the rotation.

The fix is to apply rotation as a *delta* on the stored Euler, not to overwrite it:

```cpp
if (glm::decompose(model, scale, rotation, translation, skew, perspective))
{
    const glm::vec3 deltaEuler = glm::eulerAngles(rotation) - transform.rotation;
    transform.translation = translation;
    transform.rotation += deltaEuler;
    transform.scale = scale;
}
```

Translation and scale come straight off the decomposition; only the rotation *change* since the last frame is added to the stored Euler. A translate-only drag produces a near-zero delta, so the stored rotation is left untouched.

## In the code

| What | File | Symbols |
|---|---|---|
| Gizmo draw + write-back | `editor_gizmo.cpp` | `drawGizmo`, `glm::decompose` |
| Op state + cycle | `editor_gizmo.cpp` | `ctx.gizmoOp`, the W/E/R guards |
| Viewport-window scoping | `editor_gizmo.cpp` | `ImGuizmo::SetDrawlist`, `SetRect` |
| Un-flipped projection (caller) | `editor_app.cppm` | `cameraProjection(cam, aspect)` (no Y-flip) |

## Related

- [Editor camera](../editor-camera/) — the RMB guard, and the shared `CameraView`
- [Transform and matrices](../../scene-and-ecs/transform-and-matrices/) — the Euler-radians transform the gizmo edits
- [Viewport panel](../viewport-panel/) — supplies the image rect the gizmo clips to
