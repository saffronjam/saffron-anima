+++
title = 'Skeleton overlay'
weight = 3
+++

# Skeleton overlay

The skeleton overlay draws a line skeleton over the selected rig so you can see the joints an
animation moves. For each bone of the selected entity's `SkinnedMesh` it draws a segment to the
bone's parent joint, a joint dot, and — when enabled — three short RGB axis lines from the bone's
world-rotation basis. It is the editor counterpart to Blender's armature display or UE's skeleton
view: read-only chrome that never edits the rig.

Two choices shape what it looks like:

- **On top, always.** The overlay pass has no depth test, so the skeleton draws over the mesh
  unconditionally (Blender's "In Front", UE's `SDPG_Foreground`). You see every joint even when the
  skin would occlude it. An occluded/dim-when-behind mode is deferred.
- **Edit *and* Play.** The manipulation gizmo and the entity billboards are Edit-only editor chrome,
  but the skeleton draws in every play state — so you can enter Play, watch a clip run, and see the
  bones move with it. The selection re-resolves to the play-scene twin on the play edge, so the same
  selected entity drives the overlay in both modes.

It is scoped to the **selected** entity (or, while previewing an asset, the preview root). Multiple
visible skeletons would z-fight (no depth test), and one rig bounds the vertex count; selecting
another entity moves the overlay. It is **opt-in** — `show` defaults off — so the viewport stays
clean until an animator asks for bones.

## How it draws

`build_skeleton_overlay` reuses the native-overlay primitives — the same `add_line_flat` /
`add_circle_fill` builders the gizmo uses, which pack the analytic-feather `edge` coordinates the
overlay shader needs into each `OverlayVertex`. Per bone it:

1. Reads the joint entity's `world_translation` and projects it to viewport pixels with
   `viewport_project`, skipping joints that project off-screen.
2. Draws a ~2px bone segment to the parent joint — but only when the parent (via
   `Relationship.parent_handle`) is itself a joint, i.e. carries a `Bone` component. Root bones get
   a dot but no segment.
3. Draws a joint dot whose radius is held screen-constant, so joints keep a stable on-screen size as
   you zoom and never vanish at a distance. **Line thickness stays a fixed pixel value** — only the
   dot radius scales.
4. When `axes` is on, draws three short RGB lines from the bone's world rotation basis (X red,
   Y green, Z blue) — nearly free, and the fastest way to read a joint's orientation.

While an asset preview is active, a `highlight_joint` index resolves to its spawned bone entity and
draws in a distinct tint, so the asset editor can call out one bone without blanking the overlay.

## Driving it

The `set-skeleton-overlay` control command toggles it; `get-skeleton-overlay` reads the current
state. Both report `{ show, axes, jointSize }`. All three set-params are optional, so each call
patches only what it passes:

```sh
sa set-skeleton-overlay --show true --axes true   # bones + per-joint axes
sa set-skeleton-overlay --show false              # hide it again
```

The options live on `SceneEditContext` as `SkeletonOverlayOptions`, beside the gizmo state, so they
are session state — not serialized into the project.

## In the code

| What | File | Symbols |
|---|---|---|
| Overlay geometry builder (segments, dots, axes) | `engine/crates/host/src/overlay.rs` | `build_skeleton_overlay`, `submit_scene_edit_overlay` |
| Line + dot primitives (feathered) | `engine/crates/host/src/overlay.rs` | `add_line_flat`, `add_circle_fill`, `viewport_project` |
| Overlay options on the editor context | `engine/crates/sceneedit/src/overlay.rs` | `SkeletonOverlayOptions` |
| Control commands | `engine/crates/control/src/commands_animation.rs` | `set-skeleton-overlay`, `get-skeleton-overlay` |
| Bone source + parent links | `engine/crates/scene/src/component.rs` | `SkinnedMesh`, `Bone`, `Relationship` |

> [!NOTE]
> Bone **picking** (click a segment to select the joint) and bone **labels** (a text system the
> engine does not yet have) are deferred follow-ups. This page is read-only visualization only.

## Related

- [Playback runtime](../playback-runtime/) — the evaluator that moves the joints this draws
- [Animation data model](../animation-data-model/) — the skeleton and pose types it reads
- [Gizmo](../../ui-and-editor/gizmo/) — the native overlay path it shares
- [Asset editor](../../ui-and-editor/asset-editor/) — keys this overlay to the previewed model and adds a highlight channel
