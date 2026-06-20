# Phase 9 — Docs + e2e fixtures + perf budget + milestone gate

**Status:** NOT STARTED

**Depends on:** Phases 2, 3, 4, 5, 6, 7, 8 — the concepts (import, storage, runtime, GPU deform,
motion/RT, control, frontend) must all exist before they can be documented, pinned by e2e, and gated.

## Why

Morph targets and node-TRS animation are new engine concepts and new drivable state. AGENTS.md
keep-current makes docs part of "done": every concept that landed across phases 2–8 needs its
explanation page, and the format pages (`.smesh`, `.sanim`, glTF import) must describe the *one*
current format, not a change journey. The behavior must be pinned by the bun e2e harness over the
control plane (the language-appropriate place for engine behavior tests). This phase also lands the
performance hardening that gives the AAA-scaling story — a per-frame deformation budget, a
distant-instance skip, a named morph profiler scope, and the documented two-pass scale-up — then runs
the full milestone gate and closes the plan.

No new control commands ship here (they shipped in Phase 7); the e2e suite asserts their wire behavior.
No new frontend ships here (it shipped in Phase 8); the docs describe it.

## Grounding (real files / symbols)

- Animation hub: `docs/content/explanations/animation/_index.md` — current rows: `animation-data-model`,
  `playback-runtime`, `skeleton-overlay`, `timeline`, `foot-ik-and-physics-ahead`. Intro currently says
  "Skeletal animation deforms a rigged mesh…"; that must be widened (animation is no longer
  skeleton-only).
- Format docs: `docs/content/explanations/geometry-and-assets/smesh-format.md` (describes v1/v2 today,
  `MeshFormatVersion`/`MeshFormatVersionSkinned`), `.../sanim-format.md` (`SANimHeader`,
  `SANimTrackRecord`, `AnimFormatVersion`), `.../gltf-and-obj-import.md` (the import pipeline page).
- Concept docs to update: `.../animation/animation-data-model.md` (AnimTrack/AnimClip/JointPose/
  PoseBuffer/sampleTrack/sampleClip), `.../animation/playback-runtime.md` (the tickAnimation write seam),
  `.../animation/timeline.md` (Timeline panel).
- Page style (Diátaxis, hugo-book): one concept/page, TOML front matter, `title` == body `# H1`, slim
  `What | File | Symbols` table (symbols, not line numbers), plain voice (run the `humanizer` pass),
  KaTeX `$…$` for math, GitHub-alert callouts. Start a new page from an archetype.
- e2e: `tests/e2e` — bun tests that boot a headless engine via `harness.ts` (`Engine.boot`,
  `engine.call`, `engine.importEntity`, `engine.rig`, `engine.settle`, `engine.validationErrors`,
  `engine.shutdown`). The binary is `build/debug/bin/SaffronAnima` (override `SAFFRON_ANIMA_BIN`); each
  test boots its own headless weston. Look at `animation-playback.test.ts`, `skinning.test.ts`,
  `skinned-motion.test.ts` for the established style: assert over the wire (transforms, weights,
  counts) and a validation-clean log, not pixels (the harness runs on llvmpipe).
- Existing e2e fixtures: `tests/e2e/fixtures/` (`skinned-strip.gltf`, `leg.gltf`, `two-materials.gltf`,
  `mapped-material.glb`) plus engine-side `engine/assets/models/animated-strip.gltf`. New morph/node
  fixtures go in `tests/e2e/fixtures/`.
- Gate: `tools/ci/check.sh` (engine build → present-only smoke → control-DTO contract test → script-def
  drift → project smoke → frontend `bun run build` + `bun test`). `make check` wraps it; `make e2e` runs
  the `tests/e2e` suite; `make prepare-for-commit` = format + lint. Contract test lives in
  `tools/check-control-schema/check.ts`.
- Profiler scope API: `render_graph.cppm` `GpuScope` (RAII timestamp scope inside a pass body) +
  `RgTimestamps`; profiler mode is driven from the control plane (`profiler.set-mode`,
  `renderer_types.cppm` profiler-mode enum). A named scope inside the morph pass body is the inspectable
  cost.
- Perf seams: `renderer_detail.cppm` `recordSkinnedBlasBuilds` + the `SkinMaxSetsPerFrame=64` descriptor
  pool; `renderer_drawlist.cpp` `submitDrawList` (host-side bucketing, dispatch list build);
  `renderer_types.cppm` `SkinDispatch`/`Skinning`/`Rt`. The morph dispatch list (Phase 5) and the
  prev-morph dispatch (Phase 6) are where the budget + distant skip clamp.

