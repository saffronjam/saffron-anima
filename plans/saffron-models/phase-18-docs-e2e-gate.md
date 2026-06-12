# Phase 18 — Docs, e2e hardening, gate

**Status:** COMPLETED
**Depends on:** 17

> Implementation note: new docs pages `smodel-container.md` (explanation) + `clean-unused-assets.md`
> (how-to), with the import-pipeline + asset-catalog explanation pages and both hub `_index.md` rows
> updated; hugo builds both new pages clean. The full-flow e2e is `tests/e2e/model_flow.test.ts`
> (import → reload-survives → instantiate ×2 → extract → reimport-skip → references → clean, all
> validation-clean). Gate: engine build green; present-only smoke renders 10 frames validation-clean
> (the VMA teardown abort is the pre-existing, unrelated leak the plan flagged); control-schema contract
> = 135 checks; `bun run check` + `bun run lint` clean; frontend `bun run build` green; full e2e 182
> pass / 1 fail (the pre-existing camera `frustumMaxDistance` default mismatch, no `.smodel` involvement).
> Prose ran through a humanizer self-review. **Git left unstaged per the repo rule.**

## Goal

Land the documentation, harden the end-to-end coverage into one full-flow round-trip, and bring the whole
feature through the reproducible gate. Add a `.smodel` container docs page + a cleanup how-to, update the
import-pipeline and asset-catalog explanation pages and their hub `_index.md` rows, write a full-flow e2e
(import → scan → instantiate → extract → reimport → clean) that stays validation-clean, get `make check`
green, and run the prose through the humanizer. Flip the plan's Status lines to `COMPLETED` as work lands.

## Why

Per AGENTS.md, a change that adds/alters an engine concept updates the matching docs page in the same
change, and `make check` (engine build → smoke → contract test → frontend build) is the reproducible
definition of done. This phase is the "keep current" obligation for the whole plan and the final proof that
the pieces compose.

## Docs

- **New:** `docs/content/explanations/geometry-and-assets/smodel-container.md` — the `.smodel` concept:
  why one self-contained file, the header/TOC/MetadataChunk layout, sub-assets by `(modelId, subId)`,
  embedded-vs-extracted + remap, the prefix-read scan, reimport. House style: title = sentence-case noun
  phrase matching the `# H1`; a slim `What | File | Symbols` table; KaTeX/mermaid where useful; humanizer
  pass.
- **New:** `docs/content/how-to/clean-unused-assets.md` — the deliberate cleanup workflow (categories,
  dry-run, confirm, VCS-commit-first).
- **Update:** `docs/content/explanations/geometry-and-assets/import-pipeline.md` (translator → bake; import
  no longer spawns) and `asset-server-and-catalog.md` (filesystem source of truth, scan-derived catalog,
  `.smeta`, the regenerable cache). Update the hub `_index.md` rows for each.
- Note in the docs that `.smodel` supersedes the `editor-view` `.srig` sidecar for rig persistence.

## E2e hardening

A single full-flow test on a `tests/e2e/fixtures` model, asserting a validation-clean log throughout:
1. `import-model-to-asset` → one `.smodel`, N catalog rows, no entity, no loose textures.
2. delete `project.json`, reload → `scan-assets` reconstructs the identical catalog (orphan-proof).
3. `instantiate-model` ×2 → two independent entity trees.
4. `extract-subasset` a material → standalone `.smat`, same id, remap set, instance resolves the external.
5. edit the source → `reimport-model` → geometry updates on the live instance, extracted material survives,
   a no-op reimport is `skipped`.
6. `clean-assets` → an unused model flagged, the used one kept, a script-referenced asset = review;
   `delete-unused {confirm}` removes only the unused one.

## Files to touch

- `docs/content/explanations/geometry-and-assets/` (+ new page + `_index.md`), `docs/content/how-to/`.
- `tests/e2e/` — the full-flow round-trip + fixtures.
- `plans/saffron-models/README.md` + each `phase-NN-*.md` — flip `**Status:**` to `COMPLETED` as landed.

## Steps

1. Write the `.smodel` docs page + the cleanup how-to; update import-pipeline + asset-catalog pages + hub
   rows; humanizer pass on all prose.
2. Write the full-flow e2e; ensure it self-spawns the headless engine + weston as the suite does.
3. Run `make check` (engine build → present-only smoke → control-schema contract → `bun run build`); fix
   every warning.
4. Flip Status lines to `COMPLETED`; update the README progress section.

## Gate / done

- `make check` green (engine + smoke + contract + frontend); `make e2e` full-flow round-trip validation-
  clean; `make prepare-for-commit` clean; docs build (`hugo`) clean with no broken links; humanizer pass
  done.
- The plan README + all phase Status lines reflect reality.

## Risks

- **Headless display for the gate:** `make schema`/smoke need a Wayland display; wrap in weston (the e2e
  harness self-spawns one). Capture the exit code before any `pkill` (the toolbox wrapper surfaces the
  pkill signal, not the real exit).
- **Pre-existing teardown noise:** the headless smoke may abort at teardown on the known VMA "allocations
  not freed" assertion unrelated to this work; confirm rendering runs validation-clean up to teardown and
  don't chase a pre-existing leak.
- **Docs/code drift:** symbols move during implementation; the docs `What | File | Symbols` table uses
  symbols (not line numbers) for resilience, but verify them at the end.
- **E2e flakiness:** the full-flow test touches the filesystem (delete `project.json`, edit a source); make
  it hermetic (a temp project dir) so it doesn't depend on or mutate a shared fixture.
