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

Light billboards are drawn the same way. Camera entities use editor-only helpers instead: a
black system camera model is appended to the edit-mode draw list, and the overlay draws a
dark-orange frustum from the camera FOV and near/far planes, capped by `frustumMaxDistance`. The
`CameraComponent` can hide either helper with `showModel` or `showFrustum`; play mode renders
neither one.

The frustum is **depth-tested against the scene depth**, so the camera model and any geometry in
front of it occlude the lines instead of the frustum painting over them. The overlay pass binds the
1x scene depth as a read-only attachment (depth test on, depth write off), and only the frustum
reads it — the manipulation handles and billboards still draw on top, so they stay visible and
grabbable even behind a wall. Each frustum vertex carries its projected NDC depth, computed through
the same editor camera the scene draws with, so the values line up with the depth buffer exactly.
When an [AA mode](../../anti-aliasing/aa-modes/) makes the scene multisampled, the scene pass first
resolves its depth into the 1x buffer the overlay reads.

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

The operation (T/R/S), the world/local space, and the preserve-children flag are **one** shared gizmo state on the engine, read and written through `get-gizmo`/`set-gizmo`. A single source of truth keeps the Topbar buttons, the keyboard shortcuts, and an external `sa set-gizmo` in agreement.

- The **Topbar** has a T/R/S button group, a World/Local toggle, and an anchor-icon preserve-children toggle. A click updates `store.gizmo` optimistically and fires `set-gizmo`.
- **W/E/R** are the *default* translate/rotate/scale shortcuts, bound on the webview (gated off while a text field is focused so typing a value never retargets the gizmo). They are rebindable in [Editor Settings](../editor-settings/); the Topbar tooltips show whatever key is currently bound.
- The reconcile poll's `get-gizmo` read folds any external change back into `store.gizmo`, so the Topbar stays correct no matter who set the mode.

## Preserve children

By default a parent's transform carries its whole subtree — that is what parenting means. Preserve children (Blender's *Affect Only Parents*, Maya's *Preserve Children*) inverts that for the moment you want to adjust the parent alone: with the toggle on, transforming a parent rebases each direct child's local TRS against the parent's new world matrix, so the children visually stay put.

A drag freezes every direct child's world matrix at `begin` alongside the parent snapshot, and each applied frame rewrites the child locals from those frozen matrices — the same `local = world⁻¹ · childWorld` rebase [reparenting](../../scene-and-ecs/scene-hierarchy/) uses, so there is no per-frame drift. The Inspector path (`set-transform`) does the same around its one write. Grandchildren need no handling: they are relative to the rebased child and follow it.

The rebase is TRS-only, like the reparent decompose: a rotated child under a non-uniformly scaled parent would need shear to hold its pose exactly, and `TransformComponent` cannot represent shear — keep parents you scale non-uniformly unrotated relative to their children (or parent through a unit-scale empty).

## In the code

| What | File | Symbols |
|---|---|---|
| Pointer forwarding | `editor/src/panels/ViewportPanel.tsx` | `gizmoPointer`, the `begin`/`drag`/`end` gesture, `DRAG_THRESHOLD_PX` |
| The gizmo-pointer wrapper | `editor/src/control/client.ts` | `gizmoPointer`, `GizmoPointerPhase` |
| T/R/S + world/local + preserve children | `editor/src/panels/Topbar.tsx` | `selectOp`, `selectSpace`, `togglePreserveChildren` |
| Child rebase (engine) | `scene_edit_gizmo.cpp`, `scene.cppm` | `rebasePreservedChildren`, `setLocalFromMatrix` |
| W/E/R shortcuts | `editor/src/app/useGizmoShortcuts.ts` | `useGizmoShortcuts`, `GIZMO_COMMANDS`, `matchesBinding` |
| Shared gizmo state | `editor/src/state/store.ts` | `gizmo`, `setGizmo` |
| Mode commands (engine) | `control_commands_scene.cpp` | `get-gizmo`, `set-gizmo`, `gizmo-pointer` |
| Overlay geometry (engine) | `engine/source/saffron/host/host.cppm` | `buildNativeGizmo`, `buildSceneEditCameraFrustums`, `addLine`, `submitNativeGizmo` |
| Hit-test / shared geometry (engine) | `scene_edit_gizmo.cpp` | `hitNativeGizmo`, `gizmoPlaneCorners`, `ringBasis` |
| Analytic AA (engine) | `gizmo_overlay.slang`, `renderer_types.cppm` | `fragmentMain`, `OverlayVertex` |
| Depth-tested overlay (engine) | `renderer.cppm`, `renderer_pipelines.cpp` | `newOverlayPipeline` (`depthTest`), the `editor-overlay` pass depth attachment, `submitOverlay` |

## Related

- [Editor camera](../editor-camera/) — the eye the gizmo and scene share
- [Play mode](../play-mode/) — the gizmo is hidden during play (its writes would be discarded with the play scene)
- [Selection](../selection/) — the click-vs-drag split, and ray-pick on a non-drag click
- [Viewport panel](../viewport-panel/) — where pointer phases are captured and forwarded
- [Scene commands](../../tooling-and-control/scene-commands/) — `get-gizmo`/`set-gizmo`/`gizmo-pointer`
- [Transform and matrices](../../scene-and-ecs/transform-and-matrices/) — the Euler-radians transform the gizmo edits
