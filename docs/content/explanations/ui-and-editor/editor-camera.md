+++
title = 'Editor camera'
weight = 3
+++

# Editor camera

The viewport has its own fly-camera, the scene-view eye. It is separate from any `CameraComponent` in the scene: those are game cameras you author and place, while the editor camera is just how you look around while editing. Hold the right mouse button over the viewport to look, WASD to move, Shift up, Ctrl down.

## State

`EditorCamera` is a plain struct of position and orientation, no class hierarchy. Orientation is yaw and pitch in degrees rather than a stored matrix or quaternion, so the look controls are a direct add to two scalars. At yaw 0 the camera looks down `-Z`, and the forward vector is rebuilt from the angles whenever it's needed:

```cpp
return glm::normalize(glm::vec3(std::cos(pitch) * std::sin(yaw),
                                std::sin(pitch),
                                -std::cos(pitch) * std::cos(yaw)));
```

## Driving it from ImGui input

`updateEditorCamera` runs every frame from `onUi`, because it reads ImGui's input state, which is only valid there. It gates on the right mouse button and whether the viewport is hovered:

```cpp
const bool rmb = ImGui::IsMouseDown(ImGuiMouseButton_Right);
if (!rmb || !(viewportHovered || camera.controlling))
{
    camera.controlling = false;
    return;
}
camera.controlling = true;  // latch so the drag keeps control if it leaves the rect
```

The `controlling` latch is what makes a drag feel right. Control starts only while the cursor is over the viewport, but once RMB is held the latch stays set even if the cursor leaves the panel mid-drag. Without it, swinging the view fast enough to move the cursor off the viewport would drop control and stop the look. The latch clears the moment RMB releases.

Look adds the mouse delta to yaw and pitch, with pitch clamped just shy of straight up or down so the camera never flips. Movement is frame-rate independent — `moveSpeed * dt` — along the forward/right basis, with Shift and Ctrl moving along world up regardless of where the camera looks.

## Feeding the renderer and the gizmo

The scene, the gizmo, and click-picking all draw and project through the same eye, so the editor camera converts to a `CameraView`, the same view type a scene camera produces:

```cpp
auto editorCameraView(const EditorCamera& camera) -> CameraView
{
    CameraView result;
    const glm::vec3 forward = editorCameraForward(camera);
    result.view = glm::lookAt(camera.position, camera.position + forward, glm::vec3(0,1,0));
    result.fov = camera.fov;
    ...
    return result;
}
```

`renderScene`, the [gizmo](../gizmo/), and [picking](../selection/) all consume this `CameraView`, so manipulated objects line up exactly with what you see. The view holds only the world-to-view transform and the projection params; the projection matrix (and the Vulkan Y-flip) is built where it's used, which is why the gizmo and the renderer can disagree about the flip on purpose.

## In the code

| What | File | Symbols |
|---|---|---|
| State | `editor_context.cppm` | `EditorCamera` |
| Forward from yaw/pitch | `editor_camera.cpp` | `editorCameraForward` |
| Input + movement | `editor_camera.cpp` | `updateEditorCamera`, the `controlling` latch |
| Convert to a view | `editor_camera.cpp` | `editorCameraView` |

## Related

- [Gizmo](../gizmo/) — manipulates through the same `CameraView`
- [Selection](../selection/) — click-pick builds its ray from this camera
- [Viewport panel](../viewport-panel/) — supplies the hover flag that gates control
