# Saffron.SceneEdit

The editor's mutable state core: the scene being edited + the `ComponentRegistry` + selection, the
fly-camera, editor play mode, the pure-math native gizmo, and edit smoothing. Module `Saffron.SceneEdit`
with one interface partition `:Context`. Deps are `Core`, `Signal`, `Scene`, `Json` — **no** `Rendering`
or SDL, so input arrives as backend-neutral structs the host fills. Consumed by `Saffron.Control` (per
command) and `Saffron.Host` (per-frame call ordering + the gizmo vertex builder).

## Files

| File | Role |
|---|---|
| `scene_edit_context.cppm` | Partition `:Context` — every exported state struct + the free-function surface. Read this first. |
| `scene_edit_context.cpp` | `setSelection`, gizmo op/space name↔enum, `newSceneEditContext` (seeds camera + sun), destroy. |
| `scene_edit_play.cpp` | The Edit/Playing/Paused state machine, `enterPlay`/`stopPlay`, `tickPlay` + the `simTick` seam. |
| `scene_edit_gizmo.cpp` | Projection/hit-test/drag math, the smoothing steppers, `preserveChildren` rebasing, `syncNativeGizmo`. |
| `scene_edit_camera.cpp` | Fly-camera forward/view/serde + `updateSceneEditCamera`. |
| `scene_edit_components.cpp` | `registerBuiltinComponents` — the JSON serde lambdas for every built-in component. |
| `sceneedit.cppm` | Outer module, re-exports `:Context`. |

## Rules that are easy to break

- **`activeScene(ctx)` is the only sanctioned scene accessor.** It is the single place that branches on
  `playState` (Edit → the authored scene, Playing/Paused → the play duplicate). Every control command,
  gizmo op, and host pass must route through it.
- **Play mode has no undo — the discard *is* the restore.** `enterPlay` duplicates the scene via
  `sceneToJson`/`sceneFromJson` (sharing the catalog); the authored scene is never written through
  `activeScene` while playing, and `stopPlay` simply drops the duplicate. `PlayFixedStep` is `1/60` for
  stepped frames; `stepFrames` is granted only while Paused; `PlayMaxDelta` clamps a hitch.
- **The gizmo has two type layers.** `GizmoOp`/`GizmoSpace` is the single source of truth; the
  `NativeGizmo` `mode`/`space` are a per-frame *mirror* — call `syncNativeGizmo`, never set the mirror
  directly.
- **Version stamps drive the editor's diff poll.** `sceneVersion` (mutations + each applied drag/smoothing
  frame), `selectionVersion`, `playVersion`, and `animationVersion` are what the control-plane reconcile
  poll keys on — bump the right one or the editor desyncs.
- **Smoothing queues hold per-entity handles tied to one registry.** `materialSmoothing` /
  `transformSmoothing` (and the selection) must be cleared / re-resolved on a play transition or scene
  swap, or they point into the wrong registry. The look-drain, gizmo-drag smoothing, and
  `stepEditSmoothing` all share the same `tau = 0.025` exponential that de-staircases ~60 Hz control
  samples at frame rate.
- **`preserveChildren` freezes direct-child worlds at drag/edit begin** and rebases their locals each
  applied frame, so a parent moves without dragging its children.
- **`simTick` is a `std::function` the host fills** (it points at the script runtime), which keeps this
  module SDL-free.

Note: the gizmo **geometry** (`buildNativeGizmo`, `OverlayVertex`) lives in `Saffron.Host`; only the
hit-test/projection/drag **math** lives here. A new editor-session field belongs on `SceneEditContext`
(never on `Scene`); it stays out of the scene serde, though a couple of view fields (the fly-camera, the
debug overlays) are round-tripped into `project.json` through the save/load caller. Add a matching control
command + version stamp if it is drivable, and extend `runPlayModeSelfTest` for any play-mode invariant.
