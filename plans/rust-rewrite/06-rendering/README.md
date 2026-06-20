# 06 — Rendering: ash bring-up, the render graph, and every render feature

This is the single hardest area of the rewrite, and the difficulty is **not** `ash`. ash is the easy,
mechanical layer — a near-1:1 match for the engine's no-exceptions / no-RAII / PFN-dispatch Vulkan-Hpp
style. The cost is two things, both of which fail *silently* if they drift:

1. **Re-architecting the ~80-field by-reference `Renderer` god-aggregate into borrow-checker-legal
   sub-state.** Today every free function takes `Renderer&` and mutates whatever it likes, and per-frame
   code holds `Image&` / `ViewTargets&` sub-references while sibling fields mutate. That is illegal
   under Rust's one-`&mut` rule. The fix is a deliberate split into owned sub-state with a borrow
   discipline (§4), not a transcription.
2. **Porting the `RgUsage`→barrier engine, the std430 GPU-layout contracts, the RAII drop order, and the
   concurrency points with zero semantic drift.** A missing barrier is a data race; a wrong std430
   offset is a corrupted material hashed by raw bytes; a wrong Drop order is a use-after-free at
   teardown. None of these is a compile error — only the validation layer and the golden-image / e2e
   gate catch them.

The whole area lives in **one crate, `saffron-rendering`** (PP-1 keeps it whole, internal mods), with
`#![allow(unsafe_code)]` at the crate root and a top-of-file justification naming the ash seam — the
only `unsafe` in the engine besides Jolt and the shm publisher. We reject `wgpu` (cannot express
bindless-at-scale, RT pipelines, custom barriers, or the exact ABIs) and `vulkano` (auto-sync fights
the hand-derived barrier graph). The allocator pick (`vk-mem-rs` real-VMA vs `gpu-allocator` pure-Rust)
is a PP-2 decision; this plan is written against whichever lands, since both expose create/destroy/map/
budget at the call sites the engine uses.

The area is large enough to sub-divide; the phase files below are dependency-ordered increments, each
leaving the workspace compiling and its named test green. Many interleave with other areas in the
master order (you cannot bring up the device before `00-foundations`, nor render a scene before
`03-ecs-and-scene` and `07-assets-and-materials` feed it draw items) — PP-14 resolves the global order.

---

## 1. The lifecycle the area must reproduce

The C++ run loop drives the renderer through a fixed sequence per frame:

```
beginFrame → (app onRender → submit/submitUi closures) → beginFrameGraph (build cull+scene+post passes)
           → (app onRenderGraph adds passes) → endFrame (finish ui pass, executeRenderGraph, present/publish)
```

`beginFrameGraph` (`renderer.cppm:1112`) is the heart: it imports the frame's images/buffers into a
fresh `RenderGraph` and declares every pass conditionally (skin, shadows, cull, depth-prepass, motion,
gbuffer, gtao, ao-blur, contact, ssgi/blur/accum, ddgi×5, tlas-build, restir×3, sky, scene, fxaa, taa,
ssgi-history, tonemap, grid, editor-overlay, then the ui/present pass). `executeRenderGraph`
(`render_graph.cppm:541`) derives each pass's barriers from its declared usage and records the body.
The submit seam (`submit`/`submitUi`, `renderer.cppm:1073`) stashes closures replayed inside the
matching pass; in Rust these become `Box<dyn FnOnce(CommandBuffer)>` recorded on the render thread only.

## 2. Borrow-checker strategy for the aggregate (the load-bearing decision)

The C++ `Renderer` (`renderer_types.cppm:1735`) is one struct of ~40 named sub-structs
(`VulkanContext`, `Swapchain`, `FrameSync`, `Descriptors`, `Lighting`, `Instancing`, `Skinning`,
`Pipelines`, `Targets`, `views[ViewCount]`, `Ibl`, `ReflectionProbes`, `Sky`, `Ssao`, `Ddgi`, `Rt`,
`Restir`, `restirViews[ViewCount]`, `FrameGraphState`, the profiler stack, …) plus loose scalars. Every
function takes `Renderer&`. The Rust split (locked here, detailed per phase):

- **`Device` (the immutable-after-init core): `VulkanContext` + the VMA allocator + resolved PFN
  dispatch tables (`RtDispatch`, calibrated-timestamps, debug-utils labels).** Constructed once in
  `Renderer::new`, then borrowed `&Device` (shared, read-only) by nearly everything. This is the bucket
  that lets many passes hold a handle while siblings mutate, because it is never `&mut` after init.
