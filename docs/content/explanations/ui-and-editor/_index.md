+++
title = 'UI & editor'
weight = 14
+++

# UI & editor

The editor is a Tauri desktop app: a React/TypeScript front-end (shadcn/ui + Tailwind) in a webview, with the engine running as a separate process. The webview never renders the scene — the engine's own SDL/Vulkan window is reparented as a native child over the viewport div and presents directly, while ImGui is skipped (present-only mode). Every editor operation rides the JSON-over-unix-socket [control protocol](../tooling-and-control/control-plane-architecture/), and a focus-gated reconcile poll keeps a small Zustand store in sync with the running engine. The old Dear ImGui editor is retired; the `SaffronEngine` host executable (built from the `engine/` project) survives only as the headless viewport host.

## Pages

| Page | Covers | Code |
|---|---|---|
| `tauri-editor-and-x11-bridge` | Tauri/React shell, X11-reparent present-only bridge, the one generic control passthrough, auto-start/attach + crash recovery | `editor/src/control/client.ts` · `App.tsx` · `LoadingOverlay.tsx` |
| `viewport-panel` | the reparented native host div, two-tier bounds-sync, the Radix-portal occlusion rule, pointer forwarding | `ViewportPanel.tsx` |
| `editor-camera` | the engine `EditorCamera`, kept and driven by `get-/set-camera`, rendered through present-only | `editor_camera.cpp` |
| `gizmo` | the engine-rendered overlay gizmo, `gizmo-pointer`, the Topbar T/R/S + world/local | `Topbar.tsx` · `useGizmoShortcuts.ts` |
| `hierarchy-panel` | the React entity list, optimistic select, Create presets, copy/delete | `HierarchyPanel.tsx` · `CreateMenu.tsx` |
| `inspector` | the schema-driven inspector (fieldRenderer + FIELD_HINTS), RMW writes, add/remove guarded | `InspectorPanel.tsx` · `fieldRenderer.tsx` |
| `asset-pickers-and-drag-drop` | the AssetPicker uuid combo, type-gated HTML5 drag-drop | `AssetPicker.tsx` · `AssetTile.tsx` |
| `assets-panel-and-thumbnails` | the React asset browser, `get-thumbnail` base64 PNG + blob-URL cache, import dialog, View modal | `AssetsPanel.tsx` · `AssetTile.tsx` · `AssetViewer.tsx` |
| `selection` | select/get-selection/deselect, the version-stamped reconcile round-trip, optimistic select | `state/store.ts` · `ViewportPanel.tsx` |
| `theme-and-fonts` | the `theme::` palette → shadcn tokens (forced dark), bundled Roboto + Roboto Mono, the resizable dock | `styles.css` · `Layout.tsx` |
| `mesh-thumbnails` | the engine `renderMeshThumbnail` 3/4 preview, read back as a base64 PNG | `renderer_thumbnail.cpp` |
