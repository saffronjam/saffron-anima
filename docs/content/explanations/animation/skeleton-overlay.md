+++
title = 'Skeleton overlay'
weight = 3
+++

# Skeleton overlay

The skeleton overlay draws a line skeleton over the selected rig so you can see the joints an
animation moves. For each bone of the selected entity's `SkinnedMeshComponent` it draws a segment
to the bone's parent joint, a joint dot, and — when enabled — three short RGB axis lines from the
bone's world-rotation basis. It is the editor counterpart to Blender's armature display or UE's
skeleton view: read-only chrome that never edits the rig.

Two choices shape what it looks like:

- **On top, always.** The overlay pass has no depth test, so the skeleton draws over the mesh
  unconditionally (Blender's "In Front", UE's `SDPG_Foreground`). You see every joint even when the
  skin would occlude it. An occluded/dim-when-behind mode is deferred.
- **Edit *and* Play.** The manipulation gizmo and the entity billboards are Edit-only editor chrome,
  but the skeleton draws in every play state — so you can hit Play, watch a clip run, and see the
  bones move with it. The selection re-resolves to the play-scene twin at `enterPlay`, so the same
  selected entity drives the overlay in both modes.

It is scoped to the **selected** entity. Multiple visible skeletons would z-fight (no depth test),
and one rig bounds the vertex count; selecting another entity moves the overlay. It is **opt-in** —
`show` defaults off — so the viewport stays clean until an animator asks for bones.

## How it draws

`buildSkeletonOverlay` reuses the native-overlay primitives — the same `addLine` / `addCircleFill`
builders the gizmo uses, which pack the analytic-feather `edge` coordinates the overlay shader
needs. Per bone it:

1. Resolves the joint entity through `SkinnedMeshComponent.boneHandles[]` (skipping `entt::null`)
   and projects its `worldTranslation` to viewport pixels with `viewportProject`.
2. Draws a ~2px bone segment to the parent joint — but only when the parent (via
   `RelationshipComponent.parentHandle`) is itself a joint, i.e. carries a `BoneComponent`. Root
   bones get a dot but no segment.
3. Draws a joint dot whose radius is held screen-constant: `max(rMin, distance × k)`, where
   `distance` is the camera-to-joint distance. The dot keeps a stable on-screen size as you zoom,
   so joints never vanish at a distance. **Line thickness stays a fixed pixel value** — only the
   dot radius scales.
4. When `axes` is on, draws three short RGB lines from the bone's `worldRotation` basis (X red,
   Y green, Z blue) — nearly free, and the fastest way to read a joint's orientation.

## Driving it

The `set-skeleton-overlay` control command toggles it; `get-skeleton-overlay` reads the current
state. Both report `{ show, axes, jointSize }`. All three set-params are optional, so each call
patches only what it passes:

```sh
se set-skeleton-overlay --show true --axes true   # bones + per-joint axes
se set-skeleton-overlay --show false              # hide it again
```

The options live on `SceneEditContext` as `SkeletonOverlayOptions`, beside the gizmo state, so they
are session state — not serialized into the project.

## In the code

| What | File | Symbols |
|---|---|---|
| Overlay geometry builder (segments, dots, axes) | `host.cppm` | `buildSkeletonOverlay`, `submitSceneEditOverlay` |
| Overlay options on the editor context | `scene_edit_context.cppm` | `SkeletonOverlayOptions` |
| Control commands | `control_commands_animation.cpp` | `set-skeleton-overlay`, `get-skeleton-overlay` |
| Line + dot primitives (feathered) | `host.cppm` | `addLine`, `addCircleFill` |
| Bone source + parent links | `scene.cppm` | `SkinnedMeshComponent`, `BoneComponent`, `RelationshipComponent` |

> [!NOTE]
> Bone **picking** (click a segment to select the joint) and bone **labels** (a text system the
> engine does not yet have) are deferred follow-ups. This page is read-only visualization only.

## Related

- [Playback runtime](../playback-runtime/) — the evaluator that moves the joints this draws
- [Animation data model](../animation-data-model/) — the skeleton and pose types it reads
- [Gizmo](../../ui-and-editor/gizmo/) — the native overlay path it shares
- [Rig editor](../../ui-and-editor/rig-editor/) — keys this overlay to the previewed rig and adds a highlight channel
