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
input, and renders the scene through it via the [present-only path](../tauri-editor-and-viewport-transport/).
Camera, gizmo, and meshes line up because they share one `CameraView`, with no second projection to
keep in sync.

## State and orientation

`SceneEditCamera` is a plain struct of position and orientation. Orientation is stored as yaw and pitch,
so the look controls add directly to two scalars. At yaw 0 the camera looks down `-Z`, and the
forward vector is rebuilt from the angles when needed:

```cpp
return glm::normalize(glm::vec3(std::cos(pitch) * std::sin(yaw),
                                std::sin(pitch),
                                -std::cos(pitch) * std::cos(yaw)));
```

## Input

Look and move input is native — the engine reads the raw SDL stream from its own window. While the
**right mouse button is held** the engine takes the X11 input focus, grabs the keyboard, and locks the
pointer (mouse grab + relative-mouse mode), then reads right-drag for look and WASD + Space/Shift for
movement; releasing the button (or losing focus, or pressing Escape) releases everything and hands the
focus back to the editor.

The focus move is what lets the keys reach the engine at all: its window is an X11 child reparented
under the Tauri webview, and an X11 child holds no keyboard focus — key events go to the focused
top-level (the webview), and SDL only applies grabs and routes relative motion to a focused window. So
the engine borrows the focus for exactly the duration of the hold (the XEmbed model). Mouse buttons
arrive regardless — X11 delivers them to the window under the cursor — which is why the gizmo needs no
focus and why RMB-down can bootstrap the fly. A "controlling" latch keeps control while the view
swings off the panel mid-drag; movement is frame-rate independent (`moveSpeed * dt`) along the forward
and right basis, and pitch is clamped just shy of vertical so the camera never flips.

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
auto sceneEditCameraView(const SceneEditCamera& camera) -> CameraView
{
    CameraView result;
    const glm::vec3 forward = sceneEditCameraForward(camera);
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
| State | `scene_edit_context.cppm` | `SceneEditCamera`, `SceneEditCameraInput` |
| Forward from yaw/pitch | `scene_edit_camera.cpp` | `sceneEditCameraForward` |
| Move/look math | `scene_edit_camera.cpp` | `updateSceneEditCamera`, the `controlling` latch |
| Convert to a view | `scene_edit_camera.cpp` | `sceneEditCameraView` |
| SDL input + keyboard grab | `host.cppm` | RMB-fly event sink, `endSceneEditFly` |
| Camera commands (engine) | `control_commands_scene.cpp` | `get-camera`, `set-camera`, `focus` |
| Camera wrappers (client) | `editor/src/control/client.ts` | `getCamera`, `setCamera` |

## Related

- [Gizmo](../gizmo/) — manipulates through the same `CameraView`
- [Selection](../selection/) — click-pick builds its ray from this camera
- [Scene commands](../../tooling-and-control/scene-commands/) — `get-camera`/`set-camera`/`focus`
