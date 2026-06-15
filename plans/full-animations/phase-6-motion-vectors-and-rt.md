# Phase 6 — Motion vectors + RT BLAS for morphed geometry

**Status:** NOT STARTED

**Depends on:** Phase 5 (GPU morph deform stage: the `morph` compute pass, the `morphedBase` buffer,
`MorphDispatch`, the `Skinning` deform state extended with `morphedBaseBuffers`/`morphActiveBuffers`,
`DrawItem.morphWeights`/`morphMesh`, and the unskinned-morph-writes-`deformedBuffer`-directly contract).
Phase 5 keeps the single deform-state owner named `Skinning` and adds **no** prev-weights cache — this
phase introduces that cache itself.

## Why

Phase 5 deforms the *current* frame into the deformed buffer. Two consumers still see morph as if it
never moved:

- **TAA / motion vectors** read this frame's deformed position (binding 0) and **last frame's** deformed
  position (binding 1). For a skinned-morph mesh, binding 1 is the prev-deformed buffer the *prev-skin*
  dispatch writes — but that prev pose was skinned from the **rest** base, not the previous-frame morphed
  base, so the morph component of the velocity is missing. For an **unskinned-morph** mesh there is no
  prev dispatch at all yet, so binding 1 falls back to the static stream and morph velocity is zero. A
  blink/lip morph under a static camera then **ghosts** in TAA.
- **RT (shadows/GI/ReSTIR)** traces the BLAS. The skinned-morph BLAS already refits the post-skin
  deformed buffer (which is post-morph, since morph feeds skin), so skinned morph is covered for free.
  But **unskinned-morph** instances are not in `skinnedRtInstances` at all, so RT traces the rest
  silhouette, not the morphed one.

The fix carries morph through the **existing** motion + BLAS plumbing — no new motion shader, no new
BLAS code path. The motion pass (`motion.slang`) is unchanged; it already reads cur + prev deformed.
This phase only makes the prev-deformed buffer reflect the *previous frame's weights*, and makes the
unskinned-morph deformed buffer reachable by the TLAS at the right transform.

## Grounding (the existing motion + BLAS plumbing)

- **Motion pass** `renderer.cppm:1442-1447`: a graphics `RgPass` declaring `VertexInputRead` on both
  `deformedBuffer` and `prevDeformedBuffer`; `motion.slang` binding 0 = current deformed pos (location
  0), binding 1 = previous deformed pos (location 3). It reprojects `inst.model·curPos` vs
  `inst.prevModel·prevPos` (object + deformation motion together). **No shader change this phase.**
- **Prev-skin chain** `renderer.cppm:1264-1273`: the `skin` pass runs `skinDispatches` into
  `deformedBuffer`, then `prevSkinDispatches` into `prevDeformedBuffer`, using the previous joint
  palette. Both buffers declare `StorageWriteCompute` (`renderer.cppm:1235-1236`); the graph derives
  the compute-write→vertex-input/AS-build barriers from each consumer's read.
- **Prev caches** `renderer_types.cppm:1219-1222`: `Skinning.prevPaletteByEntity` (deformation motion)
  + `prevModelByEntity` (object motion). Committed **after** the prev values are read
  (`renderer_drawlist.cpp:878-888`); a new entity reads back `current == previous`, so frame 1 emits
  zero velocity (no flash). This is the exact pattern the morph prev-weights cache mirrors.
- **Dispatch lists** `renderer_types.cppm:653-656`: `SceneDrawList.skinDispatches` /
  `prevSkinDispatches` / `skinnedRtInstances`. Phase 5 added `morphDispatches` / `prevMorphDispatches`
  to the same struct (the prev morph list currently unfilled — this phase fills it).
- **BLAS refit** `renderer_detail.cppm:572-720` `recordSkinnedBlasBuilds`: builds one per-entity refit
  BLAS over each `SkinnedRtInstance`'s slice of `deformedBuffers[frame]` (vertexData =
  `deformedBase + deformedOffset*sizeof(Vertex)`), MODE_BUILD the first frame (`PREFER_FAST_TRACE` +
  `ALLOW_UPDATE`), MODE_UPDATE every later frame (`slot.built`). Already reads the post-morph+post-skin
  buffer for skinned instances — no change there.
- **TLAS transform** `renderer.cppm:2917-2934` `buildTlas`: static instances transpose the per-instance
  world matrix `models[i]` into the row-major `VkTransformMatrixKHR` (`:2900-2908`); **skinned**
  `SkinnedRtInstance`s use a **hardcoded identity** transform because the skin kernel bakes
  `worldBone·inverseBind` (no model matrix) so the deformed verts are already world space. **This
  identity assumption is wrong for unskinned-morph** — its deformed buffer is *object* space, so its
  TLAS instance must use the node world matrix.
