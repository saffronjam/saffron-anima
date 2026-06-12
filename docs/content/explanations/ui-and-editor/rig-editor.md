+++
title = 'Rig editor'
weight = 6
+++

# Rig editor

The rig editor is a full work-area tab that opens a rigged model on its own — a live 3D preview of the rig, a skeleton tree, the rig's clips, and a scrubbable timeline — without ever spawning the asset into the scene you are building. Double-click a clip in the Assets panel, or a rigged mesh, and the rig opens in its own tab. It is the engine's equivalent of UE5's Persona, Unity's import preview, and Godot's Advanced Import Settings: asset-level rig inspection lives apart from the open level.

v1 is a viewer, not an authoring tool. You can look at the rig, walk its bones, switch clips, and play or scrub them at full frame rate. Keyframe editing, notify tracks, retargeting, and sockets are out of scope.

## The third mode: preview

The scene edit context already had two modes routed through one accessor, `activeScene`: Edit hands back the authored scene, Play hands back a throwaway duplicate (see [Play mode](../play-mode/)). The rig editor adds a third — Preview — the same way. Entering a preview builds a fresh `Scene` holding only the previewed rig plus its furnishing, and `activeScene` returns it while it is engaged. The render path, compute skinning, the animation evaluator, and every entity-addressed control command retarget to the preview for free, because they all already route through that one chokepoint.

Preview stays in `PlayState::Edit` (it is mutually exclusive with Play), so it is best read as "Edit, but looking at an isolated rig instead of your scene." The authored scene cannot leak into a save, because `save-project` serializes `ctx.sceneEdit.scene` explicitly — never `activeScene`. The keystone invariant the end-to-end test guards: entering a preview, scrubbing it, and leaving it returns `project.json` **byte-identical**, including the `editorCamera` block. The camera, the selection, and the skeleton-overlay preferences are stashed on enter and restored on exit, all engine-side, so even a CLI-driven `enter`/`exit` with no editor in the loop is leak-proof.

Commands that would mutate the authored scene or project — `new-project`, `open-project`, `load-scene`, `load-project`, `reload-project`, `delete-asset`, `import-model`, `assign-asset`, `set-material` — refuse while a preview is engaged ("exit the rig preview first"), the same way they refuse during Play. Entering a preview while Play is running is likewise refused, and `play` is refused while previewing.

## One viewport, taken over

The preview reuses the one Wayland subsurface the scene viewport already presents on — it does not open a second live view. The rig tab keeps that subsurface inside its own center pane (the bounds emitter is `useSubsurfaceBounds`, extracted from the viewport panel so a second host rect can drive it), and the engine publishes the preview scene through the existing shared-memory ring. Scrubbing the timeline updates the live frame at monitor refresh with no presenter or transport changes.

The cost is that the preview *takes over* the single stream, exactly like Play takes over the viewport: while a rig tab is active the scene's dock viewport is parked (its host rect goes 0×0 and no-ops). A second concurrent live view — your scene and a rig at once — is deliberately out of scope; it would need a second offscreen chain and per-view scene addressing that no plan owns yet.

## The rig is asset data

The rig is not a scene object — it is data baked into the mesh's [`.smodel` container](../../geometry-and-assets/smodel-container/). The container's metadata chunk holds the node forest and the skin (joints, inverse binds, skeleton root, mesh node); the animation clips and materials are sub-assets of the same file. So the clip↔mesh↔material association is intrinsic — one file, one asset — with no catalog link to chase and no project version bump.

`get-rig {asset}` reads that metadata and returns the rig as a flat parent-indexed bone tree plus the model's clips. It accepts the model, a mesh sub-asset, or a clip sub-asset — all resolve to the same owning container, which is why opening a clip and opening its mesh focus the *same* tab. The bone list is the skeleton subtree (the joints and their intermediate ancestors, bounded at the skeleton root), so the mesh node and unrelated scene roots are excluded. An asset with no skin in its container returns a clear "no rig" error, which the editor renders as the tab's error state with a re-import hint.

Because the rig lives in the file, re-opening the project — or deleting the spawned entities — re-derives it from the container. There is nothing to persist.

## The panels

**The tab is keyed by the rig**, not the clicked asset: a mesh and any of its clips open or focus one tab (`rigEditor:<rigMeshId>`). That is why the single-preview engine constraint can never be violated by two tabs of one rig, and why switching from rig A to rig B remounts the workspace (cleanup exits A, mount enters B) rather than silently leaving A previewing under B's panels.

