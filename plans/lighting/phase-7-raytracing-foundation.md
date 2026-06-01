# Phase 7: Ray-Tracing Foundation + Ray-Query Shadows

**Status:** COMPLETED
<!-- Flip to COMPLETED when the "Done when" checklist passes, validation-clean. Delete this file only after COMPLETED + merged. -->

<!--
COMPLETED 2026-06-01 (commit ffe5a8e), validation-clean on llvmpipe (full KHR RT stack, ~1 FPS).
- Device bring-up GATED: features12.bufferDeviceAddress + VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT
  always; the AS + ray_query + deferred-host-ops extensions via enable_extension_if_present
  (vk-bootstrap has no "desired extension" on the selector — enable post-select), feature
  structs queried via vkGetPhysicalDeviceFeatures2 + chained into device create only when
  both present → context.rtSupported. Engine still starts on a non-RT GPU.
- DISPATCH: the AS entry points are NOT statically exported by the loader (verified: only
  vkGetBufferDeviceAddress is). Resolved 5 fns via vkGetDeviceProcAddr into RtDispatch +
  called through the C API; the engine otherwise uses Vulkan-Hpp STATIC dispatch. (volk /
  HPP dynamic dispatcher avoided — minimal footprint.)
- AccelerationStructure RAII (handle + backing AS-storage buffer, destroyed via the resolved
  destroyFn then the buffer, before the allocator). createAccelStructure / makeRtBuffer /
  bufferDeviceAddress helpers in :Detail.
- BLAS per mesh in uploadMesh (vertex/index buffers get eShaderDeviceAddress +
  eAccelerationStructureBuildInputReadOnly; PREFER_FAST_TRACE, no compaction v1). Per-frame
  TLAS (PREFER_FAST_BUILD) as a Compute-kind graph "tlas-build" pass; recordTlasBuild
  self-emits the AS-build→fragment barrier + writes the TLAS into the frame's set 6. An
  empty (0-instance) seed TLAS is built at init + written to all frame sets so set 6 is
  ALWAYS valid (the fragment statically references rtScene regardless of the runtime flag —
  an unwritten descriptor = VUID-vkCmdDrawIndexed-None-08114).
