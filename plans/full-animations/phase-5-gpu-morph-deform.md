# Phase 5 — GPU morph deform stage (compute) + runtime weight application

**Status:** NOT STARTED

**Depends on:** Phase 3 (morph delta bank on GPU + MorphComponent)

## Why

Morph deltas must be applied on the GPU, in object space, **before** skinning, writing the same 32-byte
deformed `Vertex` layout so every downstream pass reads it unchanged. This is the canonical
glTF/UE/Unity order and the minimal-footprint cutover: extend the existing deform-prepass family, do not
add a parallel deform path.

## Grounding (the existing skin prepass to compose with)

- `skin.slang` (compute): one thread/vertex, reads bind-pose `Vertex` (ByteAddressBuffer, 32 B) + skin
  influences (24 B) + joint palette, writes deformed `Vertex` (32 B). Deformed pos omits the model
  matrix (graphics passes apply it).
- The skin pass in `renderer.cppm:1215-1275`: a `Compute` `RgPass` declaring `StorageWriteCompute` on
  `deformedBuffer`/`prevDeformedBuffer`; consumers declare `VertexInputRead`; the graph derives the
  barrier (`render_graph.cppm` `usageInfo`/`applyAccess`).
- `SkinDispatch` (`renderer_types.cppm:620`): set + vertexCount + jointOffset + deformedOffset, built
  host-side in `submitDrawList` (`renderer_drawlist.cpp:434,645-700`), descriptor wired by `wireSet`
  (`:812`). `Skinning` state (`:1206`): deformed/prevDeformed buffers, `prevPaletteByEntity`.
- `DrawItem.skinned`/`jointOffset` (`renderer_types.cppm:582-600`) built in `renderScene`
  (`assets.cppm:5746-5800`).

## Decisions (locked)

1. **Morph runs as a compute stage immediately upstream of skin, into an intermediate "morphed base"
   buffer that the skin pass then consumes as its input vertex stream.** Order: `rest Vertex → morph
   (add weighted sparse deltas, renormalize normal, re-orthonormalize tangent) → morphedBase buffer →
   skin pass reads morphedBase instead of the static mesh vertices → deformed buffer`. For an
   **unskinned morph mesh**, the morph stage writes the deformed buffer directly (no skin pass for that
   instance) and the mesh draws from the deformed buffer like a skinned static stream — one deformed
   contract, the skin step is simply skipped when `jointCount == 0`.
   - This keeps `skin.slang` almost unchanged: its `inVertices` binding becomes "the morphed base for
     morphed instances, else the static mesh" — chosen host-side at `wireSet` time. No skin shader
     permutation.
2. **`morph.slang` (new compute kernel).** Bindings (ByteAddressBuffer scalar packing, like skin.slang):
   `[0]` static bind-pose `Vertex` (32 B), `[1]` the shared sparse `MorphDelta` bank (40 B records),
   `[2]` a per-instance active-morph list (target index + weight, only non-zero weights), `[3]`
   morphedBase output (32 B). Push constants: vertexCount, base-vertex offset, active-morph count, delta
   bank offsets. **Pass 1 (single-pass float accumulate)** for the v1 footprint: copy base, then for
   each active morph iterate its sparse deltas and add `weight·delta` to position/normal/tangent
   (scatter by `vertexIndex`). Because deltas are sparse and per-instance weights are few, single-pass
   float accumulation is correct and simplest; the UE-style integer-atomic two-pass design is a
   documented later optimization (Phase 6 risk note), not v1. Renormalize normal; re-orthonormalize
   tangent (Gram-Schmidt vs base tangent).
3. **One new `RgPass`, graph-derived barriers.** Add a `morph` Compute pass before `skin`
   (`renderer.cppm:1215`), declaring `StorageWriteCompute` on `morphedBaseBuffer`; the skin pass adds an
   `RgAccess{ morphedBaseBuffer, RgUsage::VertexInputRead }`-equivalent compute read
   (`StorageReadCompute`) so the morph→skin barrier is derived. Unskinned morph instances declare
   `StorageWriteCompute` directly on `deformedBuffer` (same buffer skin would write), and the graph
   serializes morph-then-consumers exactly as it does skin-then-consumers. **No hand-written barrier.**