- **Per-area owned sub-state structs** (`Lighting`, `Instancing`, `Skinning`, `Ibl`, `Ssao`, `Ddgi`,
  `Rt`, `Restir`, `ReflectionProbes`, `Sky`, `Pipelines`, `Descriptors`, `Targets`) each own their
  Vulkan handles and Drop them. They are siblings on the `Renderer` and are mutated by their own
  methods taking `&mut self.<area>` plus `&Device`.
- **`Views: [ViewTarget; ViewCount]`** — the per-pane viewport-sized images + temporal/history state +
  the per-view descriptor sets that bind those images (`ViewTargets`, `renderer_types.cppm:1265`).
  `ReSTIR` per-view reservoirs (`RestirView`) ride alongside. The active view is selected by index, so
  per-frame code borrows `&mut self.views[active]` once and the `&Device` separately — the split that
  makes the C++ `Image&` + sibling-mutate pattern legal.
- **Frame-graph building** never holds `&mut Renderer` across the closure capture. The C++ closures
  capture `Renderer&`; the Rust passes capture the **specific** `Arc`/`&Device` handles + plain
  `Copy` push-constant data they need (the per-pass `execute` is `FnOnce(CommandBuffer)` built from
  already-resolved handles, not a live `&mut Renderer`). The graph's `execute` phase borrows the
  resource table only.

The principle stated here, the mechanics proven per phase: **the device is shared-immutable; each
feature owns its handles and mutates them through a narrow method; per-frame closures capture resolved
handles, never the aggregate.** Two concurrency sites are explicit `Arc<Mutex>` (§5).

## 3. The std430 / `#[repr(C)]` + bytemuck contract

Every GPU-uploaded struct is a triple/double contract: the Rust definition must byte-match the std430
layout the Slang shader reads, and `MaterialParamsData` is additionally **hashed by raw bytes** for
per-frame material dedup — a wrong offset corrupts the dedup, not just a render. The rule: every such
struct is `#[repr(C)]`, derives `bytemuck::{Pod, Zeroable}`, pins `glam::Vec4`/`Mat4` (16-byte aligned)
or padded `Vec3`+`f32` (never `Vec3A` implicitly — measure the field), and carries a
`const _: () = assert!(size_of::<T>() == N)` matching the C++ `static_assert`. The known structs:
`InstanceData` (`renderer_types.cppm:1868`, 8×16B blocks), `MaterialParamsData`
(`:1884`, `static_assert == 96`), `GpuLight` (`:2018`, 4×vec4), the cluster-params UBO, the per-probe
`ProbeMeta`, the DDGI box SSBO, the ReSTIR reservoir record, and the directional-light UBO. The base
`Vertex` (32B) / `VertexSkin` (24B) come from `saffron-geometry` (already size-asserted there).

## 4. Resource RAII and Drop order

