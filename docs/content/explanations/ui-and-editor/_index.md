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
| `tauri-editor-and-viewport-bridge` | Tauri/React shell, the one generic control passthrough, engine spawn env, auto-start + crash recovery | `editor/src/control/client.ts` · `App.tsx` · `LoadingOverlay.tsx` |
| `viewport-compositing` | shm/seqlock/subsurface/dma-buf foundations, offscreen render → pipelined shm ring → wl_subsurface below the transparent toplevel, backdrop + segment-remap traps, two-tier resize | `renderer_capture.cpp` · `wayland_viewport.rs` |
| `viewport-panel` | the transparent host div, two-tier bounds-sync over `set_viewport_bounds`, parking, gizmo + pointer-lock fly forwarding | `ViewportPanel.tsx` |
| `editor-camera` | the engine `EditorCamera`, fly input streamed over `fly-input`, driven by `get-/set-camera` | `editor_camera.cpp` |
| `gizmo` | the engine-rendered overlay gizmo, `gizmo-pointer`, the Topbar T/R/S + world/local | `Topbar.tsx` · `useGizmoShortcuts.ts` |
| `hierarchy-panel` | the React tree outliner (`parentId` → forest), drag-reparent, the Environment sentinel, Create presets | `HierarchyPanel.tsx` · `HierarchyTree.tsx` |
| `inspector` | the DTO-typed component inspector (fieldRenderer + FIELD_HINTS), RMW writes, add/remove guarded | `InspectorPanel.tsx` · `fieldRenderer.tsx` |
| `asset-pickers-and-drag-drop` | the AssetPicker uuid combo, type-gated HTML5 drag-drop | `AssetPicker.tsx` · `AssetTile.tsx` |
| `assets-panel-and-thumbnails` | the React asset browser, virtual folders, asset tabs, `get-thumbnail` base64 PNG + blob-URL cache, import dialog | `AssetsPanel.tsx` · `AssetTile.tsx` · `AssetViewer.tsx` |
| `selection` | select/get-selection/deselect, the version-stamped reconcile round-trip, optimistic select | `state/store.ts` · `ViewportPanel.tsx` |
| `theme-and-fonts` | shadcn theme tokens, font defaults, the resizable dock | `styles.css` · `Layout.tsx` |
| `mesh-thumbnails` | the engine `renderMeshThumbnail` 3/4 preview, read back as a base64 PNG | `renderer_thumbnail.cpp` |
