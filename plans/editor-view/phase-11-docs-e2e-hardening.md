# Phase 11 — docs, e2e hardening, gate

**Status:** COMPLETED (kept as a build record; superseded by the general Asset editor — see this dir's README)

**Depends on:** phase 1 (the rig query commands), phase 2 (the preview scene + byte-identity), phase 3
(preview furnishing + the skeleton overlay), phases 5–9 (the workspace, skeleton tree, clip list, and
timeline), phase 10 (the asset affordances) — the whole feature complete, plus `plans/saffron-models`
(the `.smodel` container + `instantiateModel` the preview is built on).

## Goal

Close the plan: the full-flow end-to-end test, the documentation set, and a final pass over the
reproducible gate — the phase that earns flipping the plan's status to COMPLETED.

## What exists to build on

- Per-phase e2e already covers the parts: the `.smodel` rig read (phase 1's `get-rig`/`list-clips`
  against the container), preview byte-identity (phase 2), furnishing screenshots (phase 3), seek-pose
  tracking (phase 9), the DTO/affordance surface (phase 10). This phase adds the **composed** flow and
  the cross-cutting invariants. (The container round-trip and `import-model` themselves are
  saffron-models' e2e territory; this plan's tests start from an imported `.smodel`.)
- The e2e harness (`tests/e2e/harness.ts`) boots per-test engines with validation capture; the
  contract test (`tools/check-control-schema`) sweeps every command; `make check` is the
  reproducible gate (`tools/ci/check.sh`: regen-diff → engine → smoke → contract → editor build).
- Docs structure: Diátaxis under `docs/content/` — explanations per concept, hubs with row tables;
  the animations plan's pages (`animation/_index.md`, `timeline.md`, `skeleton-overlay.md`) are the
  cross-link targets; the docs-page skill/house style applies.

## Work

### 1. The full-flow e2e (`tests/e2e/rig-editor.test.ts`)

One engine, the composed journey, asserting at each seam:
1. Import `leg.gltf` → the `.smodel` container exists and `get-rig` resolves (3 bones, 1 clip) from
   its metadata.
2. Snapshot `project.json` (save).
3. `enter-rig-preview` → bone table resolves; `play-animation` paused-pick → `seek-animation` ×3 →
   a bone's `get-world-transform` tracks the pose; screenshots at two seek times differ.
4. Switch preview to a second rig (enter while entered) → states swap cleanly.
5. `exit-rig-preview` → save → **byte-identical** `project.json`; `list-entities` identical.
6. Validation-clean log throughout.

Plus the negative lanes: enter during Play rejected; `play` during preview rejected; `get-rig` on a
clip whose owning model has no rig (no skin in its `.smodel`) errors with the stable message.

### 2. The documentation set

- `docs/content/explanations/ui-and-editor/rig-editor.md` — the editor view: what it shows, the
  open paths, the preview-scene model (Edit/Play/Preview triad), the takeover-not-second-viewport
  decision, the What|File|Symbols table.
- `docs/content/explanations/geometry-and-assets/` (or the asset-model page): the rig as
  asset-persisted data — the node hierarchy + skin in the `.smodel` MetadataChunk, the clips and
  materials as sub-assets of the same container, and `get-rig` reading both from there. Saffron-models
  owns the `.smodel` container page; this page covers how the rig editor reads the rig and clips from
  the container metadata and its animation sub-assets.
- Hub `_index.md` rows for both; cross-links from `animation/timeline.md` (shared components,
  different target) and `skeleton-overlay.md` (the preview defaults it on).
- Honesty notes the research flagged: re-import re-derives the rig from the container (the
  association is intrinsic to the one file); one live stream (no scene+preview simultaneously).

### 3. The gate + polish pass

- `make check` green end-to-end; `make e2e` full suite; the contract test with all new fixtures;
  `make prepare-for-commit` across the accumulated tree.
- A skim for the revamp constraints (no global tool slices, no hand-rolled strips, shared timeline
  components clean of module-level cross-mount state) — written down as checked.
- Update this plan's README phase table to COMPLETED as the final act (per the plans rule: delete
  only after COMPLETED, and only deliberately).

## Validation (done criteria)

- All of §1 green in CI conditions (headless weston, software GPU); the suite is deterministic
  (no wall-clock pose assertions — the animations-plan flake lessons: compare frozen poses, settle
  generously).
- Docs build clean (`make run-docs` / Hugo), no broken links (the link-check pass).
- `make check` exits 0 from a clean tree.

## Notes / gotchas

- The byte-identity assertion is the plan's keystone invariant — if any furnishing/selection state
  leaks into the authored scene's save, this test is what catches it; do not weaken it to
  structural comparison.
- Screenshot-diff assertions inherit the known timing flake — follow the settled pattern from
  `skinned-motion.test.ts`/`foot-ik.test.ts`: freeze poses via pause+seek before comparing, never
  compare two moving frames.
- If the tabsystem-revamp has landed by the time this phase runs, re-verify the workspace against
  its TabStrip/panel-host — the constraints were designed to make that a no-op, but check, don't
  assume.