- **`SkinnedRtInstance`** `renderer_types.cppm:631-641`: `entity`, `deformedOffset`, `vertexCount`,
  `indexCount`, `mesh`. No transform field today (identity assumed in `buildTlas`). This phase adds one.

## Decisions (locked)

1. **`prevMorphDispatches` parallels `prevSkinDispatches` and deforms the previous frame's weights.**
   The morph stage runs twice per *changing* morphed instance: current weights → `morphedBase` (or
   `deformedBuffer` for unskinned), previous weights → `prevMorphedBase` (skinned) or directly into
   `prevDeformedBuffers` (unskinned). The full prev deform chain by mesh kind:
   - **skinned-morph:** `prevMorph` (prev weights) → `prevMorphedBase` → `prevSkin` (prev palette) →
     `prevDeformedBuffers`. So `prevMorphedBase` becomes the **prev-skin pass's input stream**, exactly
     as `morphedBase` is the current-skin input. The prev-skin dispatch's `inVertices` is chosen
     host-side to be `prevMorphedBase` for morphed instances (mirrors Phase 5's current-skin
     `inVertices` = `morphedBase` choice in `wireSet`). The motion pass then reads `prevDeformedBuffer`
     as binding 1 — unchanged.
   - **unskinned-morph:** `prevMorph` (prev weights) → `prevDeformedBuffers` directly (no prev-skin
     step, `jointCount == 0`). The motion pass reads `prevDeformedBuffer` as binding 1 — unchanged.

2. **Change-gate the prev-morph dispatch (the free win).** If an instance's `prevWeights ==
   curWeights` (the `prevMorphWeightsByEntity` cache this phase adds in step 3 holds last frame's resolved weights),
   **skip** the prev-morph dispatch and point binding 1 at the *current* deformed/morphedBase slice
   instead (prev deform is identical). This saves a full deform pass for every static-weight instance —
   the common case, since most morphed meshes hold a pose for many frames. For the skinned-morph case:
   if weights match **and** the palette also matches, the whole prev chain (prev-morph + prev-skin)
   collapses to "read current"; if weights match but the palette moved, skip only prev-morph and feed
   prev-skin from the current `morphedBase` (the bone moved, the shape did not).

3. **`prevMorphWeightsByEntity` mirrors `prevPaletteByEntity`.** Add it to the `Skinning` deform
   state as the cross-frame committed copy of resolved weights. Phase 5 extended `Skinning` with
   `morphedBaseBuffers`/`morphActiveBuffers` but no prev-weights cache, so this is the only one. Keyed by
   entity uuid. Committed **after** this frame's prev values are read (alongside the
   `prevPaletteByEntity` commit at `renderer_drawlist.cpp:878-888`). A just-spawned entity finds no row
   → reads back `current == previous` → **zero frame-1 morph motion** (no flash), identical to the
   skin/object caches.

4. **The motion pass is unchanged.** It reads cur + prev deformed (binding 0/1). All this phase does is
   make binding 1 carry the previous weights. No edit to `motion.slang` or the motion `RgPass`
   (`renderer.cppm:1442-1447`). The new prev-morph dispatch writes into the same `prevDeformedBuffers`
   (unskinned) / `prevMorphedBase`→`prevSkin`→`prevDeformedBuffers` (skinned) the existing plumbing
   already wires.

5. **RT skinned-morph: no change.** `recordSkinnedBlasBuilds` already refits the deformed buffer, which
   is post-morph for skinned-morph meshes (morph → morphedBase → skin → deformed). A skinned-morph
   instance is already a `SkinnedRtInstance`. Nothing to do.

6. **RT unskinned-morph: include it in `skinnedRtInstances`, with a real world transform.** Gate =
   *deforms this frame* (the current morph dispatch ran, i.e. any non-zero weight). Its deformed slice
   lives in `deformedBuffers[frame]` at its `deformedOffset` (Phase 5 routes the unskinned-morph deform
   there), so `recordSkinnedBlasBuilds` refits it with **no change**. The **TLAS transform** is the
   only difference: unskinned-morph deformed verts are **object space**, so the TLAS instance must use
   the **node world matrix**, not identity. Add a `worldTransform` field to `SkinnedRtInstance` (default
   identity for skinned) and have `buildTlas` use it. Recording this explicitly is mandatory — without
   it morphed RT geometry lands at the origin.

7. **Refit + representative-pose initial build.** Topology is fixed (morph changes only
   positions/normals, never triangle/index counts), so the existing MODE_BUILD-once-then-MODE_UPDATE
   refit policy holds unchanged. **The initial MODE_BUILD must run over a representative resolved-weight
   pose, not the zero-weight base.** Because `recordSkinnedBlasBuilds` runs in the same cmd *after* the
   morph/skin compute (graph-ordered via `AccelStructBuildRead` on `deformedBuffer`,
   `renderer.cppm:1878`), the first refit already sees a resolved pose — no special-casing beyond
   ensuring the unskinned-morph instance is added only on a frame where its dispatch ran (decision 6's
   "deforms this frame" gate guarantees this). A **periodic full MODE_BUILD rebuild cadence** (refit
   drift over a wide weight range degrades traversal quality) is a **documented knob, scheduler
   deferred** to Phase 9 — leave a TODO, do not build a scheduler here.

## Ordered steps

### Backend — state (`renderer_types.cppm`)

1. **`Skinning` prev-weights cache:** add `std::unordered_map<u64, std::vector<float>>
   prevMorphWeightsByEntity;` to the `Skinning` deform state (Phase 5 added no prev-weights cache, so this
   phase introduces the only one). Doc-comment it like `prevPaletteByEntity`
   (`renderer_types.cppm:1219-1221`): a new entity reads back current == previous, so its first frame
   emits zero morph motion.
2. **`SkinnedRtInstance` world transform:** add `glm::mat4 worldTransform{ 1.0f };` with a `///` note:
   identity for skinned (deformed verts are world space), the **node world matrix** for an
   unskinned-morph instance (deformed verts are object space). The identity default keeps the skinned
   path untouched.