4. **Host builds `MorphDispatch` like `SkinDispatch`.** Add `MorphDispatch { set; vertexCount;
   morphedBaseOffset; activeMorphCount; }` (`renderer_types.cppm`), built in `submitDrawList` from the
   per-instance resolved weights (`MorphWeightOverrideComponent` else `MorphComponent`, read on
   `DrawItem` — add `DrawItem.morphWeights`/`morphMesh`). `renderScene` (`assets.cppm:5746`) fills
   `DrawItem.morphWeights` from the mesh entity. The morphed-base buffer grows like the deformed buffer
   (`ensureDeformedCapacity` sibling). The active-morph list (non-zero weights only) is uploaded per
   instance — skip the dispatch entirely when all weights are zero (cost ∝ active morphs, per Unity).
5. **GPU mesh carries the shared delta bank** (uploaded in Phase 3's `uploadMesh` hook): `GpuMesh`
   gains a `Ref<Buffer> morphDeltas` + per-target offset table; bound read-only by every instance's
   morph dispatch.

## Edits

- `engine/assets/shaders/morph.slang` (new); CMake compiles it (the `*.slang → SPIR-V` step).
- `renderer_types.cppm`: `MorphDispatch`; `SceneDrawList.morphDispatches`/`prevMorphDispatches`;
  `Skinning` (or a new `Morphing`) state for the morphedBase buffers + capacity + `prevWeightsByEntity`;
  `GpuMesh.morphDeltas` + offsets; `DrawItem.morphWeights`.
- `renderer.cppm`: build the `morph` `RgPass` before `skin`; skin pass declares the morphedBase read;
  pipeline creation for the morph kernel (`Pipelines`).
- `renderer_drawlist.cpp`: build `MorphDispatch` in `submitDrawList`; `wireSet` morph variant; grow the
  morphedBase buffer; choose skin `inVertices` = morphedBase for morphed instances.
- `assets.cppm` `renderScene`: fill `DrawItem.morphWeights` from `MorphWeightOverrideComponent` else
  `MorphComponent`; mark `DrawItem.morphMesh` when the mesh has morph targets.

## Verification

- `make engine`; `make prepare-for-commit`; a headless present-only smoke with `AnimatedMorphCube`.
- Visual/headed: drive a morph weight (Phase 7 command) and observe deformation; with weight 0 the
  dispatch is skipped (profiler shows no morph pass cost).
- Skinned-morph mesh: morph applies before skin (the morphed bulge follows the bone, no swimming) —
  validate on a skinned+morph fixture.
- Validation-clean log (the e2e gate): the morph→skin barrier is present (no sync validation error).

## Risks

- **Order correctness:** morph must precede skin; reversing causes swimming. The buffer chain
  (morphedBase feeds skin's `inVertices`) enforces it structurally — there is no way to skin first.
- **`SkinMaxSetsPerFrame` cap** (256) also bounds morph dispatches; document the same cap, or a morph-
  heavy scene truncates. Keep morph + skin caps aligned (same per-frame budget).
- **Tangent stream:** the engine `Vertex` has no tangent today (`geometry.cppm:36`, "tangents deferred
  to material time"). v1 morph applies position + normal deltas; tangent re-derivation is in the
  material/lighting path, so the morph kernel only writes pos+normal into the 32 B `Vertex` — no layout
  change. dTangent storage (Phase 3) is forward-looking; document that v1 deform uses pos+normal.
- **Two-pass atomics** deferred: single-pass float is correct for small active-morph counts; record the
  UE integer-fixed-point two-pass design as the scale-up path so it is one extension, not a rewrite.
