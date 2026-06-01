+++
title = 'UI & editor'
weight = 14
+++

# UI & editor

The editor runs on Dear ImGui with the SDL3 and Vulkan backends through dynamic rendering. The scene shows as a texture in a dockable viewport, a fly-camera and an in-viewport gizmo drive authoring, and the inspector is generic — driven entirely by the [component registry](../scene-and-ecs/component-registry/), so it needs no per-component UI code.

## Pages

| Page | Covers | Code |
|---|---|---|
| `imgui-integration` | SDL3 + Vulkan backends, docking, dynamic rendering, descriptor pool | `ui.cppm` |
| `viewport-panel` | `ImGui_ImplVulkan_AddTexture`, descriptor refresh, 1-frame-lag resize | `ui.cppm` · viewport |
| `editor-camera` | RMB-look + WASD fly-cam, separate from ECS cameras | `editor_camera.cpp` |
| `gizmo` | ImGuizmo TRS in the viewport, W/E/R, un-flipped projection, decompose write-back | `editor_gizmo.cpp` |
| `hierarchy-panel` | entity tree, create/copy/delete, deferred ops | `editor_panels.cpp` |
| `inspector` | the generic registry-driven inspector, add/remove/edit | `editor_panels.cpp`; `editor_components.cpp` |
| `asset-pickers-and-drag-drop` | mesh/material combos, type-safe drag-drop payloads | `editor_components.cpp` |
| `assets-panel-and-thumbnails` | tile grid, texture/mesh/SVG thumbnails, in-place rename | `editor_panels.cpp`; `renderer_thumbnail.cpp` |
| `selection` | `SubscriberList<Entity>` selection, click-pick, empty-space deselect | `editor_context.cpp`; `assets.cppm` · `pickEntity` |
| `theme-and-fonts` | the dark theme, Roboto + Roboto Mono, default dock layout | `ui.cppm` |
| `mesh-thumbnails` | orthographic 3/4 preview, auto-framing, pipeline reuse | `renderer_thumbnail.cpp` |
