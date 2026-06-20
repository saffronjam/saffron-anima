# Phase 11 — Gizmo math + edit smoothing

**Status:** COMPLETED

**Depends on:** 03-ecs-and-scene:phase-10-play-mode

## Goal

Port the native-gizmo hit-test/projection/drag math and the edit-smoothing steppers — the pure-glm,
Rendering-free half of the gizmo (the *geometry* `build_native_gizmo` stays in the host). This is the
last sceneedit phase and completes the area: viewport projection, handle hit-testing, the
translate/rotate/scale drag math, `preserveChildren` child rebasing, the drag-begin snapshot, the
`tau = 0.025` pointer-smoothing stepper, the material/transform smoothing queues + `step_edit_smoothing`,
and `sync_native_gizmo`.

## Why this shape (NO LEGACY)

- **`GizmoOp`/`GizmoSpace` is the single source of truth; `NativeGizmo` mode/space is a per-frame
  mirror.** `sync_native_gizmo` mirrors the source onto `native_gizmo.mode/.space` each frame; nothing
  sets the mirror directly (`sceneedit/AGENTS.md`; `scene_edit_gizmo.cpp:23`). The two enum layers
  (`GizmoOp`/`GizmoSpace` and `NativeGizmoMode`/`NativeGizmoSpace`/`NativeGizmoHandle`) are kept distinct
  Rust enums exactly as the C++ split has them.
- **Projection + hit-test are shared between the SDL/winit event path and the control gizmo-pointer
  command.** `viewport_project` (world → pixel + NDC + visible), `pixel_to_ndc`, `camera_position`,
  `point_segment_distance`, `gizmo_axes` (world identity vs entity world-rotated basis in Local space),
  `handle_axis`, `gizmo_plane_corners` (shared by drawing + hit-test so they always agree), `ring_basis`
  (NaN-safe orthonormal basis for the rotation ring, including world up), and `hit_native_gizmo`. The
  feasibility study flags "the gizmo numeric edge cases" — `ring_basis` NaN-safety and the plane-corner
  agreement are the load-bearing ones, covered by `#[test]`s.
- **The drag math runs in world space then rebases into the parent frame.** `apply_native_gizmo_drag`
  projects the pointer delta onto the active axis/plane (`units_per_pixel` from camera distance + fov),
  applies it in world space, then writes the local transform by peeling the frozen `start_parent_world`
  (identity for a root). Rotate adds Euler about the active axis and, for a non-root, peels the frozen
  parent rotation and converts via `quat_to_euler_zyx`; a **root keeps the raw Euler** to preserve
  rotate-drag continuity past angles a matrix extraction would wrap (`scene_edit_gizmo.cpp:429`). Scale
  multiplies `start_scale` per-axis (uniform handle special-cased), floored at `0.05`. This is one drag
  implementation, faithfully ported — not a simplified rewrite.
- **The drag/snapshot operate on `editor.scene` directly (the authored scene) and bump
  `scene_version`.** The C++ `apply_native_gizmo_drag`/`snapshot_native_gizmo_start` reach
  `editor.scene` (not `active_scene`) and bump `sceneVersion` so the control-plane poll re-inspects the
  drag live (`scene_edit_gizmo.cpp:319,336,374`). Preserve that: gizmo dragging is an Edit-mode authored
  operation. Returning `&mut SceneEditContext` and indexing `.scene` keeps the borrow shape clean.
- **`preserveChildren` freezes direct-child worlds at drag begin and rebases their locals each applied
  frame.** `snapshot_native_gizmo_start` captures each direct child's world matrix (when
  `preserve_children`); `rebase_preserved_children` rebases `inv(target_world) * child_world` into each
  child local after the target writes, so a parent moves without dragging its children
  (`scene_edit_gizmo.cpp:311,340`).
- **The `tau = 0.025` exponential is shared across pointer-drag, look-drain, and edit-smoothing.**
  `step_native_gizmo_drag` smooths the raw pointer toward `drag_target` and applies the drag;
  `step_edit_smoothing` converges material + transform smoothed edits toward their targets, snapping
  exactly and dropping the entry once converged. `material_smooth_entry_for`/`transform_smooth_entry_for`
  append-or-find per entity; `cancel_*_smoothing` drops an entry (an exact write wins). A smooth edit
  issued during play converges in (and is discarded with) the play scene. Keep the single named const
  shared with phase-9.

## Grounding (real files / symbols)

- `engine-old/source/saffron/sceneedit/scene_edit_gizmo.cpp`: `syncNativeGizmo` (23), `viewportProject`
  (47), `pixelToNdc` (70), `cameraPosition` (75), `pointSegmentDistance` (81), `pointInConvexQuad` (93),
  `ringBasis` (119), `hitRotateRing` (132), `axisColor` (164), `gizmoAxes` (185), `handleAxis` (195),
  `gizmoPlaneCorners` (212), `hitNativeGizmo` (228), `parentOf`/`rotationOf`/`rebasePreservedChildren`
  (292/301/311), `snapshotNativeGizmoStart` (333), `applyNativeGizmoDrag` (364), `stepNativeGizmoDrag`
  (483, `tau=0.025`), `materialSmoothEntryFor`/`transformSmoothEntryFor` (498/510),
  `cancelMaterialSmoothing`/`cancelTransformSmoothing` (522/528), `stepEditSmoothing` (559, `tau=0.025`).
- `engine-old/source/saffron/sceneedit/scene_edit_context.cppm`: `NativeGizmoState` (102),
  `NativeGizmoMode`/`Space`/`Handle` (78–100), `GizmoOp`/`GizmoSpace` (58/64), the gizmo-math decls
  (385–440).

## Acceptance gate

- Cargo workspace compiles; the whole gizmo-math + smoothing surface exists.
- `cargo test -p saffron-sceneedit`:
  - `viewport_project`/`pixel_to_ndc` round-trip a world point to pixels and back within tolerance;
    `ring_basis` returns an orthonormal, NaN-free basis for arbitrary normals including world up.
  - `sync_native_gizmo` mirrors `GizmoOp`/`GizmoSpace` onto the native mirror.
  - a translate drag on a root moves the transform along the projected axis; a rotate drag on a root keeps
    the raw Euler (continuity); a rotate drag on a parented entity peels the parent rotation; a scale drag
    multiplies `start_scale` and floors at `0.05`; each bumps `scene_version`.
  - `preserve_children`: after a parent translate drag, a direct child's world transform is unchanged
    (rebased local), within `1e-4`.
  - `step_native_gizmo_drag`/`step_edit_smoothing` converge toward the target monotonically and drop the
    entry on convergence; `cancel_*_smoothing` removes the entry.
- Workspace build green; the full 03-ecs-and-scene area passes its tests.
