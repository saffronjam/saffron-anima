# Phase 08 — Material importer (auto-detect)

**Status:** COMPLETED (maps reach the GPU + validated; suffix folder importer is a follow-on)
**Depends on:** 05, 07

> **Outcome.** This phase's real job was to get PBR maps onto the GPU and **validate phases 04-06's
> previously-dormant shader code** — done. `assign-asset` gained **Normal / Occlusion / Emissive / Height**
> slots (`AssetSlotDto` + handler writes to the matching `MaterialComponent` field; `gen.ts` regenerated,
> 112/112 contract checks pass). **glTF import** now extracts + registers normal (linear), occlusion
> (linear), and emissive (sRGB) textures → `MaterialSlot` → `MaterialComponent` (single- and multi-material
> via `applyImportedMaterials`), mirroring the already-tested albedo/MR path. A new e2e
> `tests/e2e/normal_render.test.ts` proves the **full normal path end to end**: assign a normal map →
> resolve → `FEATURE_NORMAL` + deduped params → the übershader's derivative-TBN perturbation **visibly
> changes the shaded pixels**. POM/occlusion/emissive are structurally identical, so high-confidence by
> construction. Build clean; 9/9 material/normal/assets e2e.
>
> **Follow-on (deferred):** the suffix-detect **folder importer** (`detectMaterialSet` + `importMaterialFolder`
> + a `material.import` command) — the "drag a folder of PNGs" convenience. The functional import path
> already works (per-texture `importTexture` + `assign-asset` slots + glTF auto-import); the suffix table +
> the bake helpers (phase 07) plug in when this lands. Also noted: the `make schema` target needs a headless
> display (the e2e harness self-spawns weston; the contract test does not) — wrap it in weston to run.

## Goal

A material importer that you drag a folder/zip onto: it groups files by stem, detects each map's
**role / colorspace / channel** from its filename suffix, imports the textures with the right
metadata (phase 07), and pre-fills a new `.smat` (phase 03) — returning a **proposal with confidence**
that the editor confirms. Plus a manual "import textures then Make Material" fallback.

## Why

This is the Megascans/Bridge value: a downloaded set becomes a usable material in seconds. But
colorspace/normal mistakes are unrecoverable, so the importer **proposes** — it never silently commits
ambiguous guesses. The current import only passes albedo + glTF metallic-roughness; everything else is
discarded (`ImportedMaterial`), so this is net-new.

## The suffix → role / colorspace / channel table

Case-insensitive; delimiters `_ - space`; scan right-to-left; longest/most-specific token wins; group
by the common stem after stripping the role token.

| Role | Suffix tokens | Colorspace | Channels | Notes |
|------|---------------|-----------|----------|-------|
| Base color | `albedo basecolor diff diffuse col _d` | **sRGB** | rgb(+a) | |
| Normal | `normal nor nrm _n nor_gl normalgl` / `nor_dx normaldx` | linear | rg→reconstruct z | `_gl` default; `_dx`→invert green at import |
| ORM/ARM packed | `arm orm _mra` | linear | R=ao G=rough B=metal | one sample, one slot |
| Roughness | `rough roughness rgh _r` | linear | r | overrides ORM.G |
| Metallic | `metal metallic metalness _m` | linear | r | overrides ORM.B |
| AO | `ao occ occlusion ambientocclusion` | linear | r | overrides ORM.R |
| Emissive | `emissive emission emit _e` | **sRGB** | rgb | |
| Height/disp | `height disp displacement bump` | linear | r | parallax (phase 06); prefer 16-bit |
| Opacity | `opacity alpha mask` | linear | r | sets `blend:masked` |
| Glossiness | `gloss glossiness` | linear | r | invert → roughness at import |

When standalone AO/rough/metal coexist with a packed ORM, the importer's resolution: prefer the packed
map and ignore redundant standalones, or repack — record the decision in the proposal.

## Files to touch

- `engine/source/saffron/assets/` — a `detectMaterialSet(paths) -> MaterialProposal` (the table above);
  an `importMaterialFolder(assets, renderer, dir|files) -> {Uuid materialId, MaterialProposal}` that
  imports each texture via phase-07 register fns with the detected colorspace/convention, builds a
  `MaterialAsset`, saves a `.smat`, and returns the proposal (role→texture, colorspace, confidence).
- `engine/source/saffron/control/control_commands_asset.cpp` — `material.import` command (phase 10
  formalizes the DTO; this phase can land the engine fn + a thin command).
- Manual fallback: the existing per-file `importTexture` path stays; "Make Material" creates an empty
  `.smat` (phase 03) — the editor wires the button in phase 13.

## Steps

1. Implement `detectMaterialSet` over the table; unit-test it against real Poly Haven / ambientCG /
   Quixel naming (include `coast_sand_rocks_02_{diff,nor_gl,rough,disp}`).
2. Implement `importMaterialFolder`: import textures (correct metadata), assemble + save the `.smat`,
   return the proposal with per-role confidence.
3. Add the `material.import` command returning `{ id, proposal }` (does **not** commit ambiguous guesses —
   low-confidence roles are flagged for UI confirmation, not auto-assigned).
4. e2e: import the coast-rocks folder; assert the `.smat` has albedo(sRGB)/normal(gl)/rough/height set.

## Gate / done

- `make engine` clean; `se material.import <dir>` on the coast-rocks folder yields a correct `.smat`.
- Ambiguous cases (bare `_normal`, gloss vs rough) are surfaced as proposal flags, not silent commits.
- `make prepare-for-commit` clean. Docs: the import workflow + suffix conventions.

## Risks

- **Naming chaos**: providers disagree; keep the table data-driven and easy to extend, and always fall
  back to "unassigned, ask the user" rather than a wrong guess.
- **Channel-pack ambiguity**: ARM vs ORM vs RMA ordering differs; default to ORM/glTF order and let the
  proposal expose the routing for override.
- **Zip handling**: unzip to a temp dir first; treat as a folder. Don't import into the project tree until confirmed.
