# Phase 5 — GPU morph deform stage (compute) + runtime weight application

**Status:** NOT STARTED

**Depends on:** Phase 3 (the shared `MorphDelta` bank on the GPU, `MorphComponent`/`MorphWeightOverrideComponent`, spawn seeding)

## Why

Morph deltas must be applied on the GPU, in object space, **before** skinning, writing the same 32-byte
deformed `Vertex` layout so every downstream pass (depth, shadow, G-buffer, scene, motion, RT BLAS)
reads it unchanged. This is the canonical glTF/UE/Unity order. It is the minimal-footprint cutover:
**extend the existing deform-prepass family — do not add a parallel deform path.** The morph-before-skin
order is made physically impossible to reverse by making the morph output buffer the skin pass's input
binding.

## Grounding (verified — the skin prepass to compose with)

- `engine/assets/shaders/skin.slang`: one thread/vertex compute kernel. `ByteAddressBuffer inVertices`
  (binding 0, 32 B `Vertex`), `inSkins` (1, 24 B `VertexSkin`), `StructuredBuffer<float4x4> jointMatrices`
  (2), `RWByteAddressBuffer outVertices` (3). Push `{ vertexCount, jointOffset, deformedOffset, pad }`.
  Deformed position omits the model matrix (graphics passes apply `inst.model`). The header documents the
  scalar-packing reason a typed `StructuredBuffer<Vertex>` shatters the layout — morph.slang must follow
  the same `ByteAddressBuffer` pattern.
- The skin `RgPass` in `renderer.cppm` (`doSkin` block, around line 1222–1276): `RgPassKind::Compute`,
  `accesses = { {deformedBuffer, StorageWriteCompute}, {prevDeformedBuffer, StorageWriteCompute} }`. The
  comment at ~1218 states explicitly that the static/skin/palette reads need **no** barrier because they
  are host-uploaded/static — this is exactly the invariant Phase 5 breaks for the morphed case.
- Consumers (`shadow` ~1300, `spot` ~1326, `point` ~1348, `depth` ~1391, `motion` ~1446-1447, `gbuffer`
  ~1475) each `accesses.push_back(RgAccess{ deformedBuffer, RgUsage::VertexInputRead })`. The graph
  derives the compute-write → vertex-input barrier in `render_graph.cppm` (`usageInfo`/`applyAccess`,
  `executeRenderGraph`).
- `RgUsage` (`render_graph.cppm:25-37`): `StorageWriteCompute` and `StorageReadCompute` both already
  exist — **no new variant is needed.** `VertexInputRead` derives to the vertex-input stage (wrong for a
  compute-stage read).
- `SkinDispatch` (`renderer_types.cppm:620`): `{ set; vertexCount; jointOffset; deformedOffset }`.
  `SceneDrawList.skinDispatches`/`prevSkinDispatches`/`skinnedRtInstances` (`:644-656`). `Skinning`
  (`:1206-1223`): `setLayout`, `pools[MaxFramesInFlight]`, `deformedBuffers`/`deformedCapacity`,
  `prevDeformedBuffers`/`prevDeformedCapacity`, `peakVertices`, `prevPaletteByEntity`,
  `prevModelByEntity`.
- `submitDrawList` (`renderer_drawlist.cpp:439`): buckets `DrawItem`s, builds `dispatches`/`prevDispatches`
  (~673-700), grows the deformed buffers (`ensureDeformedCapacity`/`ensurePrevDeformedCapacity` ~781-786),
  clamps to `SkinMaxSetsPerFrame` (~791-798), resets the per-frame pool (~799), and `wireSet` (~812-843)
  writes the 4 storage-buffer descriptors {static vertices, skin, palette, deformed}.
- `bindBatchVertices` (`renderer_drawlist.cpp:62-72`) and the scene-pass inline binder (~962-968) bind
  binding 0 = `deformedBuffers[frame]` for a skinned batch (offset = `deformedVertexOffset` ×
  `sizeof(Vertex)`), else `batch.mesh->vertexBuffer`. **Every geometry pass binds the same way**, so a
  deformed batch is drawn through the ordinary static `vertexMain`/`transformVertex` path.
