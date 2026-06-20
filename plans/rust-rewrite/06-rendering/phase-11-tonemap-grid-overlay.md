# Phase 11 — the mandatory tonemap, the ground grid, and the editor overlay

**Status:** COMPLETED

**Depends on:** 06-rendering:phase-10-aa-and-temporal

## Goal

Port the final post chain that runs every frame: the mandatory HDR→display tonemap (exposure + Reinhard
+ gamma, an in-place compute pass on the offscreen color), the optional analytic ground grid (a
fullscreen depth-tested debug overlay), and the editor overlay (gizmo handles + entity billboards
composited after tonemap so present-only blits them too). This completes the offscreen-color frame; the
result is what the swapchain present / shm publish consumes.

## Why this shape (NO LEGACY)

- **The tonemap is mandatory and added by `add_tonemap_pass` after the scene + AA passes** — the scene
  wrote linear HDR (RGBA16F) which must be display-mapped before present (`renderer.cppm:2296`). It is an
  in-place compute pass (`StorageImageRWCompute` on offscreen). Exposure is `exp2(exposure_ev)`
  (`renderer.cppm:2795`). There is exactly one tonemap; it is not optional.
- **The grid + overlay run at 1x on the resolved offscreen, after tonemap** (so gizmo handles draw on top
  of the display-referred image and present-only embeds them). By here `sceneColor` is always the 1x
  offscreen regardless of AA (`renderer.cppm:~2200`+). The grid writes `SV_Depth` and depth-tests against
  the persisted 1x scene depth so geometry occludes it; the overlay has a depth-tested variant (camera
  frustums, occluded) and an on-top variant (handles, always drawn).
- **The overlay vertex list is submitted per frame as plain CPU geometry (`submit_overlay`), uploaded
  into a grow-only per-frame vertex buffer in the pass body** (`renderer.cppm:2345`, the overlay pass
  ~`:2227`). The `OverlayVertex` list + `depthTestedCount` come from the host's native gizmo builder
  (`buildNativeGizmo`, PP-10) — this phase owns the *pass* and the pipelines (`overlay`/`overlayDepth`/
  `grid`), not the geometry generation. The overlay state (`OverlayState`, `renderer_types.cppm:1006`)
  is per-frame; it is the host overlay's `Rc<RefCell>`-class single-thread state, but the renderer side
  is just a `Vec<OverlayVertex>` consumed in the pass.
- **`OverlayVertex` is `#[repr(C)]` + bytemuck** (the vertex stream the overlay PSO binds). The grid PSO
  reconstructs the world ray from a push-constant `invViewProj` and is alpha-blended.
- **One tonemap, one grid, one overlay pass — present-only and editor mode share them.** Present-only
  mode blits the offscreen straight to the swapchain (no ui pass); editor mode adds the ui pass. The
  offscreen content (incl. overlay) is identical (`set_present_viewport_only`, `renderer.cppm:2316`).

## Grounding (real files/symbols)

- `engine-old/source/saffron/rendering/renderer.cppm` — `addTonemapPass` (`:2296`), the `grid` +
  `editor-overlay` passes in `beginFrameGraph` (`:~2200`–`:2280`), `submitOverlay` (`:2345`),
  `recordGrid` (`:2 recordGrid`), `newOverlayPipeline` (`:1846`), `newGridPipeline` (`:1849`),
  `setExposure`/`exposureEv` (`:2795`/`:2800`), `setShowGrid` (`:3096`),
  `setPresentViewportOnly` (`:2316`).
- `engine-old/source/saffron/rendering/renderer_types.cppm` — `OverlayVertex` (`:993`), `OverlayState`
  (`:1006`), `Pipelines.tonemap`/`overlay`/`overlayDepth`/`grid` (`:1227`), `exposureEv` (`:1765`).
- Shaders: `tonemap`, `grid`, `gizmo_overlay`.
- README §6; PP-10 owns `buildNativeGizmo` (the geometry source).

## Acceptance gate

- `cargo build -p saffron-rendering` and the workspace build are green.
- `cargo test -p saffron-rendering` passes named tests:
  - `OverlayVertex` size/offset asserts; the tonemap exposure push (`exp2(ev)`) matches.
  - the tonemap pass is always present in the graph (mandatory); the grid/overlay passes appear only when
    armed (`show_grid`, non-empty overlay list).
  - present-only vs editor mode produce identical offscreen content (the ui pass adds only swapchain
    composition).
- **Golden-image** tests: a known HDR scene tonemapped matches a committed display-referred golden; the
  grid + a gizmo overlay composite correctly over it (committed golden). Validation log clean.
