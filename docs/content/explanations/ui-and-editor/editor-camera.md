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
input, and renders the scene through it via the [compositing path](../viewport-compositing/).
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

Look and move input streams over the control plane — the engine's hidden window receives no
events. While the **right mouse button is held** over the [viewport panel](../viewport-panel/),
the webview takes pointer lock and sends `fly-input` snapshots: accumulated relative mouse
deltas plus the WASD/Space/Shift key state, at roughly the pointer-event cadence. The engine
stores the latest snapshot on the edit context and drains the accumulated look delta once per
frame into `updateSceneEditCamera`, so a burst of samples between frames is never lost.
Releasing the button — or Escape, which exits pointer lock natively — sends `active:false`
and ends the fly.

Samples arrive at ~60Hz, slower than the engine renders, so applying each delta whole would
staircase the view. The drained delta instead lands in a pending accumulator (`lookPending`)
that yaw and pitch consume through an exponential filter — the same ~25ms time constant the
[gizmo](../gizmo/) uses for drag samples — turning the sample steps into continuous motion at
about two frames of lag. The filter only reshapes timing; every pixel of input still lands.

A "controlling" latch keeps control while the view swings off the panel mid-drag; movement is
frame-rate independent (`moveSpeed * dt`) along the forward and right basis, and pitch is
clamped just shy of vertical so the camera never flips.

The camera is also scriptable over the control socket through `get-camera` and `set-camera`, which
merge the fly-cam fields the same way the transform commands do:

```ts
getCamera(): Promise<EditorCamera> { return call("get-camera"); }
setCamera(camera: Partial<EditorCamera>): Promise<EditorCamera> { return call("set-camera", camera); }
```

`sa focus` moves the eye through this path: it reads the target transform and pulls the camera back
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

## Persistence

The eye is part of the project: saving writes an `editorCamera` block (position, yaw, pitch, fov)
into [`project.json`](../../geometry-and-assets/project-serialization/), and opening a project
restores it, so a reopened project shows the framing it was saved with. Projects saved before the
block existed keep the current camera. The tuning fields (speeds, planes) are not persisted —
they are session preferences, not framing.

## In the code

| What | File | Symbols |
|---|---|---|
| State | `scene_edit_context.cppm` | `SceneEditCamera`, `SceneEditCameraInput` |
| Forward from yaw/pitch | `scene_edit_camera.cpp` | `sceneEditCameraForward` |
| Move/look math | `scene_edit_camera.cpp` | `updateSceneEditCamera`, `lookPending`, the `controlling` latch |
| Convert to a view | `scene_edit_camera.cpp` | `sceneEditCameraView` |
| Persisted view ↔ JSON | `scene_edit_camera.cpp` | `sceneEditCameraToJson`, `sceneEditCameraFromJson` |
| Fly snapshot drain | `host.cppm` | the `onUpdate` `flyInput` drain |
| Pointer-lock streaming (webview) | `editor/src/panels/ViewportPanel.tsx` | the fly `useEffect` |
| Camera commands (engine) | `control_commands_scene.cpp` | `fly-input`, `get-camera`, `set-camera`, `focus` |
| Camera wrappers (client) | `editor/src/control/client.ts` | `getCamera`, `setCamera` |

## Related

- [Gizmo](../gizmo/) — manipulates through the same `CameraView`
- [Play mode](../play-mode/) — the viewport renders through the scene camera during play, falling back to this one
- [Selection](../selection/) — click-pick builds its ray from this camera
- [Scene commands](../../tooling-and-control/scene-commands/) — `get-camera`/`set-camera`/`focus`