- Skin descriptor layout (`renderer_detail.cppm:3118-3134`): 4 `eStorageBuffer` bindings, compute stage.
  Skin pool (`:3137-3152`): per frame-in-flight, `poolSizes = 8*SkinMaxSetsPerFrame` storage descriptors,
  `maxSets = 2*SkinMaxSetsPerFrame`, reset wholesale each frame. `SkinMaxSetsPerFrame = 64`
  (`renderer_types.cppm:78`).
- `DrawItem` (`renderer_types.cppm:582-598`): `mesh, model, normalMatrix, submeshMaterials, material,
  skinned, jointOffset, jointCount, entity`. `GpuMesh` (`:228-340`): `vertexBuffer`, `skinBuffer` (null
  unskinned), `indexCount`, `vertexCount`, `submeshes`, CPU copies. Phase 3 declared `GpuMesh.morphDeltas`
  + a per-target offset table (uploaded in the `uploadMesh` hook); this phase binds it.
- `renderScene` builds `DrawItem`s from scene entities (`assets.cppm`, around 5746-5800); this is where
  `DrawItem.morphWeights` is filled from the mesh entity's components.

## Decisions (locked)

1. **Morph runs as a compute stage immediately upstream of skin, into a "morphed base" buffer that the
   skin pass consumes as its input vertex stream.** Order:
   `rest Vertex → morph (copy base, add Σ weightᵢ·deltaᵢ to pos/normal, renormalize normal) → morphedBase
   → skin reads morphedBase instead of static vertices → deformed buffer`. The morph→skin order is
   structural: skin's `inVertices` *is* the morph output, so skin-first is physically impossible.
   - For an **unskinned morph mesh** the morph stage writes the **deformed buffer directly** (no skin
     dispatch for that instance) and the mesh draws from the deformed buffer through the same static
     `vertexMain`/`transformVertex` path that a skinned batch uses. One deformed-buffer contract.
   - **Model-matrix correctness (load-bearing):** a *skinned* deformed buffer is world-space (the palette
     is `worldBone · inverseBind`), so skinned batches draw with `inst.model = identity`. An
     *unskinned-morph* deformed buffer is **object-space** (morph adds object-space deltas, no skin
     matrix), so its batch must draw with `inst.model = the node world matrix`. `renderScene` already sets
     `DrawItem.model` to the node world matrix for non-skinned items and leaves it identity for skinned —
     so the unskinned-morph case is correct *as long as it stays a non-skinned `DrawBatch`* (skinned ==
     false) that simply reads the deformed buffer. Verify the batch-skinned flag and the `inst.model`
     fed to the deformed draw per kind so the matrix is neither dropped nor double-applied.
   - `skin.slang` is **not permuted**: its `inVertices` binding is pointed at morphedBase for a morphed
     instance and the static mesh otherwise, chosen host-side at `wireSet` time.

