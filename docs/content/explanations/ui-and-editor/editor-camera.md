+++
title = 'Editor camera'
weight = 3
+++

# Editor camera

The editor camera is the viewport's own fly-camera, the eye through which the scene appears while
editing. It is distinct from any `CameraComponent` in the scene: those are authored game cameras,
while the editor camera only controls the editing viewpoint. The scene, the [gizmo](../gizmo/),
and [picking](../selection/) all draw and project through it.

The camera is engine state, not part of the webview. The engine owns the eye, runs the look and move
input, and renders the scene through it via the [present-only path](../tauri-editor-and-x11-bridge/).
Camera, gizmo, and meshes line up because they share one `CameraView`, with no second projection to
keep in sync.

## State and orientation

`EditorCamera` is a plain struct of position and orientation. Orientation is stored as yaw and pitch,
so the look controls add directly to two scalars. At yaw 0 the camera looks down `-Z`, and the
forward vector is rebuilt from the angles when needed:

```cpp
return glm::normalize(glm::vec3(std::cos(pitch) * std::sin(yaw),
                                std::sin(pitch),
                                -std::cos(pitch) * std::cos(yaw)));
```

## Input

Look and move input is native. The engine reads right-mouse-drag for look and WASD, Shift, and Ctrl
for movement directly. A "controlling" latch keeps control while the view swings off the panel
mid-drag. Movement is frame-rate independent (`moveSpeed * dt`) along the forward and right basis, and
pitch is clamped just shy of vertical so the camera never flips.

The camera is also scriptable over the control socket through `get-camera` and `set-camera`, which
merge the fly-cam fields the same way the transform commands do:

```ts
getCamera(): Promise<EditorCamera> { return call("get-camera"); }
setCamera(camera: Partial<EditorCamera>): Promise<EditorCamera> { return call("set-camera", camera); }
```

`se focus` moves the eye through this path: it reads the target transform and pulls the camera back
along its forward axis. The native input and the control commands stay consistent because both read
and write the one engine-side camera.

## Feeding the renderer and the gizmo

The editor camera converts to a `CameraView`, the same view type a scene camera produces, so
`renderScene`, the gizmo overlay, and the pick ray all consume one view:

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

The view holds only the world-to-view transform and the projection params. The projection matrix, and
the Vulkan Y-flip, is built where it is used.

## In the code

| What | File | Symbols |
|---|---|---|
| State | `editor.cppm` | `EditorCamera` |
| Forward from yaw/pitch | `editor_camera.cpp` | `editorCameraForward` |
| Input + movement | `editor_camera.cpp` | `updateEditorCamera`, the `controlling` latch |
| Convert to a view | `editor_camera.cpp` | `editorCameraView` |
| Camera commands (engine) | `control_commands_scene.cpp` | `get-camera`, `set-camera`, `focus` |
| Camera wrappers (client) | `editor/src/control/client.ts` | `getCamera`, `setCamera` |

## Related

- [Gizmo](../gizmo/) — manipulates through the same `CameraView`
- [Selection](../selection/) — click-pick builds its ray from this camera
- [Scene commands](../../tooling-and-control/scene-commands/) — `get-camera`/`set-camera`/`focus`