3. **`SceneDrawList`:** confirm `prevMorphDispatches` exists from Phase 5 (`morphDispatches` /
   `prevMorphDispatches` were added there). This phase populates the prev list. If the prev morphedBase
   buffer is not yet declared, add `prevMorphedBaseBuffers` + capacity to `Skinning`, mirroring
   `prevDeformedBuffers` / `prevDeformedCapacity` (`renderer_types.cppm:1215-1216`).

### Backend — host bucketing (`renderer_drawlist.cpp`, `submitDrawList`)

4. **Resolve previous weights per morphed instance.** Where Phase 5 reads `DrawItem.morphWeights` to
   build the current `MorphDispatch`, look up `prevMorphWeightsByEntity[entity]`; on miss, set
   `prevWeights = curWeights` (frame-1 zero motion). Compute `weightsChanged = (prevWeights !=
   curWeights)`.
5. **Build `prevMorphDispatches` (change-gated).** When `weightsChanged`:
   - allocate a prev active-morph slice in the Phase 5 per-frame host-mapped morph ring from
     `prevWeights`, and wire a prev `MorphDispatch` whose output is the **prev** morphedBase slice
     (skinned) or the **prev-deformed** slice (unskinned) at the same per-instance offset the current
     dispatch uses; push it to `list.prevMorphDispatches`.
   - When **not** changed: push nothing; the prev consumer (prev-skin input, or motion binding 1) reads
     the **current** slice. Implement "prev == cur" the same way the skin path does (the existing
     prev-deformed plumbing already supports prev == cur); mirror that mechanism — do not invent a
     second one.
6. **Skinned-morph prev-skin input selection.** Where Phase 5's `wireSet` chose the current-skin
   `inVertices` = `morphedBase` for morphed instances, choose the **prev-skin** `inVertices` =
   `prevMorphedBase` for morphed instances whose prev-morph dispatch ran this frame; else `morphedBase`
   (weights unchanged, only the bone moved). Non-morph skinned instances keep the rest base for
   prev-skin, unchanged.
7. **Grow the prev morphedBase buffer.** Add `ensurePrevMorphedBaseCapacity` (grow-only,
   per-frame-pinned, mirroring `ensurePrevDeformedCapacity`). Unskinned morph reuses the existing
   `prevDeformedBuffers` — no new buffer for it.
8. **Include unskinned-morph in `skinnedRtInstances` (RT only).** When RT consumes the scene and an
   unskinned-morph instance **deforms this frame** (its current morph dispatch ran), push a
   `SkinnedRtInstance` with `entity`, its `deformedOffset` (into `deformedBuffers[frame]`),
   `vertexCount`, `indexCount`, `mesh`, and `worldTransform = the node world matrix` (the same matrix
   `renderScene` puts in the instance buffer for this entity — read it from the `DrawItem` / world
   transform already available). Skinned instances keep `worldTransform = identity` (default).