## Decisions (locked)

1. **Two new docs pages, both with a `What | File | Symbols` table and a hub row.**
   - `docs/content/explanations/animation/morph-targets.md` — the blend-shape concept end to end:
     - the **sparse delta model** (`MorphDelta { u32 vertexIndex; vec3 dPosition; vec3 dNormal }`,
       28 B, NORMAL-only, tangent re-derived by Gram-Schmidt at deform time — say *why* there is no
       `dTangent`: the engine `Vertex` carries no tangent stream);
     - the **morph-before-skin order**, enforced structurally (the morph stage writes `morphedBase`,
       which IS the skin pass's input binding — skin-first is physically impossible), and the unskinned
       case (morph writes the deformed buffer directly, drawn as a static stream);
     - the **GPU stage** (`morph.slang` compute, the explicit `StorageReadCompute` the skin pass
       declares on `morphedBase` so the graph derives the morph→skin barrier — the seam where "the graph
       derives it" would otherwise silently fail because the static input previously needed no access);
     - **weights resolution** order (`node.weights` → `mesh.weights` → zeros at spawn; the animated
       override on top);
     - the **`MorphComponent` / `MorphWeightOverrideComponent` pair** (durable vs runtime-only; the
       override is the non-destructive Edit-preview layer, removed on stop, never serialized);
     - **motion + BLAS** (prev-weights cache + change-gated prev dispatch so morph-only motion does not
       ghost under TAA; BLAS refit on the post-morph buffer, topology fixed, built in a representative
       resolved-weight pose);
     - the **`set-morph-weights` / `get-morph-weights` command** and the live `morphWeights` in
       `AnimationStateResult`;
     - a short **two-pass scale-up** note (see decision 4): the UE integer-fixed-point InterlockedAdd
       scatter+normalize is the documented graduation for dense facial rigs, kept out of v1.
     - `What|File|Symbols`: `geometry.cppm` (`MorphDelta`, `MorphTarget`, `.smesh` morph section);
       `morph.slang` (`computeMain`); `renderer.cppm` / `render_graph.cppm` (morph pass,
       `StorageReadCompute`); `scene.cppm` (`MorphComponent`, `MorphWeightOverrideComponent`);
       `animation.cpp` (`tickAnimation` weights write); `control_commands_animation.cpp`
       (`set-morph-weights`).
   - `docs/content/explanations/animation/node-trs-animation.md` — animating plain entity transforms:
     - the **lifted gate** (the skin-only animation gate at `geometry.cppm` is lifted; one decode path
       produces bone, node, and weights tracks — name the deleted skip/flatten paths, NO LEGACY);
     - the **Bone / Node / Weights track model** (`AnimTrack.Target`, `Path::Weights`, `targetName`
       subsuming the old `jointName`);
     - **name → Uuid binding** resolved once and cached, re-resolved on miss (UE FGuid + Unity
       name-path hybrid: a reparent keeps the Uuid, a rename re-resolves by name);
     - **`PoseOverrideComponent` reuse** — `localMatrix` already prefers `PoseOverrideComponent` for any
       entity, so a driven node is just a node with an override and composes through
       `updateWorldTransforms` with zero new compose code;
     - the **container player** attach (a player on a non-rigged animated subtree drives nodes through
       the single play/seek/loop/state path) and `list-clip-bindings` for resolved/unresolved
       inspection.
     - `What|File|Symbols`: `geometry.cppm` (`AnimTrack.Target`, `targetName`); `animation.cpp`
       (`sampleClipResolved`, `tickAnimation` node write); `scene.cppm` (`localMatrix`,
       `updateWorldTransforms`, `PoseOverrideComponent`); `assets.cppm` (per-node spawn, container
       player); `control_commands_animation.cpp` (`list-clip-bindings`).
   - Add both rows to the `animation/_index.md` table and **rewrite the intro** so it leads with
     "animation drives a transform source over time" — skeleton, plain node, and morph weights are three
     sources into the one sample → pose/override → compose seam — not "skeletal animation deforms a
     rigged mesh".

2. **Update existing pages (concept changed ⇒ page changes, in this same body of work).**
   - `animation-data-model.md`: the generalized `AnimTrack` (`Target { Bone, Node }`, `Path::Weights`,
     `targetName`, `morphCount`), the N-wide weights stream layout (per-keyframe block of N scalars;
     CUBICSPLINE = `3·N` laid `[in[N], value[N], out[N]]`, tangents scaled by `deltaT`), and that one
     `sampleClip` drives bones, nodes, and weights — only the write target differs.
   - `playback-runtime.md`: the one `tickAnimation` write seam now writes three payloads — bone
     `PoseOverrideComponent`, node `PoseOverrideComponent`, mesh `MorphWeightOverrideComponent` — all
     cleared on stop (non-destructive in Edit and Play by construction).
   - `timeline.md`: one clip = one bar; channels are nested drill-down metadata (not multiplied track
     rows); the Inspector morph sliders are 0..1 scalar weight rows bound by name; the rig gate widened
     to `AnimationPlayer || SkinnedMesh || Morph`. Describe the existing model extended, never a parallel
     UI.
   - `smesh-format.md`: **one** `MeshFormatVersion` + a `flags` word (skin bit, morph bit) + a sparse
     morph section. Delete the v1/v2 dual-version description entirely (NO LEGACY applies to docs voice:
     describe the current single format, with no "v2 was…" note). Update the header struct snippet, the
     defensive-load block (now also validates the morph section bounds), and the version constant in the
     `What|File|Symbols` table.
   - `sanim-format.md`: `AnimFormatVersion` v2 — the per-track `Target` kind (Bone/Node), `morphCount`,
     and `targetName` (replacing `jointName`). Describe the replaced reader (old v1 rejected), not a
     migration.
   - `gltf-and-obj-import.md`: the gate lift (node-forest + animation decode run unconditionally; only
     the skin payload stays gated), the sparse-accessor decode via `cgltf_accessor_unpack_floats` (with
     the note that `cgltf_accessor_read_float` returns 0 on sparse — the load-bearing primitive choice),
     and the new node + weights channels routed by path.

3. **e2e fixtures + assertions** (assert over the wire, validation-clean logs).
   - `tests/e2e/morph_weights.test.ts` — import an **AnimatedMorphCube**-equivalent (2 morph targets,
     CUBICSPLINE weights animation):
     - assert the imported mesh reports `morphCount == 2` and the weights track output size is `3·N·M`
       (CUBICSPLINE; via `get-asset-model` / `list-clip-bindings` channel metadata, or `inspect`);
     - `set-morph-weights` then `get-morph-weights` round-trip (e.g. set target 0 to 0.5, read it back);
     - `play` / `play-animation`, poll `get-animation-state` and assert `morphWeights` change
       frame-over-frame (gated by `animationVersion` like the existing poll);
     - assert the morphed world-space bounds change frame-over-frame (read bounds over the wire, e.g. via
       `inspect`/a bounds query), proving the GPU deform actually moved geometry;
     - assert `engine.validationErrors()` is empty.
   - `tests/e2e/morph_sparse.test.ts` — import a **MorphStressTest / MorphPrimitivesTest**-equivalent
     (sparse deltas, multi-primitive):
     - assert sparse deltas decode (non-zero per-target delta counts reported through the model/clip
       metadata) — proves `unpack_floats` resolved the sparse overlay rather than producing all-zero
       deltas;
     - assert a multi-primitive **target-count mismatch is rejected** with an `ok:false` error from
       `import-model` (the importer validates all of a node's primitives agree on target count);
     - validation-clean log.
   - `tests/e2e/node_trs.test.ts` — import a **BoxAnimated**-equivalent (2 nodes, parent→child, node-TRS,
     no skin):
     - spawn, attach/locate the container `AnimationPlayer`, `play`, poll over frames and assert **both
       nodes move** and the **child composes through the parent** (read world transforms over the wire;
       the child's world reflects the parent's animated local);
     - `list-clip-bindings` reports the node tracks **resolved** (status per channel, name → Uuid).
   - **Bundle minimal Khronos fixtures** under `tests/e2e/fixtures/`: keep them tiny (headless boot is
     repeated per test). Prefer trimmed/hand-authored `.gltf` equivalents over the full Khronos GLBs to
     keep the repo lean and the boot fast (the existing fixtures `skinned-strip.gltf`/`leg.gltf` were
     authored this way; `gen_leg.py` shows the pattern). If using Khronos CC-BY assets verbatim, vendor
     only the minimal files needed and keep attribution.

4. **Performance hardening (lands here).**
   - **Per-frame deformation budget.** In the host-side dispatch build (`renderer_drawlist.cpp`
     `submitDrawList`), cap the work scheduled per frame by two counters: total **vertices deformed**
     (morph + skin) and total **BLAS primitives updated** (`recordSkinnedBlasBuilds`). Past the cap,
     defer the lowest-priority instances to a later frame (round-robin so none starves), priority by
     screen size / distance. The budget is a named constant (sibling to `SkinMaxSetsPerFrame`), tunable
     and documented.
   - **Distant / low-LOD instance skip.** Skip the morph (and skin) dispatch for instances below a
     distance/screen-coverage threshold, leaving them on the last deformed buffer (or the static bind
     pose for far morph meshes). This is the AAA-scaling lever: a crowd of facial rigs only pays for the
     near ones.
   - **Reconcile the morph dispatch with `SkinMaxSetsPerFrame=64` + its descriptor pool**
     (`renderer_detail.cppm`). Decide **own-pool vs shared**: the morph pass binds the shared read-only
     delta bank + a per-instance weights/output descriptor, so its set count is independent of the skin
     pool. Document the decision (recommend a dedicated `MorphMaxSetsPerFrame` pool so a heavy
     morph-mesh frame cannot starve skinning, or grow the shared pool and split the budget) and make the
     pool size a named constant matching the budget.
   - **Named morph GPU profiler scope.** Wrap the morph compute pass body in a `GpuScope(timestamps,
     cmd, "morph")` (the `render_graph.cppm` RAII scope) so its cost shows up live under
     `profiler.set-mode` alongside `skin`, `motion`, etc. — no new command, it rides the existing
     profiler readout.
   - **Two-pass scale-up — documented, not built.** Document (in `morph-targets.md` and as a note in
     the README) the UE integer-fixed-point two-pass `InterlockedAdd` scatter+normalize as the
     graduation for dense facial rigs: an i24 fixed-point scale derived from import-time per-morph bounds
     × current weights, scattered with `InterlockedAdd`, then a normalize pass; the graph derives the
     inter-pass `StorageWrite → StorageRead` barrier. Frame it as a one-extension scale-up behind a
     profiling trigger, **not** a rewrite of the v1 single-pass float accumulation. Do **not** implement
     it in v1 (keep one code path).

5. **Plan bookkeeping.** Mark **each** phase file `**Status:** COMPLETED` as it lands, and mark the
   `plans/full-animations/README.md` `**Status:** COMPLETED` once all phases ship and this gate is
   green. Per the AGENTS.md plans rule, a phase file may be deleted only *after* it is COMPLETED —
   prefer leaving the COMPLETED files in place as the record unless asked to prune.

## Ordered steps

1. **Verify prerequisites.** Confirm phases 2–8 are in (gate lift + sparse decode, morph storage +
   `.smesh` flags, node-TRS runtime, GPU morph stage, motion/RT, control commands `set/get-morph-weights`
   + `list-clip-bindings` + `morphWeights` in `AnimationStateResult`, frontend Timeline/Inspector). The
   docs and tests describe what exists; do not document or test ahead of the code.

2. **Build the fixtures.** Author/trim and add to `tests/e2e/fixtures/`:
   `morph-cube.gltf` (2 targets, CUBICSPLINE weights animation, looping), `morph-sparse.gltf` (sparse
   deltas + a multi-primitive variant or a separate `morph-mismatch.gltf` whose primitives disagree on
   target count), `box-animated.gltf` (2 nodes parent→child, node-TRS, no skin). Keep each under a few
   KB. If a Python generator helps (cf. `fixtures/gen_leg.py`), add it beside the fixture.

3. **Write `tests/e2e/morph_weights.test.ts`.** Boot, import the morph-cube fixture, resolve the morph
   mesh entity (extend `engine.rig`-style lookup to find the `Morph` component), assert `morphCount==2`
   and the `3·N·M` weights size, `set-morph-weights`/`get-morph-weights` round-trip, `play` + poll
   `get-animation-state` for changing `morphWeights`, assert bounds change frame-over-frame, assert a
   clean validation log. Mirror the structure of `animation-playback.test.ts`.

4. **Write `tests/e2e/morph_sparse.test.ts`.** Import the sparse fixture (assert non-zero decoded delta
   counts via model/clip metadata), then import the mismatch fixture and assert `import-model` rejects it
   (`expect(engine.call(...)).rejects`). Clean validation log.

5. **Write `tests/e2e/node_trs.test.ts`.** Import + spawn the box-animated fixture, locate the container
   player, `play`, poll world transforms of both nodes over several frames, assert both move and the
   child composes through the parent, assert `list-clip-bindings` reports the node tracks resolved with a
   name → Uuid. Clean validation log.

6. **Run `make e2e`** (private `build/<name>` if another agent is building). Fix until the three new
   tests and the existing suite pass with empty validation logs.

7. **Performance: deformation budget + distant skip.** In `renderer_drawlist.cpp` `submitDrawList`, add
   the vertex + BLAS-prim budget counters and the distance/screen-coverage skip on the morph and skin
   dispatch lists; round-robin deferral so no instance starves. Add the named budget + pool constants
   (sibling to `SkinMaxSetsPerFrame`). Reconcile the morph descriptor pool (own vs shared) in
   `renderer_detail.cppm` and document the decision in `morph-targets.md`.

8. **Performance: named morph profiler scope.** Add `GpuScope(timestamps, cmd, "morph")` to the morph
   compute pass body so the cost surfaces under the existing profiler readout. Confirm it appears via a
   `profiler.set-mode` round-trip (extend `profiler.test.ts` or assert in `morph_weights.test.ts`).

9. **Write the new docs pages.** `morph-targets.md` and `node-trs-animation.md` per decisions 1 and 4
   (each: TOML front matter, title == H1, lead with the concept + why, `What | File | Symbols` table,
   the two-pass scale-up note in `morph-targets.md`). Run each through the `humanizer` pass.

10. **Add the hub rows + rewrite the intro** in `animation/_index.md` (decision 1).

11. **Update the existing pages** per decision 2: `animation-data-model.md`, `playback-runtime.md`,
    `timeline.md`, `smesh-format.md`, `sanim-format.md`, `gltf-and-obj-import.md`. Delete the v1/v2
    dual-version description in `smesh-format.md`; describe the single current format.

12. **Build + link-check the docs.** `git submodule update --init --depth 1 docs/themes/hugo-book` if
    needed, then `cd docs && hugo` (extended) builds clean; verify the two new pages render, the hub
    rows link, and no internal links 404.

13. **Run the full milestone gate.** `make check` (engine build → present-only smoke → control-schema
    contract → script-def drift → project smoke → frontend build + unit tests), `make e2e`,
    `make prepare-for-commit` (clang-format + clang-tidy across C++, oxfmt + oxlint across editor TS).
    Fix every warning this change raises. The contract test must stay green (no DTO drift; the
    `bun run check` git-diff-clean check on the generated files passes).

14. **Plan bookkeeping.** Flip each phase file's `**Status:**` to `COMPLETED` and the README to
    `COMPLETED` (decision 5).

## Backend changes (this phase)

- `renderer_drawlist.cpp` `submitDrawList`: per-frame deformation budget (vertices deformed + BLAS prims
  updated) with round-robin deferral; distance/screen-coverage instance skip on the morph + skin
  dispatch lists.
- `renderer_detail.cppm`: morph descriptor-pool decision (own `MorphMaxSetsPerFrame` pool vs shared with
  `SkinMaxSetsPerFrame=64`), reconciled with the morph dispatch count.
- `renderer_types.cppm`: named budget + pool constants beside `SkinMaxSetsPerFrame`.
- The morph compute pass body (added in Phase 5): a named `GpuScope("morph")`.
- No new control command, no DTO change, no format change in this phase — the contract test must remain
  clean. If the budget surfaces drivable/inspectable state (e.g. current deferred-instance count), that
  is out of scope here; the profiler scope rides the existing readout.

## Frontend changes (this phase)

None — the Timeline channel drill-down + Inspector morph sliders + widened rig gate shipped in Phase 8.
This phase only **documents** them (`timeline.md`).

## Performance considerations

- The budget + distant skip are the AAA-scaling story: a crowd of facial rigs pays only for the near,
  on-screen instances; the rest hold their last deformed buffer (or static bind pose) and defer.
- The named `morph` profiler scope makes the cost live-inspectable so the budget can be tuned against
  real numbers rather than guessed.
- The two-pass InterlockedAdd path is the documented graduation for dense facial rigs (hundreds of
  shapes) — kept out of v1 to preserve the single deform code path, written up so the scale-up is a
  known one-extension move, not a rewrite.
- e2e assertions stay over-the-wire (transforms, weights, counts, bounds) because the harness runs on
  llvmpipe; pixel diffs would be flaky.

## Control commands

None new — `set-morph-weights`, `get-morph-weights`, and `list-clip-bindings` shipped in Phase 7, and
node-TRS reuses the single play/seek/loop/state path. The e2e suite asserts their wire behavior, and the
contract test (`tools/check-control-schema`) + `bun run check` keep the generated DTOs/OpenRPC/manifest
in sync (run as part of the gate).

## Docs pages

- **New:** `animation/morph-targets.md`, `animation/node-trs-animation.md` (each with a
  `What | File | Symbols` table; the two-pass scale-up note in morph-targets).
- **Hub:** `animation/_index.md` — two new rows + rewritten intro (animation is no longer
  skeleton-only).
- **Updated:** `animation/animation-data-model.md`, `animation/playback-runtime.md`,
  `animation/timeline.md`, `geometry-and-assets/smesh-format.md` (one version + flags + morph section),
  `geometry-and-assets/sanim-format.md` (v2 + target kind + morphCount),
  `geometry-and-assets/gltf-and-obj-import.md` (gate lift, sparse decode, node + weights channels).

## Tests

- New e2e: `tests/e2e/morph_weights.test.ts`, `tests/e2e/morph_sparse.test.ts`,
  `tests/e2e/node_trs.test.ts` (specs above) + the bundled fixtures in `tests/e2e/fixtures/`.
- The existing `tests/e2e` suite must stay green (skinning, motion, RT, animation-playback,
  skeleton-overlay, profiler).
- The engine self-tests extended in earlier phases (`.smesh`/`.sanim` round-trip,
  `runSceneSerializationSelfTest`, `runAnimationSelfTest`) run via the present-only smoke in `make
  check`.

## Acceptance criteria

- `morph-targets.md` and `node-trs-animation.md` exist, each with a `What | File | Symbols` table and a
  hub row in `animation/_index.md`; the hub intro is widened (animation is no longer skeleton-only).
- The format pages describe the current single format: `smesh-format.md` (one `MeshFormatVersion` +
  flags + morph section, no v1/v2 dual-version description), `sanim-format.md` (v2 + target kind +
  `morphCount` + `targetName`), `gltf-and-obj-import.md` (gate lift, sparse `unpack_floats` decode, node
  + weights channels). `animation-data-model.md`, `playback-runtime.md`, `timeline.md` updated.
- `morph_weights.test.ts`, `morph_sparse.test.ts`, `node_trs.test.ts` pass with validation-clean logs:
  AnimatedMorphCube CUBICSPLINE (`morphCount==2`, `3·N·M` size, weight round-trip, changing
  `morphWeights`, bounds change frame-over-frame), MorphStressTest sparse (non-zero decoded deltas,
  multi-primitive target-count mismatch rejected), BoxAnimated hierarchy (both nodes move, child composes
  through parent, `list-clip-bindings` resolved).
- A per-frame deformation budget (vertices deformed + BLAS prims updated) and a distant-instance skip
  exist in `submitDrawList`, reconciled with the morph descriptor pool; a named `morph` GPU profiler
  scope is present; the two-pass scale-up is documented (not implemented).
- `make check` + `make e2e` + `make prepare-for-commit` clean (engine build, smoke, contract test,
  script-def drift, project smoke, frontend build + unit tests, format + lint, no warnings from this
  change); `cd docs && hugo` builds and internal links check.
- Each phase file's `**Status:**` is `COMPLETED` and `plans/full-animations/README.md` is `COMPLETED`.

## Risks

- **Fixture size/licensing:** keep fixtures tiny and headless-fast; prefer trimmed/hand-authored `.gltf`
  over full Khronos GLBs (vendor minimal CC-BY files with attribution only if needed).
- **Docs drift / change-journey voice:** the format pages must describe the one current format with no
  "v2 was…" notes (the AGENTS.md comment rule applies to docs voice).
- **Gate flakiness on software GPU:** assert behavior over the wire, not pixels (the established e2e
  style).
- **Budget regressing correctness:** the deferral must round-robin so no instance starves and a deferred
  morph mesh still renders (last deformed buffer, never an undefined buffer); guard with the
  frame-over-frame bounds assertion in `morph_weights.test.ts`.
