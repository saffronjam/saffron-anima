+++
title = 'Transform gizmo'
weight = 4
+++

# Transform gizmo

The selected entity gets a translate/rotate/scale gizmo. It is rendered by the engine, not by the UI: under [present-only mode](../tauri-editor-and-x11-bridge/) ImGui is skipped, so the old ImGuizmo path is gone. The engine draws the gizmo through an overlay pipeline into the viewport at 1x after tonemap, and the webview forwards pointer intent and the chosen mode over the control socket.

## Engine-rendered overlay

The gizmo is part of the scene the engine presents. An overlay pipeline runs at the offscreen's native (1x) resolution *after* the tonemap pass, so the handles stay crisp and unaffected by exposure, MSAA resolve, or post-process. Because it is engine-side, the gizmo lines up exactly with the meshes it manipulates — it shoots through the same [editor camera](../editor-camera/) the scene draws with, with no second projection to keep in sync.

The light and camera billboards are drawn the same way: the engine projects each non-mesh entity to screen space and draws an icon, so a light or camera is selectable in the viewport even though it has no geometry.

## The gizmo-pointer command

The webview owns the DOM and the pointer events; the reparented native window does not receive raw mouse from it. So the [viewport panel](../viewport-panel/) translates each pointer phase into NDC and forwards it with the `gizmo-pointer` command:

```ts
gizmoPointer(phase: GizmoPointerPhase, x: number, y: number): Promise<unknown> {
  return callRaw("gizmo-pointer", { phase, x, y });
}
```

`phase` is one of `hover | begin | drag | end`, and `x`/`y` are NDC in `[-1, 1]` (the same `u*2-1` mapping `pick` uses). A bare move streams `hover` so the engine highlights the handle under the cursor; a press sends `begin`; if the pointer then travels past a few pixels it becomes a `drag` (streamed, throttled, and it sets `store.dragActive` so the reconcile poll won't clobber the in-progress transform); the release always sends `end`, where the engine commits the authoritative transform. A press that *doesn't* move is a click — it [ray-picks](../selection/) instead of dragging.

## Mode and space

The operation (T/R/S) and the world/local space are **one** shared gizmo state on the engine, read and written through `get-gizmo`/`set-gizmo`. There is a single source of truth, so the Topbar buttons, the keyboard shortcuts, and an external `se set-gizmo` all agree.

- The **Topbar** has a T/R/S button group and a World/Local toggle. A click updates `store.gizmo` optimistically and fires `set-gizmo`.
- **W/E/R** map to translate/rotate/scale, bound on the webview (gated off while a text field is focused so typing a value never retargets the gizmo).
- The reconcile poll's `get-gizmo` read folds any external change back into `store.gizmo`, so the Topbar stays correct no matter who set the mode.

## In the code

| What | File | Symbols |
|---|---|---|
| Pointer forwarding | `editor/src/panels/ViewportPanel.tsx` | `gizmoPointer`, the `begin`/`drag`/`end` gesture, `DRAG_THRESHOLD_PX` |
| The gizmo-pointer wrapper | `editor/src/control/client.ts` | `gizmoPointer`, `GizmoPointerPhase` |
| T/R/S + world/local | `editor/src/panels/Topbar.tsx` | `selectOp`, `selectSpace` |
| W/E/R shortcuts | `editor/src/app/useGizmoShortcuts.ts` | `useGizmoShortcuts`, `KEY_TO_OP` |
| Shared gizmo state | `editor/src/state/store.ts` | `gizmo`, `setGizmo` |
| Mode commands (engine) | `control_commands_scene.cpp` | `get-gizmo`, `set-gizmo`, `gizmo-pointer` |

## Related

- [Editor camera](../editor-camera/) — the eye the gizmo and scene share
- [Selection](../selection/) — the click-vs-drag split, and ray-pick on a non-drag click
- [Viewport panel](../viewport-panel/) — where pointer phases are captured and forwarded
- [Scene commands](../../tooling-and-control/scene-commands/) — `get-gizmo`/`set-gizmo`/`gizmo-pointer`
- [Transform and matrices](../../scene-and-ecs/transform-and-matrices/) — the Euler-radians transform the gizmo edits
