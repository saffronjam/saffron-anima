+++
title = 'Editor camera'
weight = 3
+++

# Editor camera

The viewport has its own fly-camera, the scene-view eye. It is separate from any `CameraComponent` in the scene: those are game cameras you author and place, while the editor camera is just how you look around while editing. It is **engine state** — the same `EditorCamera` the old editor used — and the scene, the [gizmo](../gizmo/), and [picking](../selection/) all draw and project through it. Hold the right mouse button over the viewport to look, WASD to move, Shift up, Ctrl down.

## Still engine-side

Nothing about the camera moved into the webview. The engine owns the eye, runs the look/move input, and renders the scene through it via the [present-only path](../tauri-editor-and-x11-bridge/) — so the camera, the gizmo, and the meshes line up exactly because they share one `CameraView` with no second projection to keep in sync.

`EditorCamera` is a plain struct of position and orientation. Orientation is yaw and pitch (so the look controls are a direct add to two scalars); at yaw 0 the camera looks down `-Z`, and the forward vector is rebuilt from the angles when needed:

```cpp
return glm::normalize(glm::vec3(std::cos(pitch) * std::sin(yaw),
                                std::sin(pitch),
                                -std::cos(pitch) * std::cos(yaw)));
```

## Input: native, with a control fallback

The look + move input stays native: the engine reads RMB-drag for look and WASD/Shift/Ctrl for movement directly, with a "controlling" latch so swinging the view off the panel mid-drag doesn't drop control. Movement is frame-rate independent (`moveSpeed * dt`) along the forward/right basis, and pitch is clamped just shy of vertical so the camera never flips.

The camera is also fully scriptable over the control socket through `get-camera`/`set-camera`, which merge the fly-cam fields the same way the transform commands do:

```ts
getCamera(): Promise<EditorCamera> { return call("get-camera"); }
setCamera(camera: Partial<EditorCamera>): Promise<EditorCamera> { return call("set-camera", camera); }
```

That is how `se focus` (and any future UI "frame the selection" affordance) moves the eye — it reads the target transform and pulls the camera back along its forward axis. The two paths are consistent because both read and write the one engine-side camera.

## Feeding the renderer and the gizmo

The editor camera converts to a `CameraView`, the same view type a scene camera produces, so `renderScene`, the gizmo overlay, and the pick ray all consume one view:

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

The view holds only the world-to-view transform and the projection params; the projection matrix (and the Vulkan Y-flip) is built where it's used.

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
