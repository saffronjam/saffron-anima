+++
title = 'UI & editor'
weight = 14
bookCollapseSection = true
+++

# UI & editor

The editor is a Tauri desktop app that drives the engine over a control protocol. A React/TypeScript front-end (shadcn/ui + Tailwind) runs in a webview, while the engine runs as a separate process. The webview never renders the scene. The engine renders headless and publishes frames into shared memory; the editor presents them on a Wayland subsurface below its transparent window, so the web UI composites over the live viewport. The engine carries no UI toolkit — all editor UI is the React front-end.

Every editor operation rides the JSON-over-unix-socket [control protocol](../tooling-and-control/control-plane-architecture/). A focus-gated reconcile poll keeps a small Zustand store in sync with the running engine.

## Pages

| Page | Covers | Code |
|---|---|---|
| `tauri-editor-and-viewport-bridge` | Tauri/React shell, the one generic control passthrough, engine spawn env (two per-view shm segments), auto-start + crash recovery | `editor/src/control/client.ts` · `App.tsx` · `LoadingOverlay.tsx` |
| `viewport-compositing` | shm/seqlock/subsurface/dma-buf foundations, offscreen render → pipelined shm ring → wl_subsurface below the transparent toplevel, two views / two subsurfaces + shared backdrop, per-view park, segment-remap traps | `renderer_capture.cpp` · `wayland_viewport.rs` |
| `viewport-panel` | the transparent host div, per-view two-tier bounds-sync over `set_viewport_bounds {view}`, per-view parking, gizmo + pointer-lock fly forwarding | `ViewportPanel.tsx` · `useSubsurfaceBounds.ts` |
| `editor-camera` | the engine `EditorCamera`, fly input streamed over `fly-input`, driven by `get-/set-camera` | `editor_camera.cpp` |
| `gizmo` | the engine-rendered overlay gizmo, `gizmo-pointer`, the Topbar T/R/S + world/local | `Topbar.tsx` · `useGizmoShortcuts.ts` |
| `play-mode` | play/pause/stop/step, scene-duplication discard, camera handover + fallback, live-tune-and-discard tint + locks | `scene_edit_play.cpp` · `Topbar.tsx` · `state/store.ts` |
| `debug-visualization` | the transient viewport debug overlays (pick/scene-AABB boxes, light volumes) via `set-debug-overlays`, drawn as depth-tested world-space lines, surfaced in the Render panel's Debug section | `host.cppm` · `scene_edit_context.cppm` · `RenderPanel.tsx` |
| [`asset-editor`](asset-editor/) | the asset-editor tab for every model: the preview scene (Edit/Play/Preview triad), its own AssetPreview view + surface, `set-active-view` park, `get-asset-model` + capability-gated panels, skeleton tree + highlight channel, clip list, the shared timeline, byte-identity | `control_commands_asset.cpp` · `AssetEditorWorkspace.tsx` · `scene_edit_context.cppm` |
| `editor-settings` | the gear-button settings modal, the rebindable-keybinding registry + delta `settings.json`, the `load/save_editor_settings` bridge | `SettingsModal.tsx` · `lib/keybindings.ts` · `src-tauri/src/lib.rs` |
| `hierarchy-panel` | the React tree outliner (`parentId` → forest), drag-reparent, the Environment sentinel, Create presets | `HierarchyPanel.tsx` · `HierarchyTree.tsx` |
| `inspector` | the DTO-typed component inspector (fieldRenderer + FIELD_HINTS), RMW writes, add/remove guarded | `InspectorPanel.tsx` · `fieldRenderer.tsx` |
| `physics-inspector` | the split Rigidbody/Collider sections: the enum/lockAxes/struct field kinds (motion/shape Selects, X/Y/Z lock grid, nested material sliders), Fit-to-mesh, collider-alone-static note, skinned-only rig sidecars | `fieldRenderer.tsx` · `EnumField.tsx` · `LockAxesField.tsx` · `InspectorPanel.tsx` |
| `asset-pickers-and-drag-drop` | the AssetPicker uuid combo, type-gated HTML5 drag-drop | `AssetPicker.tsx` · `AssetTile.tsx` |
| `assets-panel-and-thumbnails` | the React asset browser, virtual folders, asset tabs, `get-thumbnail` base64 PNG + blob-URL cache, import dialog | `AssetsPanel.tsx` · `AssetTile.tsx` · `AssetViewer.tsx` |
| `selection` | select/get-selection/deselect, the version-stamped reconcile round-trip, optimistic select | `state/store.ts` · `ViewportPanel.tsx` |
| `undo-redo` | editor-only inverse-command + per-tab snapshot history, gesture grouping, mouse Back/Forward + Alt-arrow nav suppression, invalidation, the extension recipe | `lib/undo.ts` · `useTabSnapshotHistory.ts` · `state/store.ts` |
| [`dock-system`](dock-system/) | the per-kind dock tree (`dockLayouts` keyed `scene`/`assetEditor`), per-main-tab isolation (disjoint `DockPanelId` spaces + active-island-only `[data-dock-leaf]` registry), the shared `TabStrip` + tear-out drag, the portal host, the locked live-subsurface leaves, the asset editor as a dock island, per-project persistence | `state/dockLayout.ts` · `components/dock/DockRoot.tsx` · `DockPanelsHost.tsx` · `dockDrag.ts` |
| `theme-and-fonts` | shadcn theme tokens, font defaults (the layout itself lives in `dock-system`) | `styles.css` |
| `mesh-thumbnails` | the engine `renderMeshThumbnail` 3/4 preview, read back as a base64 PNG | `renderer_thumbnail.cpp` |
| [`metrics-dashboard`](metrics-dashboard/) | the gated metrics poll, the uPlot live frame-time graph, per-pass + VRAM views, shared thresholds, the alarm toasts/log/badge | `RenderStatsPanel.tsx` · `FrameTimeGraph.tsx` · `state/store.ts` |
| [`profiler-panel`](profiler-panel/) | the capture tab beside Stats, the Start/Stop state machine, the table/flame/icicle views + cross-highlight, Chrome-Trace + Perfetto export | `ProfilerPanel.tsx` · `CaptureControls.tsx` · `lib/captureTree.ts` |
| `physics-panel` | the play-mode physics diagnostics dock panel: live body counts + the contact/trigger feed (open-AND-playing gated), and the per-selection ragdoll blend + character move controls | `PhysicsPanel.tsx` · `state/store.ts` · `control_commands_physics.cpp` |
