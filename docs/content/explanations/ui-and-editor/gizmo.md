+++
title = 'Transform gizmo'
weight = 4
+++

# Transform gizmo

A transform gizmo is an on-screen widget that edits a selected entity's translation, rotation, or scale by dragging handles drawn over it in the viewport. It is the direct-manipulation alternative to typing transform values into a panel.

The gizmo is rendered by the engine, not a UI toolkit. The engine has no ImGui/ImGuizmo; under [the compositing path](../viewport-compositing/) it draws the handles itself as a native overlay, and the webview forwards pointer intent and the chosen mode over the control socket.

## Engine-rendered overlay

The gizmo is part of the scene the engine presents. An overlay pipeline runs at the offscreen's native (1x) resolution *after* the tonemap pass, so the handles stay crisp and unaffected by exposure, MSAA resolve, or post-process. Because it is engine-side, the gizmo lines up exactly with the meshes it manipulates: it projects through the same [editor camera](../editor-camera/) the scene draws with, so there is no second projection to keep in sync.

Drawing after the resolve also means the scene's [AA mode](../../anti-aliasing/aa-modes/) can never smooth the overlay, so it anti-aliases itself analytically: each primitive is widened by a pixel per side and carries signed edge coordinates plus half-extents, and the fragment shader turns the interpolated distances into a coverage alpha. A line feathers across its thickness, a filled plane quad across both of its directions — so lines, rotation rings, and plane handles are smooth at every AA setting, including off.

Each mode draws only its own handles: translate shows the three axis lines plus the two-axis plane quads, rotate shows only the three rings, scale shows the axis lines with box ends and a center box for uniform scale. The plane quads are drawn from the *same* projected corners the hit-test checks (`gizmoPlaneCorners`), so the handle under the cursor is always the one that activates.

The light and camera billboards are drawn the same way. The engine projects each non-mesh entity to screen space and draws an icon, so a light or camera is selectable in the viewport even though it has no geometry.

## The gizmo-pointer command

The handles are drawn into the frame stream the webview paints, so the engine receives no raw mouse from the canvas. The [viewport panel](../viewport-panel/) therefore translates each pointer phase into NDC and forwards it with the `gizmo-pointer` command:

```ts
gizmoPointer(phase: GizmoPointerPhase, x: number, y: number): Promise<unknown> {
  return callRaw("gizmo-pointer", { phase, x, y });
}
```

`phase` is one of `hover | begin | drag | end`, and `x`/`y` are NDC in `[-1, 1]` (the same `u*2-1` mapping `pick` uses). The phases map to a gesture:

- A bare move streams `hover`, so the engine highlights the handle under the cursor.
- A press sends `begin`.
- Travel past a few pixels turns the gesture into a `drag`, streamed and throttled; it sets `store.dragActive` so the reconcile poll will not clobber the in-progress transform. Each applied drag bumps the engine's `sceneVersion`, so the poll re-inspects and the Inspector's transform fields track the drag live.
- The release always sends `end`, where the engine commits the authoritative transform.

A press that does not move is a click, and [ray-picks](../selection/) instead of dragging.

## Mode and space

The operation (T/R/S) and the world/local space are **one** shared gizmo state on the engine, read and written through `get-gizmo`/`set-gizmo`. A single source of truth keeps the Topbar buttons, the keyboard shortcuts, and an external `se set-gizmo` in agreement.

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
| Overlay geometry (engine) | `engine/source/saffron/host/host.cppm` | `buildNativeGizmo`, `addLine`, `addQuad` |
| Hit-test / shared geometry (engine) | `scene_edit_gizmo.cpp` | `hitNativeGizmo`, `gizmoPlaneCorners`, `ringBasis` |
| Analytic AA (engine) | `gizmo_overlay.slang`, `renderer_types.cppm` | `fragmentMain`, `OverlayVertex` |

## Related

- [Editor camera](../editor-camera/) — the eye the gizmo and scene share
- [Selection](../selection/) — the click-vs-drag split, and ray-pick on a non-drag click
- [Viewport panel](../viewport-panel/) — where pointer phases are captured and forwarded
- [Scene commands](../../tooling-and-control/scene-commands/) — `get-gizmo`/`set-gizmo`/`gizmo-pointer`
- [Transform and matrices](../../scene-and-ecs/transform-and-matrices/) — the Euler-radians transform the gizmo edits