9. **Commit the prev-weights cache.** After all reads (alongside the `prevPaletteByEntity` /
   `prevModelByEntity` commit at `renderer_drawlist.cpp:878-888`), write
   the `Skinning` state's `prevMorphWeightsByEntity[entity] = curWeights` for every morphed instance seen
   this frame.

### Backend — render graph (`renderer.cppm`)

10. **Prev-morph dispatch execution + barriers.** In the `morph` `RgPass` execute (Phase 5), after
    running `morphDispatches`, run `prevMorphDispatches` (mirrors the skin pass running
    `prevSkinDispatches` after `skinDispatches`, `renderer.cppm:1266-1273`). Declare
    `StorageWriteCompute` on the **prev** morphedBase buffer (skinned) and on `prevDeformedBuffer`
    (unskinned) so the graph derives the prev-morph→prev-skin and prev-morph→motion barriers. The
    prev-skin pass must add the matching `StorageReadCompute` on `prevMorphedBase` (extend Phase 5's
    explicit morphedBase read to also cover prevMorphedBase when prev-skin reads it) — otherwise the
    prev-morph-write→prev-skin-read barrier never derives, the same silent-failure seam Phase 5
    documents, applied to the prev chain. **No hand-written barrier.**
11. **TLAS transform for unskinned-morph.** In `buildTlas` (`renderer.cppm:2917-2934`), replace the
    hardcoded identity in the skinned-instance loop with `s.worldTransform` transposed into
    `VkTransformMatrixKHR` (reuse the column→row transpose the static loop at `:2900-2908` does).
    Skinned instances still pass identity (the default), so their behavior is unchanged;
    unskinned-morph instances now land at their node world matrix. Update the loop's `///` comment:
    skinned = identity (verts are world space); unskinned-morph = node world matrix (verts are object
    space).
12. **No BLAS code change.** `recordSkinnedBlasBuilds` is untouched — it refits whatever
    `deformedOffset` slice each `SkinnedRtInstance` names, which is now also the unskinned-morph slice.
    Add a `// TODO(perf): periodic full MODE_BUILD rebuild cadence for wide weight ranges (Phase 9)`
    note near the refit policy comment (`renderer_detail.cppm:566-571`) so the Phase 9 budget has a
    hook — do not implement a scheduler.

### Verification of barrier derivation

13. Confirm every new edge is graph-derived (no hand-written barrier added outside the accepted AS-pass
    exception in `recordSkinnedBlasBuilds` / `recordTlasBuild`):
    - prev-morph write (`StorageWriteCompute`) → prev-skin read (`StorageReadCompute` on
      `prevMorphedBase`) — derived.
    - prev-morph/prev-skin write (`StorageWriteCompute` on `prevDeformedBuffer`) → motion
      vertex-input read (`VertexInputRead`, `renderer.cppm:1447`) — already declared, now fed by
      prev-morph.
    - morph/skin write on `deformedBuffer` (`StorageWriteCompute`) → AS-build read
      (`AccelStructBuildRead`, `renderer.cppm:1878`) — already declared; the unskinned-morph instance
      rides the same buffer, so its BLAS read is already barriered.

## Frontend

None. No editor, Timeline, Clips, or Inspector change — this phase is renderer-internal motion/RT
plumbing. The weights it reprojects are already driven by the Phase 4 runtime and surfaced by Phase 7's
commands + Phase 8's UI.

## Control commands

None this phase. No new drivable/inspectable state — motion vectors and BLAS are derived from the
already-driven morph weights.

## Performance

- **Change-gated prev-morph saves a full deform pass for static-weight instances.** Most morphed meshes
  hold a pose across many frames, so the prev dispatch is skipped and binding 1 reads the current slice
  — the common case costs nothing extra.
- **Prev-morph doubles the morph dispatch count** for instances whose weights *do* change, under the
  same per-frame morph descriptor-set cap. Reconcile against the Phase 5 pool sizing
  (`SkinMaxSetsPerFrame = 64`, `renderer_types.cppm:78`; the morph/skin descriptor pool,
  `renderer_detail.cppm:~3142`): a changing skinned-morph instance now consumes cur+prev morph sets
  **and** cur+prev skin sets. Either grow the pool sizing or document the reduced effective instance cap
  for changing-weight scenes; the Phase 9 per-frame deformation budget reconciles the final numbers.
- **VRAM:** a skinned-morph mesh now holds cur/prev × morphedBase/deformed (~4× a static mesh's deform
  memory). Budget it explicitly in Phase 9. An unskinned-morph mesh holds cur/prev deformed (2×).
