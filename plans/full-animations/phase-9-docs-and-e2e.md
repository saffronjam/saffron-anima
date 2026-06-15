# Phase 9 — Docs + e2e fixtures + milestone gate

**Status:** NOT STARTED

**Depends on:** Phases 2-8 (the concepts + commands must exist to document and test)

## Why

Morph targets and node-TRS animation are new engine concepts and new drivable state, so docs must be
updated in the same body of work (AGENTS.md keep-docs-current), and the behavior must be pinned by the
bun e2e harness over the control plane. This phase closes the plan and runs the full gate.

## Grounding

- Docs hubs: `docs/content/explanations/animation/_index.md` (rows: data-model, playback-runtime,
  skeleton-overlay, timeline, foot-ik-and-physics-ahead) and
  `docs/content/explanations/geometry-and-assets/_index.md` (rows incl. `smesh-format`, `sanim-format`,
  `gltf-and-obj-import`). Page style: one concept/page, TOML front matter, title == H1, slim
  `What | File | Symbols` table, plain voice (humanizer pass).
- Format pages to update: `smesh-format.md` (v3 + morph section), `sanim-format.md` (v2 + weights/node
  target kind).
- e2e: `tests/e2e` (bun, boots a headless engine, drives the control plane typed via `@saffron/protocol`,
  asserts responses + a validation-clean log). The reproducible gate `tools/ci/check.sh` (engine build →
  smoke → schema contract → frontend build), `make e2e`, `make check`.
- Khronos sample fixtures: `BoxAnimated.gltf` (node-TRS + hierarchy), `AnimatedMorphCube.gltf` (2 morph
  targets, looping weights, CUBICSPLINE), `MorphPrimitivesTest`/`MorphStressTest` (multi-target/sparse).

## Decisions (locked)

1. **New docs pages.**
   - `animation/morph-targets.md` — the blend-shape concept: sparse delta model, morph-before-skin
     order, the GPU morph compute stage, weights resolution (`mesh.weights`→`node.weights`→animated
     override), the `MorphComponent`/`MorphWeightOverrideComponent` pair, motion-vector + BLAS handling,
     and the `set-morph-weights` command. `What|File|Symbols`: `geometry.cppm` (MorphDelta/MorphTarget),
     `morph.slang`, `renderer.cppm`, `scene.cppm` (MorphComponent), `control_commands_animation.cpp`.
   - `animation/node-trs-animation.md` — animating plain entity transforms: the lifted import gate, the
     Bone/Node/Weights track model, name-scoped node binding, `PoseOverrideComponent` reuse through
     `localMatrix`, the container-player attach. `What|File|Symbols`: `geometry.cppm` (AnimTrack.Target),
     `animation.cpp` (tickAnimation), `scene.cppm` (localMatrix), `assets.cppm` (spawn).
   - Add both rows to the animation hub `_index.md` table, and update its intro (animation is no longer
     skeleton-only).
2. **Update existing pages (concept changed ⇒ page changes).**
   - `animation-data-model.md`: the generalized `AnimTrack` (Bone/Node/Weights, `targetName`), weights
     stream layout, CUBICSPLINE on weights.
   - `playback-runtime.md`: node-TRS + morph application in the one `tickAnimation` write seam.
   - `timeline.md`: the channel drill-down + Inspector morph sliders (the existing model, extended).
   - `smesh-format.md`: v3 header + sparse morph section (delete the v2-as-current description — NO
     LEGACY in docs too).
   - `sanim-format.md`: v2 with target kind + `morphCount`.
   - `gltf-and-obj-import.md`: the gate lift, sparse-accessor decode, node + weights channels.
3. **e2e fixtures + assertions.**
   - `tests/e2e/morph_weights.test.ts`: import `AnimatedMorphCube`, `set-morph-weights` then
     `get-morph-weights` round-trip; `play-animation` and poll `get-animation-state` for changing
     `morphWeights`; assert a validation-clean log.
   - `tests/e2e/node_trs.test.ts`: import `BoxAnimated`, spawn, `play-animation` on the container, poll
     transforms over frames asserting both nodes move and the child composes through the parent.
   - Bundle the Khronos fixtures (or a minimal hand-authored equivalent) under the e2e assets dir; keep
     them small (the harness boots headless).
4. **Plan bookkeeping.** Mark each phase `COMPLETED` as it lands; mark this README `COMPLETED` when all
   ship; delete a phase file only after it is `COMPLETED` (AGENTS.md plans rule).

## Edits

- `docs/content/explanations/animation/morph-targets.md`, `node-trs-animation.md` (new); `_index.md`
  rows + intro; updates to `animation-data-model.md`, `playback-runtime.md`, `timeline.md`.
- `docs/content/explanations/geometry-and-assets/smesh-format.md`, `sanim-format.md`,
  `gltf-and-obj-import.md` (updates).
- `tests/e2e/morph_weights.test.ts`, `tests/e2e/node_trs.test.ts` (new) + fixtures.

## Verification (the full milestone gate)

- `make check` (engine build → present-only smoke → control-schema contract → frontend build).
- `make e2e` green (the two new tests + the existing suite), validation-clean logs.
- `make prepare-for-commit` (format + lint) clean across C++ + editor TS.
- `cd docs && hugo` builds; link-check the new pages + hub rows.

## Risks

- **Fixture licensing/size:** Khronos sample assets are CC-licensed; vendor only the minimal files
  needed, or hand-author tiny equivalents (a 2-node BoxAnimated-shape, a 2-target morph quad) to keep
  the repo lean and the headless boot fast.
- **Docs drift:** the format pages must describe v3/v2 as *the* format (no "v2 was…" change-journey
  notes — AGENTS.md comment rule applies to docs voice too).
- **Gate flakiness on software GPU:** the e2e harness runs on llvmpipe; assert behavior over the wire
  (transforms, weights) not pixels, consistent with the existing e2e style.
