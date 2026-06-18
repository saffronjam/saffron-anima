# Phase 12 — `render_scene` and pick

**Status:** COMPLETED

**Depends on:** 07-assets-and-materials:phase-7-render-ready-materials, 06-rendering (all the per-frame setters + submit_draw_list + the render-graph), 03-ecs-and-scene (forEach queries + world transforms + joint matrices), 04-animation (joint palette)

## Goal

Port the engine's single highest-coupling function — `render_scene(renderer, scene, assets, camera,
options)` — and its read-side twin `pick_entity`. `render_scene` translates a scene + camera into the
renderer's draw list + every per-frame lighting/shadow/GI/sky/RT/cluster/SSAO setter; `pick_entity`
ray-casts the same scene to find the clicked entity. Plus the supporting helpers
(`append_editor_camera_models`, `entity_id_or_zero`, `look_at_up_for_dir`, the `RenderSceneOptions`).

## Why this shape (NO LEGACY)

`render_scene` lives here, not in `saffron-rendering`, because it reads the scene + asset caches and
*drives* the renderer — the orchestrator, not a renderer helper (the AGENTS rule). It stays one
procedure with extracted helpers (light gather, draw-list build, shadow/DDGI/sky setup); it is **not**
re-architected into a trait — a procedure is the honest shape and adding a trait would be ceremony.

The borrow shape is the area's deliberate low-coupling answer to the C++ `Renderer&` god-aggregate:
`render_scene` takes `&Scene` (read), `&mut AssetServer` (mutable, for the on-demand cache fill), and the
renderer area's sub-state handles `&mut` (the renderer is split per 06-rendering's §2, so the ~30 setters
are methods on disjoint sub-state, not free functions on one 80-field struct). The three values are
distinct, so the three borrows are disjoint and the borrow checker is satisfied without `Rc<RefCell>` or
interior mutability. The frozen behaviors (README §6) port exactly:

1. Invalid-camera / zero-viewport early-out; the Y-flipped `viewProjection`.
2. `update_world_transforms(scene)` **once** before any reader.
3. Directional (parented re-aim) + point + spot light gather; the single shadowed spot/point in v1.
4. Static draw-list build (mesh + materials on demand, world AABB + DDGI box proxies) then — **gated on
   `skinning_enabled(renderer)`** — the skinned draw list (identity model, joint palette via
   `joint_matrices`, conservative bind-AABB bounds). Off → byte-identical to a no-skinning build.
5. Directional shadow ortho fit to the scene-AABB bounding sphere; the RT static-vs-skinned split; the
   DDGI volume fit + upload; the reflection-probe snapshot (consuming `dirty`).
6. `set_scene_lighting`; the env-bake source resolution (Equirect/Atmosphere/Procedural, sun from the
   directional light); `set_cluster_camera`/`set_ssao_camera`/`set_show_grid`; optional editor-camera
   models; `submit_draw_list`; `submit_sky`.

`pick_entity` rebuilds the same Y-flipped inverse-view-proj, broad-phase per-mesh AABB then narrow-phase
ray-triangle against the CPU mesh copy (static via world matrix, skinned via a fresh joint palette),
returns the nearest hit or none — reading the same last-frame world-transform flatten (lockstep with the
draw loop) but rebuilding the joint palette fresh.

## Grounding (real files/symbols)

- `engine-old/source/saffron/assets/assets.cppm`: `renderScene` (the full driver — the light gather, the
  static + skinned draw-list build, `setSpotShadow`/`setPointShadow`/`setDirectionalShadow`/`setRtScene`/
  `setDdgiScene`/`submitReflectionProbes`/`setSceneLighting`/`requestEnvBake`/`setClusterCamera`/
  `setSsaoCamera`/`setShowGrid`/`submitDrawList`/`submitSky`, the `skinningEnabled` gate, the RT
  static/skinned split, the env source resolution), `RenderSceneOptions`, `appendEditorCameraModels`,
  `loadEditorCameraModel`, `entityIdOrZero`, `lookAtUpForDir`, `pickEntity` (the broad/narrow phase),
  `worldAabbFromCorners`.
- Upstream: scene `forEach`/`world_matrix`/`world_rotation`/`world_translation`/`update_world_transforms`/
  `joint_matrices`; rendering's per-frame setters + `submit_draw_list` + `DrawItem`; the renderer
  sub-state split (06-rendering §2).
- The AGENTS rule: "`renderScene` is the orchestrator, not a helper — and it lives here… The skinned path
  is gated on skinning being enabled."

## Acceptance gate

- `cargo build -p saffron-assets` + workspace green; clippy + fmt clean.
- `#[test]`s (over a stub scene + a recording stub renderer): `render_scene` early-outs on an invalid
  camera / zero viewport; it calls `update_world_transforms` exactly once before the first draw gather;
  the skinning gate off produces a draw list with no skinned items (byte-identical setter sequence to a
  no-skinning build); the first directional/spot/point lights drive the single-shadow setters.
- A draw-list `#[test]`: a 2-mesh scene records 2 `DrawItem`s with the resolved materials + world
  matrices + per-draw AABB proxies; the RT static/skinned split puts static items (with `model`) into
  `set_rt_scene` and skinned items into the draw list with identity.
- A `pick_entity` `#[test]`: a click through a known mesh's center hits that entity; a click through the
  empty space inside a loose AABB misses (narrow-phase ray-triangle); a skinned mesh picks against a
  fresh joint palette. A borrow `#[test]`/doc-comment confirms the `&Scene` + `&mut AssetServer` +
  `&mut Renderer`-sub-state disjoint-borrow shape compiles (no `RefCell`).
- An e2e golden (driven over the frozen wire by the host once 08/09 land): a small scene renders a frame
  the editor displays; flagged here as the cross-area integration target, the unit gate above is the
  per-phase requirement.

## Substrate change after completion (for 09-control-plane `pick`)

`pick_entity` was decoupled from the full `SceneRenderer` to `pick_entity(gpu: &dyn GpuUploader,
viewport: (u32, u32), scene, assets, camera, ndc)` — it only ever used the viewport extent + the AABB
mesh upload, never the ~30-method per-frame render driver. The `pick` control handler reaches it
through the `ControlRenderer` seam (`with_gpu_uploader` for the upload, `viewport_width`/
`viewport_height` for the aspect), so the control plane needs no `SceneRenderer` stub. The two
`pick_entity` `#[test]`s pass `(width, height)` explicitly (the `RecordingRenderer` still coerces to
`&dyn GpuUploader` via the supertrait).