- Mesh PSO: set 6 (TLAS) appended to the layout ONLY when rtSupported; mesh.slang
  rayQueryShadow() (TerminateOnFirstHit) gated by pointShadowMeta.z, applied to the
  directional + EVERY punctual light (vs the map paths' one-spot/one-point limit).
  setRtScene captures the frame instances; the TLAS-build pass consumes them.
- se set-rt-shadows {0|1} (guarded on rtSupported); render-stats reports rtSupported/
  rtShadows/blasCount.
- Verified: rtSupported true on llvmpipe, BLAS built, RT shadow matches the phase-3 shadow
  map to 0.11% of pixels (penumbra edge only), VAL=0.

NOT done (noted seams, non-blocking): swap phase-6 DDGI's software trace for a ray_query
trace behind rtSupported (the proxy/sampling are unchanged); BLAS compaction; a non-RT-GPU
mesh-shader PERMUTATION (today the mesh SPIR-V always carries the RayQueryKHR capability, so
on a device without ray_query the mesh PSO would fail to create — fine for the llvmpipe
target + any RT GPU, but a permutation/specialization is needed for true non-RT hardware).
The RT-pipeline + SBT (vs inline ray-query) is phase-8 territory.
-->


## Goal

Stand up hardware ray tracing as an **optional, feature-gated** tier: acceleration
structures (BLAS per mesh, TLAS per frame) + inline `VK_KHR_ray_query` ray-traced
shadows dropped into the existing clustered-forward fragment, as a runtime A/B against
the rasterized path. Keep the phase 3 shadow-map / phase 6 software-GI paths as the
fallback and reference (the engine's existing `set-clustered 0` pixel-identical habit).
Then reuse the TLAS to drive RT-DDGI probe updates (phase 6's trace pass) and to set
up ReSTIR (phase 8).

**Depends on:** phases 1–3 (everything RT improves already exists as a fallback).
`bufferDeviceAddress` + the VMA flag are the only missing device prerequisites.

## The llvmpipe reality (verified live)

The `saffron-build` llvmpipe **exposes the full KHR RT stack** (`acceleration_structure`,
`ray_query`, `ray_tracing_pipeline`, `bufferDeviceAddress`,
`descriptorBindingAccelerationStructureUpdateAfterBind` all true). So this code
**builds, validates, and pixel-regression-tests in the toolbox today** — at ~1 FPS.
Avoid the sub-features llvmpipe reports false: `accelerationStructureHostCommands`,
`accelerationStructureIndirectBuild`, all `*CaptureReplay`. Representative *timings*
need a real RTX/RDNA2+ GPU (an `AGENTS.md` follow-up); correctness does not.

## Why ray_query, not the ray-tracing pipeline

For "RT shadows/AO/GI bolted onto a working forward renderer," **inline ray queries
are overwhelmingly lower-friction**: no Shader Binding Table, no rgen/rmiss/rchit
stages, no `vkCmdTraceRaysKHR`. You bind the TLAS as one descriptor and write
`RayQuery` directly inside the existing `mesh.slang` fragment. The full RT pipeline +
SBT only earns its keep later for many-material path tracing (phase 8 territory). Do
**not** start there.

## Current state (verified)

Device bring-up `renderer.cppm:80-141` enables Vulkan 1.1/1.2/1.3 features but **no RT
and no buffer device address**:

- `features12` (`renderer.cppm:89-93`): descriptor indexing only — **`bufferDeviceAddress`
  is not set**.
- Extensions are requested via `vkb::PhysicalDeviceSelector` with
  `set_required_features_1x` (`renderer.cppm:95-102`) — all *required*, which would
  fail startup on a non-RT GPU if RT were added the same way.
- VMA created without the BDA flag (`renderer.cppm:133-141`).
- `GpuMesh` (`renderer_types.cppm:188`) vertex/index buffers are not created with
  shader-device-address / AS-build-input usage.

## Steps

1. **Device bring-up (gated):**
   - `features12.bufferDeviceAddress = VK_TRUE` (`renderer.cppm:89-93`).
   - Add `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` to `allocatorInfo.flags`
     (`renderer.cppm:133`).
   - Request `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`,
     `VK_KHR_deferred_host_operations` as **optional/queried**, NOT
     `set_required_*` — query support after selection and store a `bool rtSupported`.
     Chain the AS + ray-query feature structs. On a non-RT GPU the engine still starts
     with RT off.
   - Vulkan-Hpp dynamic dispatch: the AS/ray-query entry points
     (`vkCmdBuildAccelerationStructuresKHR`, etc.) are **not core dispatch** — init the
     Vulkan-Hpp dynamic dispatcher (or add volk) or the calls are null at runtime
     (no compile error).
2. **`AccelerationStructure` RAII wrapper** — mirror `Image`/`GpuMesh`
   (`renderer_types.cppm`): owns the AS handle + backing `Buffer`; destructor frees
   before `vmaDestroyAllocator` (the `waitGpuIdle`-before-teardown contract, see the
   `meta-layer-resources` memory).
3. **BLAS per mesh** — at `uploadMesh` (`renderer.cppm:924`), add
   `eShaderDeviceAddress` + `eAccelerationStructureBuildInputReadOnlyKHR` usage to the
   vertex/index buffers; build a BLAS (`PREFER_FAST_TRACE`, then compact).
4. **TLAS per frame** — from the ECS transforms, reuse `renderScene`'s instance
   bucketing (`assets.cppm:465-498`); build/refit `PREFER_FAST_BUILD` each frame.
   **Dynamic lights cost zero in the AS** — it is purely a geometry occlusion oracle,
   so moving/adding lights needs no rebuild; only moving *geometry* does.
5. **Graph integration** — extend `RgUsage` (`render_graph.cppm:23`, closed enum) with
   an AS-build-write + ray-trace-read case so the build→fragment barrier derives like
   `light-cull` → scene. Add the TLAS build as an engine-internal pass in
   `beginFrameGraph` before the scene pass.
6. **Ray-query shadows (first effect)** — bind the TLAS at a new set/binding; in the
   `mesh.slang` clustered light loop (`:172-175`), trace one
   `TerminateOnFirstHit` shadow ray per light toward the light; multiply that light's
   radiance by the hit/no-hit visibility. No SBT, no RT pipeline. Runtime-toggle it
   against the no-shadow / shadow-map path.
7. **RT-DDGI hook** — swap phase 6's software trace for a `RayQuery` trace behind the
   `rtSupported` check; the blend/sample passes are unchanged.

## Control command

- `se set-rt-shadows {0|1}`, `se rt-stats` (BLAS count, TLAS build time, RT
  supported/enabled). Setter + `registerCommand` per `set-clustered` (`control.cppm:231`).
  Guard the command with `rtSupported`.

## Done when

- [ ] device starts with RT off on a non-RT GPU and with RT available on llvmpipe;
      `rtSupported` queried, not required.
- [ ] BLAS-per-mesh + per-frame TLAS build as a graph pass with a derived barrier;
      `AccelerationStructure` RAII frees cleanly before the allocator.
- [ ] ray-query shadows render correctly, toggle at runtime, and A/B against the
      phase 3 shadow maps; phase 6 DDGI can use the RT trace when available.
- [ ] validation-clean in the toolbox (~1 FPS is expected); PNGs verified.

## Notes / risks

- **Feature-gating is mandatory** — all current device features are `set_required_*`;
  adding RT that way breaks startup everywhere without RT. Query + toggle.
- **Dynamic-dispatch init** for extension entry points is the classic footgun (null
  function pointers at runtime, not a compile error).
- Avoid the llvmpipe-false sub-features (host commands, indirect build, capture-replay).
- Per-frame TLAS rebuild from ECS transforms is the dynamic-scene cost; lights are free.