- **BLAS refit cost** for unskinned-morph adds one MODE_UPDATE per deforming unskinned-morph instance
  per frame, gated to frames where the dispatch ran; the periodic-rebuild knob is Phase 9.

## Docs

None new in this phase. The motion/BLAS handling for morph is covered by `morph-targets.md` in **Phase
9** (`docs/content/explanations/animation/`). When writing that page, document: the change-gated
prev-morph dispatch, frame-1-zero-motion via `prevMorphWeightsByEntity`, the skinned (identity) vs
unskinned-morph (node world matrix) TLAS transform split, the representative-pose initial build, and the
deferred periodic-rebuild cadence knob. Do not add a page in this phase.

## Tests (`tests/e2e`, bun over the control plane)

The assertions this phase must satisfy (land them with the Phase 9 morph fixtures; drive playback via
the existing play/seek commands from Phase 4/7):

1. **Morph-only TAA (no ghosting).** Spawn an **unskinned** morph mesh (`AnimatedMorphCube`), play a
   clip animating a weight, hold the camera static, let TAA accumulate, then assert no ghosting (the
   prev-deformed buffer reflects the previous frame's weights). Use whatever TAA/ghosting probe the e2e
   harness already has (screenshot diff against a reference, or a control-exposed motion stat).
2. **Frame-1 zero morph motion.** Just-spawned at a non-zero weight, first frame: assert the morph
   motion contribution is zero (no velocity flash) — the prev cache read back current == previous.
3. **Prev-morph skip on unchanged weights.** Hold a weight constant across frames; assert the
   prev-morph dispatch is skipped (steady-state cost equals the cur-only cost) via the GPU profiler
   mode (`profiler.set-mode`) or a dispatch-count stat if exposed.
4. **RT morphed silhouette.** Enable RT (shadows/GI); with a morph weight driven, assert shadows/GI
   trace the morphed silhouette (not the rest pose) for both a skinned-morph and an unskinned-morph
   fixture. Verify the unskinned-morph case lands at the node world position, not the origin — a morphed
   cube offset from origin casts its shadow under the cube, not at world zero (the load-bearing
   transform check).
5. **Validation-clean log.** The e2e gate asserts no Vulkan sync validation errors: morph→skin→motion
   and morph→AS-build barriers all graph-derived. A missing prev-morph→prev-skin read access surfaces
   here as a sync error (the documented silent-failure seam).

Also extend the headless present-only smoke (`make engine`) so an `AnimatedMorphCube` under TAA + RT
runs clean for `SAFFRON_EXIT_AFTER_FRAMES=N` without validation errors.

## Acceptance criteria

- Morph-only motion produces correct motion vectors — no TAA ghosting on a blink/lip/shape morph under a
  static camera (prev-deformed reflects prev weights for both skinned-morph and unskinned-morph).
- A just-spawned entity has **zero frame-1 morph motion** (`prevMorphWeightsByEntity` reads back current
  == previous).
- The prev-morph dispatch is **skipped when weights are unchanged** (binding 1 reads the current slice;
  no extra deform cost for static-weight instances).
- RT BLAS refits the **post-morph** buffer; skinned-morph rides the existing skinned path unchanged; the
  **unskinned-morph TLAS transform is the node world matrix** (morphed RT geometry lands at its world
  position, not the origin); the initial BLAS build uses a **representative resolved-weight pose**.
- All barriers are **graph-derived** (validation-clean log): morph→skin→motion and morph→AS-build, plus
  the new prev-morph→prev-skin read access; no new hand-written barriers outside the accepted AS-pass
  exception.
- `make engine` + `make prepare-for-commit` clean (format + lint, no new warnings).

## Risks / notes

- **The unskinned-morph TLAS transform is the load-bearing correctness point.** Leaving identity
  silently puts morphed RT geometry at the origin — the most likely defect. The e2e off-origin shadow
  assert (test 4) is the guard.
- **One mechanism for "prev == cur".** The skin/object caches already implement "new entity reads back
  current"; the prev-morph skip and the unskinned "binding 1 = current slice" must reuse that same
  convention, not invent a parallel one (NO LEGACY: one code path).
- **Pool/budget interaction with Phase 5 is real** — a changing skinned-morph instance is now 4 deform
  sets/frame. Decide pool sizing here and hand the final budget to Phase 9; do not leave the cap
  silently halved.
- **Periodic BLAS rebuild is deferred, not skipped** — leave the documented knob + TODO so Phase 9's
  scheduler has a hook. Refit-only is correct (topology is fixed) but can drift in quality over a wide
  weight sweep.
