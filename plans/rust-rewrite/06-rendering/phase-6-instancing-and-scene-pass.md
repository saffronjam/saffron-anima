# Phase 6 — instancing, draw-list batching, and the scene + depth-prepass passes

**Status:** COMPLETED

**Depends on:** 06-rendering:phase-5-pso-cache-and-upload, 06-rendering:phase-2-render-graph

## Goal

Port the per-frame draw path: `submit_draw_list` resolves each `DrawItem`'s material to a cached PSO,
batches by (pipeline, mesh) into instanced draws, deduplicates materials, uploads the frame's instance +
material SSBOs (grown on demand), and stores the structured `SceneDrawList`. Then the `scene` graphics
pass (shaded) and the optional `depth-prepass` (depth-only) record it. This is the first real
end-to-end frame: geometry → instanced draw → a lit (flat-ambient, pre-lighting) image. It also lands
the offscreen `ViewTargets` for the active view and the submit-closure seam.

## Why this shape (NO LEGACY)

- **Bindless means texture differences do not split a batch.** A `DrawBatch` shares pipeline + mesh; the
  albedo/material index is per-instance in the instance SSBO, not a per-batch descriptor
  (`renderer_types.cppm:604`). So batching is by (pipeline, mesh) only — the instance buffer carries the
  rest. `baseInstance` offsets into the frame's instance buffer; the vertex shader reads
  `InstanceData` by `firstInstance + gl_InstanceID`.
- **`InstanceData` is `#[repr(C)]` + bytemuck, 8×16-byte blocks, size-asserted** (model, normalMatrix,
  prevModel for TAA object motion, baseColor, texture uvec4, pbr, emissive — `renderer_types.cppm:1868`).
  Per-frame material dedup hashes `MaterialParamsData` by raw bytes into a `HashMap<u64, u32>` (the
  dedup index → `InstanceData.texture.w`); the byte hash must match the C++ exactly, which is why the
  std430 layout (phase 5) is load-bearing.
- **The grow-only per-frame buffers are `Vec`-backed and resized, never shrunk** (`Instancing`,
  `renderer_types.cppm:1182`): one instance SSBO + one dedup'd material SSBO per frame-in-flight, grown
  on demand. The joint/prev-joint palettes are wired by phase 12 (skinning) — the `Instancing` struct
  reserves their slots now but they stay empty until then.
- **The submit seam: `submit(closure)` / `submit_ui(closure)` stash `Box<dyn FnOnce(CommandBuffer)>`
  replayed inside the scene / ui pass** (`renderer.cppm:1073`). Ad-hoc geometry (editor gizmo) replays
  after the batched draw-list. Closures run sequentially on the render thread; they capture `Arc`
  handles, never `&mut Renderer` (README §2).
- **The active `ViewTargets` (offscreen color RGBA16F + depth D32) is borrowed `&mut self.views[active]`
  once per frame**, with `&Device` separate — the borrow split that makes the C++ `Image&` pattern legal.
- **One scene pass, one optional depth-prepass — no duplicate "forward2" path.** The depth-prepass
  (`renderer.cppm:271`) is gated by `use_depth_prepass`; when on it pre-fills depth so the scene pass is
  `eEqual`-tested. Both consume the same `SceneDrawList`.

## Grounding (real files/symbols)

- `engine-old/source/saffron/rendering/renderer_drawlist.cpp` — `submitDrawList` (both forms), the
  (pipeline, mesh) batching, the per-frame material dedup, `recordSceneDrawList`, `recordDepthPrepass`.
- `engine-old/source/saffron/rendering/renderer_types.cppm` — `DrawItem` (`:582`), `DrawBatch` (`:604`),
  `SceneDrawList` (`:644`), `Instancing` (`:1182`), `InstanceData` (`:1868`), `RenderStats` (`:710`),
  `ViewTargets` (`:1265`, the `offscreen`/`depth` images + per-view sets).
- `engine-old/source/saffron/rendering/renderer.cppm` — `submit`/`submitUi` (`:1073`), the `scene` +
  `depth-prepass` passes in `beginFrameGraph` (the `scene.name = "scene"` block ~`:1960`+,
  `depth-prepass` ~`:1383`), `setActiveView` (`:1090`), `setViewportDesiredSize` (`:1083`).
- README §2 (borrow split), §3 (`InstanceData` layout).

## Acceptance gate

- `cargo build -p saffron-rendering` and the workspace build are green.
- `cargo test -p saffron-rendering` passes named tests:
  - `submit_draw_list` batches 3 items of 2 meshes (mesh A ×2, mesh B ×1) into 2 batches with correct
    `baseInstance`/`instanceCount`; `RenderStats.batches == 2`, `instances == 3`.
  - two items with byte-identical `MaterialParamsData` dedup to one material SSBO entry (the byte-hash
    dedup holds); two distinct materials produce two.
  - `InstanceData` size/offset asserts pass.
- A **golden-image** test: render a known 2-mesh scene with flat ambient to the offscreen target, read it
  back, and compare to a committed golden PNG within a tolerance (the first end-to-end frame). Validation
  log clean.
