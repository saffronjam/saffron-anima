+++
title = 'Debug visualization'
weight = 18
+++

# Debug visualization

Debug visualization draws the volumes the engine reasons about but never normally shows — the
boxes mouse picking tests, the box the shadow fit covers, the reach of each light. The picker
(`pickEntity`) is AABB-only: it transforms a mesh's local AABB by its world matrix, re-encloses
the eight corners axis-aligned in world space, and ray-tests that box. For a rotated mesh or one
with a thin protrusion (an antenna, a rig limb) that box bulges well past the silhouette, so a
click in empty space can still land — and skinned meshes are skipped entirely. Turning on the
**bounding-box** overlay makes that box visible, so the over-select is something you see rather
than guess at.

These are **transient editor state**: they live on the `SceneEditContext`, never serialize into a
project, and are not on the undo stack — the same footing as the [skeleton overlay](../asset-editor/),
not the [Render panel's](../metrics-dashboard/) persisted render config. They render only in Edit,
as world-space lines in the editor overlay pass, into the depth-tested bucket so scene geometry
occludes them.

## Overlays

| Overlay | Draws | Notes |
|---|---|---|
| **Bounding Boxes** | The world AABB per mesh — the exact box `pickEntity` tests | Static meshes: the per-draw box (green). Skinned meshes: the joint-union box (magenta), since the picker would need the same union to hit a rig. |
| **Scene AABB** | The whole-scene box the directional-shadow / DDGI fit derives each frame (yellow) | `renderScene` recomputes and discards this every frame; the overlay recomputes the same union for display. |
| **Light Volumes** | Point-light range as three great-circle rings; spot-light cone from apex to base ring | The spot direction matches the lighting upload (`normalize(worldRotation · direction)`), so the cone shows where the light actually shines. Directional lights are skipped (their position is arbitrary). |
| **Grid** | An infinite ground-plane reference grid with red (X) / blue (Z) axis lines | Not a line overlay — a fullscreen analytic render-graph pass (`grid.slang`): the fragment reconstructs the world ray from the inverse view-projection, intersects `y = 0`, anti-aliases the lines with `fwidth`, fades with distance, and writes `SV_Depth` so geometry occludes it. Runs at 1× after tonemap, before the line overlay. |

## Driving it

One grouped command toggles any subset; omitted fields stay unchanged (the
[`set-skeleton-overlay`](../asset-editor/) shape). The Render panel's **Debug** section mirrors the
state through a render-panel-gated poll, so an external `se` toggle shows up there too.

```sh
se set-debug-overlays --bounds true --lightVolumes true
se set-debug-overlays --bounds false      # the others stay as they were
se get-debug-overlays
```

## View modes

Where the overlays *add* lines on top of the normal render, the **view mode** *replaces* what the
scene pass outputs. It is mutually exclusive — one mode at a time — so it is a single enum verb
(`set-view-mode {lit|wireframe|albedo|normal|roughness|metallic|emissive}`), the
[`set-aa`](../../tooling-and-control/render-commands/) shape, read back through `render-stats.viewMode`
(there is no `get-view-mode`). The Render panel's **View Mode** dropdown drives it. Like the overlays
it is transient — it lives on the `Renderer`, never serializes into the project, resets to **Lit** on
load, and is not undoable. The enum lists only implemented modes, so the dropdown never offers a value
the engine would ignore.

- **Lit** — the normal forward+ PBR render.
- **Wireframe** — the mesh PSO drawn with `vk::PolygonMode::eLine`. A per-draw PSO variant selected by
  the view mode, gated on the `fillModeNonSolid` device feature; a GPU lacking it stays Lit. (Mesa
  llvmpipe — the software path — supports it, so a headless run wireframes for real.)
- **Albedo / Normal / Roughness / Metallic / Emissive** — surface channels the mesh fragment outputs
  directly instead of lighting. The active channel rides a spare `LightGlobals` slot
  (`pointShadowMeta.w`) the fragment reads (`debugViewChannel()`), so no new render targets or passes
  are involved. These still pass through the tonemap, so the values are display-referred (exposure +
  Reinhard + gamma), not raw — fine for eyeballing, not for sampling exact values.

Screen-space channels that need a producing pass to be enabled — Depth, Motion Vectors, AO (GTAO),
Overdraw, Light Complexity — are not implemented yet; they are a fullscreen-blit follow-up gated on
their producer (the G-buffer / TAA / SSAO targets exist only when those features are on).

```sh
se set-view-mode --mode wireframe
se set-view-mode --mode normal
se render-stats -o json | jq .viewMode    # "normal"
```

## Code

| What | File | Symbols |
|---|---|---|
| Overlay state (transient, per session) | `sceneedit/scene_edit_context.cppm` | `DebugOverlayOptions`, `SceneEditContext::debugOverlays` |
| View-mode state + device feature | `rendering/renderer_types.cppm` · `rendering/renderer.cppm` | `ViewMode`, `Renderer::viewMode`, `setViewMode`/`viewMode`, `VulkanContext::fillModeNonSolid` |
| Wireframe PSO variant | `rendering/renderer_pipelines.cpp` · `rendering/renderer_drawlist.cpp` | `newMeshPipeline`/`requestMeshPipeline` (wireframe), the per-draw gate |
| Buffer-channel output | `engine/assets/shaders/mesh.slang` · `lighting.slang` · `rendering/renderer_lighting.cpp` | the fragment debug branch, `debugViewChannel()`, the `pointShadowMeta.w` pack |
| Control commands | `control/control_commands_scene.cpp` · `control/control_commands_render.cpp` | `get-debug-overlays`, `set-debug-overlays`, `set-view-mode`, `RenderStatsDto::viewMode` |
| World-space line builders | `host/host.cppm` | `buildDebugOverlays`, `addWorldAabb`, `addWorldRing`, `submitSceneEditOverlay` |
| Grid pass + shader | `rendering/renderer.cppm` · `rendering/renderer_pipelines.cpp` · `engine/assets/shaders/grid.slang` | the grid `RgPass`, `newGridPipeline`, `recordGrid`, `Renderer::showGrid` (from `RenderSceneOptions::showGrid`) |
| Editor panel + state | `editor/src/panels/RenderPanel.tsx` · `editor/src/state/store.ts` | `DEBUG_OVERLAYS`, `VIEW_MODES`, `onDebugToggle`, `onViewMode`, `debugOverlays` slice |