- **Skeleton tree** (left) — the bone hierarchy from `get-rig`, render joints emphasized and intermediate nodes muted. Clicking a bone tints it in the live overlay through a dedicated **highlight channel** (`set-skeleton-highlight {joint}`), addressed by the bone's node index — *not* scene selection. This is deliberate: selecting a bone entity would null the selection-keyed animation state the timeline reads, and the selection-keyed overlay only draws for a `SkinnedMesh`. Highlighting keeps the engine selection on the rig, so the timeline stays fed and the overlay stays drawn.
- **Preview** (center) — the live rig on a floor under a key light and procedural sky, framed on enter (a 3/4 view fit to the rig's bounding sphere). Drag to orbit, wheel to dolly; both reconstruct the camera from the framed pivot the enter result returns and push a coalesced `set-camera`. Edit chrome (the gizmo, billboards, camera frustums) is suppressed — the preview is "Edit without chrome" — while the skeleton overlay defaults on. "Show floor" toggles via `set-rig-preview-options`.
- **Clip list + details** (right) — the rig's own clips (its container's animation sub-assets, not the whole catalog). Clicking a clip loads it paused at frame 0 (`play-animation {paused}` — select loads, the transport plays). The details section reports the focused clip (duration, tracks, wrap) or the rig (mesh, bones, joints, clips).
- **Timeline** (bottom) — the same transport and lane surface the dock Timeline uses, factored into shared components and mounted here against the preview rig. Play/pause/loop/step, Space to toggle, and full-rate scrubbing of the live preview through the existing 50 ms seek coalescer. The dock Timeline and the rig timeline never render at once (the dock is hidden while a non-scene tab is active).

## Opening it

Animation clips and rigged meshes/models route to the rig editor; everything else opens the image/asset viewer. "Rigged" is a flag the asset scan derives from the container's skin and puts on the catalog row (so the click handler knows synchronously and it survives a project reload). Unrigged meshes keep the image viewer but offer "Open in Rig editor" in the context menu, which surfaces the not-a-rig error state if the asset has no skin. Animation tiles carry a clapperboard icon and a duration badge.

## In the code

| What | File | Symbols |
|---|---|---|
| Preview scene + accessor + guards (engine) | `scene_edit_context.cppm` | `previewScene`, `activeScene`, `previewing` |
| Enter/exit + furnishing + framing (engine) | `control_commands_asset.cpp` | `enter-rig-preview`, `exit-rig-preview`, `set-rig-preview-options`, `furnishPreviewScene`, `leaveRigPreview` |
| Rig query (engine) | `control_commands_asset.cpp` | `get-rig` |
| Rig-keyed overlay + highlight (engine) | `engine/source/saffron/host/host.cppm` | `buildSkeletonOverlay` |
| Highlight + paused-pick (engine) | `control_commands_animation.cpp` | `set-skeleton-highlight`, `play-animation` `paused` |
| Workspace shell + orbit + lifecycle | `editor/src/panels/RigEditorWorkspace.tsx` | `RigEditorWorkspace` |
| Side panels | `editor/src/panels/RigSkeletonTree.tsx` · `RigClipList.tsx` | `RigSkeletonTree`, `RigClipList` |
| Shared timeline | `editor/src/components/timeline/` | `TimelineTransport`, `TimelineSurface`, `TimelineTarget` |
| Subsurface bounds hook | `editor/src/lib/useSubsurfaceBounds.ts` | `useSubsurfaceBounds` |
| Tab + routing | `editor/src/state/store.ts` · `AssetsPanel.tsx` | `openRigEditorTab`, `openRigEditorForAsset`, `routeView` |
| Client wrappers | `editor/src/control/client.ts` | `getRig`, `enterRigPreview`, `exitRigPreview`, `setSkeletonHighlight`, `setRigPreviewOptions` |

## Related

- [Play mode](../play-mode/) — the duplicate-scene pattern the preview extends into a third mode
- [Skeleton overlay](../../animation/skeleton-overlay/) — the line overlay the preview defaults on, here keyed to the rig with a highlight channel
- [Timeline](../../animation/timeline/) — the same transport + surface, mounted against the preview rig
- [`.smodel` container](../../geometry-and-assets/smodel-container/) — where the rig, clips, and materials live as one asset
- [Viewport panel](../viewport-panel/) — the bounds-emission machinery the preview reuses through `useSubsurfaceBounds`