2. **`morph.slang` (new compute kernel), ByteAddressBuffer scalar packing.** A typed
   `StructuredBuffer<Vertex>` imposes std430 padding (float3 → 16-byte align → 48 B stride) and shatters
   the 32 B layout — the exact trap `skin.slang` documents. Bindings:
   - `[0]` `ByteAddressBuffer inVertices` — static bind-pose `Vertex` (32 B stride).
   - `[1]` `ByteAddressBuffer morphDeltas` — the shared sparse `MorphDelta` bank (28 B stride:
     `u32 vertexIndex; float3 dPosition; float3 dNormal`). Read-only, shared across instances.
   - `[2]` `ByteAddressBuffer activeMorphs` — the per-instance active-morph list (only non-zero weights),
     each entry `{ u32 targetIndex; float weight }` (8 B). Lives in a per-frame host-mapped ring (see #4).
   - `[3]` `RWByteAddressBuffer morphedBase` — deformed output (32 B `Vertex`).
   - Push `{ u32 vertexCount; u32 baseVertexOffset; u32 activeCount; u32 activeOffset; }` where
     `baseVertexOffset` is the instance's base in morphedBase (Vertex units), `activeOffset` is the
     instance's base in the active-morph ring (entry units). The per-target delta range
     `(firstDelta, deltaCount)` is read from the GPU per-target offset table (decision #5) addressed by
     `targetIndex`; pass the offset-table buffer as an additional `ByteAddressBuffer` binding or fold its
     two u32s into the active-morph entry at build time (prefer the latter — bake
     `{ targetFirstDelta; targetDeltaCount; weight }` per active entry, 12 B, so the kernel needs no extra
     binding and never indexes a separate table). Lock the active-entry layout in the kernel header.
   - **v1 single-pass float accumulate:** `i = SV_DispatchThreadID.x`, guard `i >= vertexCount`; load base
     pos/normal/uv for vertex `i`; for each of the `activeCount` active morphs, walk its
     `[targetFirstDelta, targetFirstDelta+targetDeltaCount)` slice of the bank and, where
     `delta.vertexIndex == i` (or scatter — see note), add `weight·dPosition`/`weight·dNormal`. Because
     the per-vertex loop must find the deltas affecting *this* vertex and the bank is sparse, v1 does the
     simple correct thing: **one thread per vertex, linear scan of each active target's small delta slice,
     accumulate matches.** (The UE-style integer-atomic two-pass `InterlockedAdd` scatter is the
     documented Phase-9 scale-up path — a perf knob, not a v1 rewrite.) Renormalize the morphed normal,
     copy uv unchanged, write the 32 B `Vertex`. Tangent is re-derived at material time (the `Vertex`
     carries no tangent stream — `geometry.cppm:36`).
   - Mirror skin.slang's static-const stride/offset constants (`VertexStride=32`, `NormalOffset=12`,
     `UvOffset=24`) and the per-thread layout exactly.

3. **One new `morph` `RgPass`; barriers graph-derived — with the explicit new skin read access.**
   - Add a `morph` Compute pass in `renderer.cppm` **before** the `doSkin` block (so it is added before
     `skin` and before every consumer). It declares `StorageWriteCompute` on `morphedBaseBuffer` (the
     skinned-morph case) and `StorageWriteCompute` directly on `deformedBuffer` (the unskinned-morph case,
     same buffer skin would otherwise write).
   - **The skin pass MUST add an explicit `RgAccess{ morphedBaseBuffer, RgUsage::StorageReadCompute }`.**
     Today skin declares no access on its input stream on purpose (host-uploaded). Once morph writes that
     input on the GPU, without this added read access the morph-write → skin-read barrier **never
     derives** — the exact seam where "the graph derives it" silently fails. Use `StorageReadCompute`
     (compute-stage read), **NOT** `VertexInputRead` (which derives to the vertex-input stage, wrong for a
     compute consumer). No hand-written barrier; no new `RgUsage`.
   - For the unskinned-morph instances, morph writes `deformedBuffer` with `StorageWriteCompute` and the
     existing consumers' `VertexInputRead` on `deformedBuffer` derive the morph→draw barrier exactly as
     skin→draw does today.
   - Gate the morph pass like `doSkin`: only add it when `!sceneDrawList.morphDispatches.empty() &&
     pipelines.morph && the morphedBase buffer exists`. Import `morphedBaseBuffer` with `importBuffer`
     only inside that gate.

4. **Host builds `MorphDispatch` like `SkinDispatch`.**
   - Add `struct MorphDispatch { vk::DescriptorSet set; u32 vertexCount; u32 morphedBaseOffset;
     u32 activeCount; u32 activeOffset; }` (`renderer_types.cppm`) and `SceneDrawList.morphDispatches`
     (current pose) — `prevMorphDispatches` is added in Phase 6 (motion), not here.
   - Add the morphed-base buffers to the deform state: extend `Skinning` (it already owns the deform
     buffers) with `morphedBaseBuffers[MaxFramesInFlight]` + `morphedBaseCapacity[MaxFramesInFlight]`, and
     an `ensureMorphedBaseCapacity` sibling of `ensureDeformedCapacity` (grow-only, in Vertex/32 B units).
     Keeping it on `Skinning` (not a new `Morphing` struct) keeps one deform-state owner; rename is
     unnecessary for v1.
   - Add `DrawItem.morphWeights` (`std::vector<f32>`) and `DrawItem.morphMesh` (bool, set when the mesh
     has morph targets). `renderScene` (`assets.cppm` ~5746) fills `morphWeights` from
     `MorphWeightOverrideComponent` if present, else `MorphComponent`, on the mesh entity, and sets
     `morphMesh` from `GpuMesh` having a non-empty morph bank.
   - In `submitDrawList`, when a bucket's mesh has morph targets **and** its resolved weights are not all
     zero, allocate a morphedBase slice (a `deformedCursor`-style cursor for the morphed-base buffer —
     reuse the same cursor as the deformed buffer so a skinned-morph instance's morphedBase slice and its
     deformed slice share the base vertex offset; this keeps the skin push `deformedOffset` and the morph
     push `baseVertexOffset` identical and avoids a second cursor), build the active-morph list (drop
     zero-weight targets), append it to the per-frame ring, and push a `MorphDispatch`.
   - **Active-morph list lives in a per-frame host-mapped ring**, mirroring `jointBuffers[frame]`: one
     grow-only `morphActiveBuffers[MaxFramesInFlight]` (HOST_VISIBLE, mapped), an `ensureMorphActive…`
     sibling, memcpy + `vmaFlushAllocation` once per frame. **Never** a fresh allocation per instance.
   - **Skip the dispatch when all weights are zero** (cost ∝ active morphs, not target count) — the
     bucket then draws the static stream (skinned-only or fully static) with no morph cost.
   - For a skinned-morph bucket: the morph dispatch writes the morphedBase slice, and the existing skin
     `wireSet` is changed so its `inVertices` (binding 0) points at `morphedBaseBuffers[frame]` (at the
     instance's base offset) instead of `mesh->vertexBuffer`. For an unskinned-morph bucket: there is no
     skin dispatch — the morph dispatch's `[3]` output is `deformedBuffers[frame]` and the batch is a
     normal non-skinned `DrawBatch` whose binding 0 is the deformed buffer (so add the unskinned-morph
     batch to the deformed-buffer draw path: it must set the same `deformedVertexOffset` binding behavior
     the skinned path uses, while remaining `skinned == false` so `inst.model` = node world is applied).

5. **GPU mesh carries the shared delta bank + per-target offset table** (uploaded in Phase 3's
   `uploadMesh` hook). `GpuMesh` gains `Ref<Buffer> morphDeltas` (the 28 B sparse bank, STORAGE,
   device-local, read-only) and a per-target `{ firstDelta, deltaCount }` offset table (a small CPU array
   on `GpuMesh`, used host-side to bake the active-entry 12 B records — the kernel never reads a table).
   Bound read-only by every morphed instance's dispatch.

6. **Descriptor pool decision (resolved here).** Morph **shares the existing per-frame skin pool**, but
   the pool sizing is grown to cover both deform stages. The morph set has 4 storage-buffer bindings
   (static vertices, delta bank, active list, output) — identical descriptor *type* to skin. Update
   `renderer_detail.cppm:3137-3152`:
   - The skin descriptor-set layout (4 storage buffers, compute) is reusable as-is for morph *only if*
     the active-entry table is folded into the active-morph buffer (decision #2) so morph also has exactly
     4 storage-buffer bindings. Reuse `renderer.skinning.setLayout` for morph sets — **do not** add a
     second layout (one code path).
   - Grow the pool: `poolSizes = eStorageBuffer, 4 * (SkinMaxSetsPerFrame /*skin cur*/ +
     SkinMaxSetsPerFrame /*skin prev*/ + SkinMaxSetsPerFrame /*morph cur*/)` and
     `maxSets = 3 * SkinMaxSetsPerFrame`. (Phase 6 adds `prevMorphDispatches`; grow `maxSets` to
     `4 * SkinMaxSetsPerFrame` and the storage count to match then — note it as a Phase-6 follow-up so
     the pool is not undersized when motion morphs land.) Add a `MorphMaxSetsPerFrame` only if you want a
     distinct cap; otherwise reuse `SkinMaxSetsPerFrame` and document the shared ceiling. Clamp
     `morphDispatches` to the cap with a `logWarn` mirroring the skin clamp (~791-798).

## Edits (ordered)

1. **`renderer_types.cppm`** — add `MorphDispatch`; `SceneDrawList.morphDispatches`; on `Skinning`:
   `morphedBaseBuffers`/`morphedBaseCapacity` + `morphActiveBuffers`/`morphActiveCapacity`; on `GpuMesh`:
   `morphDeltas` Ref<Buffer> + per-target offset table (if not already added in Phase 3); on `DrawItem`:
   `morphWeights` + `morphMesh`; add `Pipelines.morph` (`Ref<Pipeline>`).
2. **`engine/assets/shaders/morph.slang`** (new) — the compute kernel per decision #2; CMake already
   compiles `engine/assets/shaders/*.slang → SPIR-V` (confirm `morph.slang` is picked up by the glob; if
   the shader list is explicit, add it).
3. **`renderer_detail.cppm`** — reuse `skinning.setLayout` for morph sets; grow the skin pool sizing
   (decision #6); create the `morph` compute pipeline next to `skin` (`newComputePipeline(renderer,
   "morph", skinning.setLayout, …)`), storing it in `Pipelines.morph`.
4. **`renderer_drawlist.cpp`** — add `ensureMorphedBaseCapacity` + `ensureMorphActiveCapacity` siblings;
   in `submitDrawList` build the active-morph ring + `MorphDispatch`es (skip all-zero), allocate morph
   sets from the per-frame pool (a `wireMorphSet` lambda binding {static vertices, delta bank, active
   list ring, morphedBase|deformed output}); point a skinned-morph instance's skin `wireSet` `inVertices`
   at the morphedBase slice; add the unskinned-morph batch to the deformed-buffer draw path as a
   non-skinned batch reading the deformed buffer. Reset the shared pool once (it already resets at ~799 —
   keep one reset before allocating both skin and morph sets).
5. **`renderer.cppm`** — in `beginFrameGraph`, before the `doSkin` block: `importBuffer` the morphedBase
   buffer; add the `morph` `RgPass` (Compute, `StorageWriteCompute` on morphedBase and, for unskinned
   morph, on the deformed buffer) with an execute that binds `pipelines.morph` and dispatches each
   `MorphDispatch` (`(vertexCount+63)/64` groups, push the per-instance offsets). In the `skinPass`,
   append `RgAccess{ morphedBaseBuffer, RgUsage::StorageReadCompute }` so the morph→skin barrier derives.
6. **`assets.cppm` `renderScene`** — fill `DrawItem.morphWeights` (override else durable) and
   `DrawItem.morphMesh`; ensure an unskinned-morph entity yields a non-skinned `DrawItem` with
   `model = node world matrix` (already the default for non-skinned items — confirm).

## Frontend (Timeline / Clips / Inspector)

**None this phase.** The runtime weight plumbing this phase establishes is driven by `MorphComponent` /
`MorphWeightOverrideComponent` (Phase 3) and inspected/driven over the control plane in Phase 7; the
Inspector sliders and Timeline channel drill-down land in Phase 8. Do not add any editor UI here.

## Control commands

**None this phase.** The `set-morph-weights` / `get-morph-weights` commands land in **Phase 7**; the
runtime state they drive (`MorphWeightOverrideComponent` written before the deform stage, the
deform stage consuming it) is what this phase builds. To exercise the GPU stage before Phase 7, drive
weights via the existing animation tick (Phase 4 writes `MorphWeightOverrideComponent`) or via a
temporary scene-component edit; do **not** add a throwaway command.

## Performance

- **Cost ∝ active (non-zero) morphs × moved vertices.** Sparse deltas + skipping all-zero dispatches mean
  a 100-shape rig with two active blendshapes pays for two. The delta bank is shared read-only across all
  instances of a mesh; per-instance state is just the active-morph list (a few 12 B entries) + the
  morphedBase output slice.
- **The active-morph list is a per-frame host-mapped ring** (one grow-only buffer per frame-in-flight,
  one memcpy + flush per frame), not a per-instance allocation — the same discipline as `jointBuffers`.
- **VRAM round-trip (documented, not optimized in v1):** the skinned-morph chain
  `rest → morph (write morphedBase) → skin (read morphedBase, write deformed) → draw (read deformed)`
  round-trips the full 32 B stream through VRAM twice. A **fused morph+skin kernel** (apply active deltas
  in registers, then skin, single write) is the lowest-bandwidth option but awkward with sparse scatter —
  document it as a future perf knob. v1 keeps the separate-pass design: simpler, and the buffer chain
  enforces morph-before-skin structurally.
- **Two-pass integer-atomic scatter (UE)** is the Phase-9 scale-up path for very dense rigs; v1
  single-pass float accumulate is correct for small active-morph counts, so the upgrade is one extension,
  not a rewrite.
- **Descriptor-pool budget:** `SkinMaxSetsPerFrame = 64` is shared (decision #6). A morphed+skinned
  instance now consumes a morph set + a skin set (and, in Phase 6, prev variants of each), so document
  the effective per-frame deformed-instance ceiling and grow the pool sizing accordingly. The per-frame
  deformation budget (Phase 9) reconciles the final numbers.

## Tests

- `make engine` clean; `make prepare-for-commit` (format + lint) clean — fix every warning this change
  raises.
- **Headless present-only smoke with `AnimatedMorphCube`** (the Phase 3 import fixture): boot the engine
  headless (weston backend, `SAFFRON_EXIT_AFTER_FRAMES=N`), spawn the morph cube, and confirm a
  validation-clean log — specifically that the **morph→skin barrier is present** (no Vulkan
  synchronization-validation error) when a morphed-skinned mesh is in the scene, and that the morph→draw
  barrier is present for the unskinned-morph case.
- **Weight-driven deformation:** drive a morph weight (via the Phase 4 animation tick writing
  `MorphWeightOverrideComponent`, or once Phase 7 lands, the `set-morph-weights` command) and observe the
  cube deform. Verify a captured frame (headed or screenshot) shows the morphed silhouette.
- **Zero-weight skip:** with all weights 0, assert no morph dispatch is recorded (the bucket draws the
  static stream); confirm via the profiler (`profiler.set-mode`) that the morph pass cost is absent.
- **Skinned-morph (no swimming):** on a skinned+morph fixture, the morphed bulge **follows the bone** —
  morph is applied before skin (object space), so the deformation rides the skeletal motion rather than
  swimming in world space.
- **tests/e2e:** the full e2e morph-deformation assertion lands in **Phase 9** (drive the Phase-7 command
  over the control plane and assert a validation-clean log + deformation). This phase's gate is the
  present-only smoke above; do not add an e2e test that depends on the not-yet-existing command.

## Acceptance criteria

- `morph.slang` applies weighted sparse deltas (copy base, add Σ weight·delta to pos/normal, renormalize
  normal) in object space **before** skinning; `morphedBase` feeds the skin pass's `inVertices` binding,
  so morph-before-skin is enforced **structurally** (skin-first is impossible).
- The skin pass declares an explicit `RgAccess{ morphedBaseBuffer, StorageReadCompute }`; the
  morph→skin barrier is **graph-derived** (no hand-written barrier, no synchronization-validation error).
  `StorageReadCompute`, not `VertexInputRead`.
- An **unskinned morph mesh** draws from the deformed buffer (morph writes it directly) with the correct
  **node world** model matrix (non-skinned batch; `inst.model` applied, not dropped or double-applied).
- **Zero-weight instances skip the dispatch**; the active-morph list lives in a per-frame host-mapped
  ring (no per-instance allocation).
- `GpuMesh` exposes the shared read-only `MorphDelta` bank + per-target offset table; every morphed
  instance's dispatch binds the same bank.
- The morph descriptor sets reuse `skinning.setLayout` and the (grown) per-frame skin pool — one deform
  set layout, one pool.
- `make engine` + `make prepare-for-commit` clean; `AnimatedMorphCube` deforms with a validation-clean
  log.

## Risks

- **Order correctness:** morph must precede skin; reversing causes swimming. The buffer chain
  (morphedBase IS skin's `inVertices`) enforces it structurally — there is no skin-first code path.
- **The missing barrier seam:** forgetting the `StorageReadCompute` access on the skin pass silently
  drops the morph→skin barrier (the skin pass's input reads were barrier-free by design). The
  validation-clean-log gate is the catch; the explicit access is the fix.
- **Model-matrix double/none for unskinned morph:** the unskinned-morph batch must stay a non-skinned
  `DrawBatch` (so `inst.model` = node world is applied) while still reading the deformed buffer. Getting
  the skinned flag wrong drops the world transform (mesh at origin) or double-applies it. Covered by the
  unskinned-morph acceptance check.
- **Descriptor-pool undersizing (Phase 6):** the shared pool must grow when `prevMorphDispatches` land in
  Phase 6; note it so the pool is not silently exhausted (allocate failures → dropped dispatches →
  undeformed draws).
- **VRAM round-trip / no fused kernel in v1:** documented as a perf knob; v1 trades bandwidth for the
  structural morph-before-skin guarantee and a non-permuted skin kernel.