The C++ move-only wrappers (`Image`, `Image3D`, `Buffer`, `GpuMesh`, `GpuTexture`, `Pipeline`,
`AccelerationStructure`; `renderer_types.cppm:96–544`) each free their handle in a hand-written dtor +
move-assignment. In Rust these are `impl Drop` types, move-only by default, so the copy/move boilerplate
evaporates. The **order** is the design concern: the device must outlive every resource, and
`vmaDestroyAllocator` must run after every VMA allocation is freed. We encode this by struct field order
(`Device` last) + explicit Drop where field order is insufficient — `waitGpuIdle` runs before any
teardown (the run loop's responsibility, PP-10), so no resource is freed under a live GPU read. The
bindless free-list is `Arc<Mutex<Vec<u32>>>` shared with every `GpuTexture` so a texture's Drop can
return its slot even off the main thread (§5).

## 5. The two explicit shared-mutable sites

PP-1 marks these up front: the C++ free-function mutex singletons (`renderer_types.cppm:33,42`) are the
only places `Arc<Mutex>` is mandatory in rendering.

- **`gpuQueueMutex()`** — the graphics queue is externally synchronized; the frame loop **and** the
  thumbnail worker thread both `submit2`/`presentKHR` on it. → the queue handle lives behind
  `Arc<Mutex<Queue>>` (or a `Mutex` guarding the submit/present call site).
- **`bindlessMutex()`** — the bindless descriptor set + its free-list are written by uploads from both
  threads. → `bindlessFreeList: Arc<Mutex<Vec<u32>>>` and the `vkUpdateDescriptorSets`/claim path takes
  the lock. Every `GpuTexture` holds a clone of the `Arc` so its Drop returns the slot.

Everything else is `Arc<T>` (read-shared assets: `Arc<GpuMesh>`, `Arc<GpuTexture>`, `Arc<Pipeline>`) or
owned-and-`&mut`-through-a-method. The thumbnail worker has its own command pool (`workerCommandPool`,
`renderer_types.cppm:1795`) because Vulkan command pools are not thread-safe.

## 6. The feature → passes → shaders → acceptance-test matrix

Every feature in the ~16.5k-LOC renderer, mapped to the phase that ports it:

| Feature | Render-graph passes (names from `beginFrameGraph`) | Slang shaders | Phase |
|---|---|---|---|
| Device + swapchain bring-up | (none — clear+present) | — | 1 |
| Render graph (RgUsage→barrier) | engine, not a pass | — | 2 |
| GPU resource wrappers (Drop) | — | — | 3 |
| Bindless textures + samplers | — | — | 4 |
| Mesh/texture upload + PSO cache (übershader) | — | `mesh`, `thumbnail`, `preview` | 5 |
| Instancing + draw-list batching + scene/depth pass | `scene`, `depth-prepass` | `mesh` | 6 |
| Directional/spot/point + clustered lighting | `light-cull`, `shadow`, `spot-shadow`, `point-shadow` | `light_cull`, `point_shadow`, `mesh` | 7 |
| IBL + sky + reflection probes | `sky` (+ off-graph bakes) | `ibl_*`, `atmos_*`, `sky` | 8 |
| Screen-space GTAO + contact + SSGI | `gbuffer`, `gtao`, `ao-blur`, `contact`, `ssgi`, `ssgi-blur`, `ssgi-accum`, `ssgi-history` | `gbuffer`, `gtao`, `ao_blur`, `contact`, `ssgi`, `ssgi_blur`, `ssgi_accum`, `copy_color` | 9 |
| Motion vectors + TAA + FXAA + MSAA | `motion`, `taa`, `fxaa` (+ MSAA resolve in graph) | `motion`, `taa`, `fxaa` | 10 |
| Tonemap + grid + editor overlay (mandatory post) | `tonemap`, `grid`, `editor-overlay` | `tonemap`, `grid`, `gizmo_overlay` | 11 |
| GPU skinning prepass + skinned-BLAS | `skin` | `skin` | 12 |
| RT TLAS + ray-query shadows | `tlas-build` | `mesh` (ray-query) | 13 |
| DDGI (voxel GI) | `ddgi-voxelize`, `ddgi-trace`, `ddgi-blend-irr`, `ddgi-blend-dist`, `ddgi-border` | `ddgi_*` | 14 |
| ReSTIR DI | `restir-initial`, `restir-reuse`, `restir-resolve` | `restir_*` | 15 |
| Capture / shm publish + thumbnails + profiler | (present-time blit/copy) | — | 16 |

Shaders are compiled by `slangc` from `xtask`/`build.rs` (PP-12 owns the fan-out + the `lighting.slang`
module-precompile trick); this area consumes the resulting `.spv` exactly as the C++ loads them.

## 7. Grounding (real files / symbols)

| What | File | Symbols |
|---|---|---|
| Module overview + the rules-that-break | `engine-old/source/saffron/rendering/AGENTS.md` | files table; barrier/submit/drop rules |
| The aggregate + all sub-state structs + RAII wrappers + std430 types | `engine-old/source/saffron/rendering/renderer_types.cppm` | `Renderer` (`:1735`), `VulkanContext` (`:1036`), `Swapchain` (`:1055`), `Descriptors` (`:1113`), `Lighting` (`:1141`), `Instancing` (`:1182`), `Skinning` (`:1206`), `Pipelines` (`:1227`), `ViewTargets` (`:1265`), `Targets` (`:1329`), `Ibl` (`:1402`), `ReflectionProbes` (`:1451`), `Sky` (`:1468`), `Ssao` (`:1488`), `Ddgi` (`:1583`), `Rt` (`:1631`), `Restir` (`:1671`)/`RestirView` (`:1685`), `FrameGraphState` (`:1702`); `Image`/`Buffer`/`GpuMesh`/`GpuTexture`/`Pipeline`/`AccelerationStructure`; `InstanceData`/`MaterialParamsData`/`GpuLight`; `gpuQueueMutex`/`bindlessMutex` (`:33`/`:42`) |
| The barrier-derivation engine | `engine-old/source/saffron/rendering/render_graph.cppm` | `RgUsage` (`:25`), `usageInfo` (`:281`), `applyAccess` (`:342`), `seedImageState` (`:325`), `importImage`/`importBuffer` (`:392`/`:419`), `executeRenderGraph` (`:541`) |
| Lifecycle + frame-graph assembly + feature toggles | `engine-old/source/saffron/rendering/renderer.cppm` | `newRenderer` (`:127`), `destroyRenderer` (`:595`), `beginFrame` (`:920`), `beginFrameGraph` (`:1112`), `endFrame` (`:2451`), `addTonemapPass` (`:2296`), `buildTlas` (`:2876`), `setDdgiScene` (`:3018`), the `setX`/`Xenabled` toggle pairs |
| Draw-list batching + scene/depth recording | `engine-old/source/saffron/rendering/renderer_drawlist.cpp` | `submitDrawList`, `recordSceneDrawList`, `recordDepthPrepass` |
| Texture upload + bindless slot alloc + SVG | `engine-old/source/saffron/rendering/renderer_textures.cpp` | `uploadTexture`, `uploadTextureFloat`, the `bindlessMutex` write (`:132`) |
| PSO creation + übershader cache | `engine-old/source/saffron/rendering/renderer_pipelines.cpp` | `requestMeshPipeline` (`:205`/`:238`), the cache key |
| Lighting UBOs + clusters + shadow setup | `engine-old/source/saffron/rendering/renderer_lighting.cpp` | `setSceneLighting`, `setClusterCamera`, the shadow setters |
| IBL bake chain | `engine-old/source/saffron/rendering/renderer_detail_ibl.cpp` | `bakeEnvironment`, `captureReflectionProbe` |
| AA toggles + MSAA/FXAA/TAA target (re)creation | `engine-old/source/saffron/rendering/renderer_aa.cpp` | `setAa` (`:67`), `clampSampleCount` (`:43`), `recreateMsaaTargets`/`recreateFxaaTarget`/`recreateTaaTargets` |
| Capture + shm publish | `engine-old/source/saffron/rendering/renderer_capture.cpp` | `captureViewport` (`:47`), `publishShmPublishSlot` (`:270`), the 32-byte header + release fence (`:129`,`:291`,`:301`) |
| Thumbnails + worker-thread pool | `engine-old/source/saffron/rendering/renderer_thumbnail.cpp` | `bindThumbnailWorkerThread` (`:1125`), `encodeAssetThumbnailPng` |
| Profiler (timestamps + pipeline stats + capture) | `engine-old/source/saffron/rendering/renderer_profiler.cpp` | `readbackGpuTimings`, `tickCapture`, `calibrateTimestamps` |
| Low-level Vulkan helpers, shader load, descriptor/pass recording | `engine-old/source/saffron/rendering/renderer_detail.cppm` | internal `:Detail` partition |
| Base vertex/index layout (consumed, not owned here) | `engine-old/source/saffron/geometry/geometry.cppm` | `Vertex` (`:36`, 32B), `VertexSkin` (`:63`, 24B), `Submesh` (`:45`) |

## 8. Phase list

1. `phase-1-device-swapchain-bringup` — instance/device/allocator/swapchain + a validation-clean clear+present.
2. `phase-2-render-graph` — the `RgUsage`→barrier engine as a standalone, unit-tested unit.
3. `phase-3-gpu-resources` — the Drop-based resource wrappers + the Device sub-state + teardown order.
4. `phase-4-bindless-and-samplers` — the bindless descriptor table, samplers, slot alloc/reclaim.
5. `phase-5-pso-cache-and-upload` — mesh/texture upload + the übershader PSO cache.
6. `phase-6-instancing-and-scene-pass` — draw-list batching + the scene + depth-prepass passes.
7. `phase-7-lighting-and-shadows` — clustered cull + directional/spot/point shadows.
8. `phase-8-ibl-sky-probes` — IBL bake chain, visible sky, reflection probes, atmosphere.
9. `phase-9-screen-space-gi` — G-buffer, GTAO, contact shadows, SSGI (+ denoise/accum/history).
10. `phase-10-aa-and-temporal` — motion vectors, TAA, FXAA, MSAA.
11. `phase-11-tonemap-grid-overlay` — the mandatory tonemap + grid + editor overlay.
12. `phase-12-skinning-prepass` — the compute skinning prepass + skinned-BLAS refit.
13. `phase-13-ray-tracing` — RT TLAS build + ray-query shadows.
14. `phase-14-ddgi` — voxel-traced dynamic diffuse GI.
15. `phase-15-restir` — ReSTIR DI many-light direct lighting.
16. `phase-16-capture-shm-profiler` — capture, the shm publish interface, thumbnails, the profiler.
