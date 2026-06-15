# Phase 6 — Motion vectors + RT BLAS for morphed geometry

**Status:** NOT STARTED

**Depends on:** Phase 5 (the morph deform stage + deformed buffer)

## Why

TAA and motion blur ghost on morph-driven motion (blinks, lip movement, an unskinned morph cube
deforming) unless the previous frame's morphed position is reconstructed. RT shadows/GI must trace the
post-morph geometry. Both must "just work" by carrying morph through the existing motion + BLAS
plumbing, with no new motion or BLAS code path.

## Grounding (the existing motion + BLAS plumbing)

- Motion: a parallel `prevSkinDispatches` deforms the **previous** pose into `prevDeformedBuffers`
  (`renderer.cppm:1264-1272`, `renderer_types.cppm:1215`); `motion.slang` reads current (binding 0) +
  previous (binding 1) deformed streams and outputs NDC delta. `prevPaletteByEntity`
  (`renderer_drawlist.cpp:696-703,886`) caches last frame's palette so frame N+1 reprojects from N. A
  new entity reads back current==previous (zero motion frame 1).
- BLAS: `recordSkinnedBlasBuilds` (`renderer_detail.cppm`) reads the deformed buffer as
  `AccelStructBuildRead` (graph-derived barrier), BUILD first frame then UPDATE (refit). Keyed by entity
  uuid. `SkinnedRtInstance` carries `deformedOffset` (`renderer_types.cppm:633`).

## Decisions (locked)

1. **Morph rides the exact same prev-buffer mechanism as skinning.** Add `prevMorphDispatches` parallel
   to `prevSkinDispatches`: deform the **previous** frame's morph weights into the morphedBase used by
   the prev-skin pass (skinned case), or directly into `prevDeformedBuffers` (unskinned morph case). The
   motion pass already reads cur+prev deformed — it needs **no change**: the prev-deformed stream now
   reflects prev morph weights too, so `prevPos != curPos` captures morph motion automatically.
2. **`prevWeightsByEntity` cache, mirroring `prevPaletteByEntity`.** In `Skinning`/`Morphing` state, add
   `std::unordered_map<u64, std::vector<f32>> prevWeightsByEntity`. `submitDrawList` reads last frame's
   weights for the prev-morph dispatch and writes this frame's at the end — the identical pattern to the
   palette cache (`renderer_drawlist.cpp:886`). A new entity reads back current==previous (zero morph
   motion frame 1, no velocity flash).
3. **BLAS is fed the final deformed buffer unchanged.** Because morph writes into the same deformed
   buffer skinning produces (the morphedBase→skin→deformed chain, or morph→deformed directly), the
   skinned-BLAS refit (`recordSkinnedBlasBuilds`) already reads post-morph+post-skin positions at the
   instance's `deformedOffset` — **no BLAS code change**. An unskinned morph mesh that needs RT gets a
   per-instance refit BLAS the same way a skinned one does (extend the `skinnedRtInstances` push in
   `renderScene` to include unskinned morph instances; one list, the gate is "deforms this frame", not
   "is skinned").
4. **Refit vs rebuild policy (documented, not new machinery).** Topology is fixed (morph changes only
   positions/normals, never triangle/index counts), so UPDATE/refit stays valid — the existing BUILD-
   once-then-UPDATE policy holds. Note the standard caveat (periodic rebuild after large cumulative
   deformation) as a documented future tuning knob, consistent with the existing skinned-BLAS comments;
   no rebuild scheduler in v1.

## Edits

- `renderer_types.cppm`: `SceneDrawList.prevMorphDispatches`; `prevWeightsByEntity` in the morph/skin
  state; widen `SkinnedRtInstance` usage to "deformed instance" (rename intent, keep the struct — its
  fields already fit unskinned-morph: `deformedOffset`/`vertexCount`/`indexCount`/`mesh`).
- `renderer.cppm`: dispatch `prevMorphDispatches` in the morph pass (prev morphedBase / prev deformed);
  the motion + tlas passes are unchanged (they already read the prev/cur deformed buffers).
- `renderer_drawlist.cpp`: build `prevMorphDispatches`; `prevWeightsByEntity` read/write; include
  unskinned morph instances in `skinnedRtInstances` when RT consumes the scene.
- `assets.cppm renderScene`: push unskinned-morph instances into the RT instance list (identity model
  matrix? No — unskinned morph keeps its node/world model matrix; the deformed buffer for an unskinned
  morph mesh is in **object** space, so its TLAS transform is the instance world matrix, unlike the
  skinned case which is identity. Record this distinction explicitly so the TLAS transform is correct.)

## Verification

- `make engine`; `make prepare-for-commit`.
- TAA: animate a morph with no skinning (the morph cube) under a static camera; assert no ghosting (the
  prev-deformed stream reflects prev weights). A frame-1 check shows zero morph motion (no flash).
- RT enabled: shadows/GI trace the morphed silhouette (the BLAS refit reads post-morph positions).
- Validation-clean log: morph→skin→motion and morph→AS-build barriers all derived, no sync errors.

## Risks

- **Unskinned-morph TLAS transform space.** Skinned deformed verts are world-space (palette =
  world·inverseBind), so TLAS transform = identity. Unskinned morph deformed verts are **object**-space
  (no model matrix applied, matching how the raster path applies `inst.model`), so the TLAS transform
  must be the instance's world matrix. Getting this wrong puts morphed RT geometry at the origin. This
  is the one genuinely new RT consideration; call it out in the BLAS code + docs.
- **Prev-weights for a just-spawned entity:** read back current==previous (the palette pattern) to
  avoid a frame-1 velocity flash.
- **Cap interaction:** prev-morph dispatches double the morph dispatch count under the per-frame cap;
  budget accordingly (the cap counts cur+prev for skin already).
