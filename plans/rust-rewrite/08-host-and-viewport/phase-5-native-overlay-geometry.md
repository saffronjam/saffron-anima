# Phase 5 — the native gizmo overlay geometry (the ~900-LOC CPU builder set)

**Status:** COMPLETED

**Depends on:** 08-host-and-viewport:phase-4-host-crate-lifecycle-wiring, 06-rendering:phase-11-tonemap-grid-overlay, 03-ecs-and-scene:phase-11-gizmo-math-and-smoothing

## Goal

Port the overlay geometry builders that `host.cppm` carries (because they touch the renderer's
`OverlayVertex`/`submit_overlay`) into a `mod overlay` in `saffron-host`, and wire
`submit_scene_edit_overlay` so the gizmo handles, entity billboards, camera frustums, debug overlays,
collider outlines, and the skeleton overlay render into the offscreen scene color (which then composites
into the shm-published frame). This fleshes out the editor's in-viewport chrome on top of the
phase-4 spine.

## Why this shape (NO LEGACY)

- **Pure CPU geometry, `&mut Vec<OverlayVertex>` arguments — no `Rc<RefCell>`.** The C++ builders take
  `std::vector<OverlayVertex>&` and push into it; the Rust port takes `&mut Vec<OverlayVertex>`. The
  PP-1 "host overlay state" `Rc<RefCell>` bucket is measured *not* needed here (README §1.3): the vecs
  are local to the `on_ui`/`submit_scene_edit_overlay` body and passed by mutable reference into each
  builder, so there is no shared accumulator to wrap.
- **`OverlayVertex` lives in 06-rendering, imported by the host.** It is the renderer's vertex format
  (`position: Vec2` NDC, `color: Vec4`, `edge: Vec4`, `depth: f32`), `#[repr(C)] + bytemuck::Pod` with a
  const size assert mirroring the C++ struct. The host does not redefine it.
- **The hit-test/projection/drag MATH already lives in `saffron-sceneedit`** (03-ecs-and-scene phase-11);
  these builders *consume* it to emit geometry — `build_native_gizmo` reads the gizmo's projected handle
  positions, `build_scene_edit_camera_frustums` reads the camera views, etc. The overlay is geometry
  emission only, never edit logic (the 03 split is preserved).
- **The two-range layout (`depth_tested` then `on_top`) ports verbatim.** `OverlayState` lays vertices
  out depth-tested-first then always-on-top so the overlay pass draws each range with its own pipeline
  from one buffer (`renderer_types.cppm:1002-1012`). `submit_scene_edit_overlay` builds `depth_tested`
  (frustums + debug overlays + colliders) and `on_top` (billboards + gizmo + skeleton), gated by
  `edit_chrome` (Edit-only, hidden in Play and the asset preview) — colliders + skeleton sit outside the
  gate with their own preview guards.
- **`glam` quaternion/matrix idioms.** The world-space builders take a `view_projection: Mat4` and clip
  lines to the near plane; glam's `Mat4`/`Vec3`/`Vec4` replace glm directly. Quaternion order is xyzw
  (glam == Jolt == the .sanim byte format after 02-geometry's reorder), so no swizzle in the gizmo
  rotation handles.

## Grounding (real files/symbols)

- `engine-old/source/saffron/host/host.cppm` (the overlay TU, lines 77-976): the 2D primitive builders
  `addTriangle` (81), `addLine` (88), `addQuad` (121), `addBox` (155), `addRectOutline` (167),
  `addCircleFill` (181), `addCircleOutline` (198), `addBulbIcon` (212), `addCameraIcon` (222); the
  composite builders `buildNativeGizmo` (258), `buildSceneEditCameraFrustums` (352),
  `buildSceneEditBillboards` (453); the world-space helpers `addClippedOverlayLine` (558),
  `buildDebugOverlays` (575), `addWorldAabb` (629), `addWorldRing` (649), `addWorldArc` (665),
  `addWorldOrientedBox` (682); the whole-scene AABB note (781), `buildColliderOverlays` (847),
  `buildSkeletonOverlay` (around 950), `BillboardKind` (69-75); and the entry
  `submitSceneEditOverlay` (957-974: builds both ranges, gates on `editChrome`, calls `submitOverlay`).
- `engine-old/source/saffron/rendering/renderer_types.cppm`: `OverlayVertex` (993-1000), `OverlayState`
  (1006-1012, the two-range layout), `submitOverlay` decl (1854).

## Acceptance gate

- Cargo workspace compiles; `cargo build -p saffron-host`; `cargo clippy`/`fmt --check` clean.
- Unit `#[test]`s on the builders (pure geometry, no GPU):
  - `add_line_emits_two_triangles_with_edge`: a thick line produces 6 vertices with the expected
    edge/thickness offsets; `add_quad`/`add_box`/`add_circle_*` emit the expected vertex counts.
  - `pixel_to_ndc_roundtrip`: pixel→NDC mapping matches the C++ for known sizes (center → 0,0; corner →
    ±1).
  - `clipped_overlay_line_near_plane`: a line crossing the near plane is clipped, not dropped; a fully
    behind-camera line emits nothing.
  - `submit_scene_edit_overlay_ranges`: with `edit_chrome=true` the `depth_tested` range carries
    frustums+debug+colliders and `on_top` carries billboards+gizmo+skeleton; with `edit_chrome=false`
    (Play / preview) only colliders+skeleton appear (their preview guards honored).
- A golden-image / composite test (gated on the renderer, else skipped+logged): a known scene + gizmo
  selection renders the offscreen frame with the overlay composited, matching a committed golden within
  tolerance, validation-clean (the overlay pass's depth-tested + on-top pipelines), and the frame
  publishes through the phase-2/3 shm path.
