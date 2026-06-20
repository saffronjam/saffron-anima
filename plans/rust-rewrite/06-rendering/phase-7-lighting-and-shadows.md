# Phase 7 — clustered lighting + directional / spot / point shadows

**Status:** COMPLETED

**Depends on:** 06-rendering:phase-6-instancing-and-scene-pass

## Goal

Port the lighting rig: the per-frame directional-light UBO + ambient + eye, the punctual-light storage
buffer, the clustered-forward froxel light-cull compute pass, and the three shadow types — directional
(orthographic depth map), spot (perspective depth map), and point (omnidirectional distance cubemap).
The scene pass (phase 6) gains real lighting: it samples the cluster light lists and the shadow maps.
This turns the flat-ambient image into a lit PBR forward+ render.

## Why this shape (NO LEGACY)

- **Clustered forward is the one lighting path; the all-lights fragment loop is a reference fallback,
  not a parallel production path.** `use_clustered` (`renderer_types.cppm:1157`) defaults true; the
  light-cull compute pass (`light-cull`, `renderer.cppm:~1357`) fills the per-cluster count+index SSBO
  the mesh fragment reads. When off, the fragment loops all lights — kept only as a correctness oracle
  behind one bool, exactly as today.
- **`GpuLight` is `#[repr(C)]` + bytemuck, 4×vec4, size-asserted** (positionRange, colorIntensity,
  directionType, spotCos — `renderer_types.cppm:2018`). The cluster-params UBO is a separate `#[repr(C)]`
  struct (camera + grid dims). Per-frame light state goes through `set_scene_lighting` +
  `set_cluster_camera` (`renderer.cppm:2034`/`:2040`).
- **Per-frame light UBO + set per frame-in-flight, so a host write never races a frame still reading on
  the GPU** (`Lighting`, `renderer_types.cppm:1145`). This is `MaxFramesInFlight` arrays, owned by the
  `Lighting` sub-state and mutated through its own methods + `&Device` — no `Arc<Mutex>` (single-threaded
  host write, frame-indexed).
- **Three shadow passes, each conditional and self-describing through `RgUsage`.** The directional/spot
  shadow passes are depth-only graphics passes (`shadow`, `spot-shadow`) that the graph transitions
  `DepthWrite → ShaderReadOnly`; the point shadow renders 6 faces into a distance cubemap (`point-shadow`,
  a compute-kind pass driving per-face draws, `renderer.cppm:~1335`). The "pending" flags
  (`shadow_pending`, `spot_shadow_pending`, `point_shadow_pending`) arm them per frame; the light-space
  transforms are set via `set_directional_shadow`/`set_spot_shadow`/`set_point_shadow`
  (`renderer.cppm:3101`/`:3107`/`:3114`). The shadow maps live in `Targets` (scene-global — one light
  rig), the compare sampler in `Descriptors` (phase 4).
- **The skinned-mesh shadow draws read the deformed buffer when skinning is on** — the shadow passes
  push a `VertexInputRead` access on the deformed buffer (`renderer.cppm:1300`), wired once phase 12
  lands. The access declaration is conditional on `do_skin`, so it is a no-op until then.

## Grounding (real files/symbols)

- `engine-old/source/saffron/rendering/renderer_lighting.cpp` — `setSceneLighting`, `setDirectionalLight`,
  `setClusterCamera`, the cluster SSBO setup, the shadow-map setup.
- `engine-old/source/saffron/rendering/renderer_types.cppm` — `Lighting` (`:1141`, light/cluster buffers,
  the shadow-pending flags + transforms), `GpuLight` (`:2018`), `Targets` shadow maps (`:1331`–`:1338`).
- `engine-old/source/saffron/rendering/renderer.cppm` — the `shadow`/`spot-shadow`/`point-shadow`/
  `light-cull` passes in `beginFrameGraph`, `setShadows` (`:3066`), `setDirectionalShadow` (`:3101`),
  `setSpotShadow` (`:3107`), `setPointShadow` (`:3114`).
- Shaders: `light_cull.slang`, `point_shadow.slang`, `mesh.slang` (the forward+ shading + shadow sampling).
- README §3 (`GpuLight` layout), §6 (the feature→pass→shader row).

## Acceptance gate

- `cargo build -p saffron-rendering` and the workspace build are green.
- `cargo test -p saffron-rendering` passes named tests:
  - `GpuLight` size/offset asserts; the cluster-params UBO size assert.
  - `set_scene_lighting` writes the directional UBO + grows the punctual SSBO for N lights; the per-frame
    set rotates with `frame.index`.
  - the light-cull dispatch fills a cluster SSBO whose count for a froxel containing a known point light
    is ≥ 1 (the cull is correct, not empty).
- **Golden-image** tests: a lit sphere under one directional light + a directional shadow caster matches a
  committed golden within tolerance; a spot light + a point light each produce their shadow. Validation
  log clean across all three shadow passes.
