# Phase 16 — Material instances / variants

**Status:** COMPLETED
**Depends on:** 13

> **Outcome.** `MaterialAsset` gained `parent` (Uuid) + a sparse `overrides` map (serde'd). Split the load:
> `loadMaterialAssetRaw` (the stored `.smat` as-is — the edit path) vs `loadMaterialAsset` (resolved for
> rendering: an instance = its parent's resolved params with `applyOverrides` on top, recursive with a
> depth-8 cycle guard; a master applies its foldable graph). `material-create-instance {parent} [name]`
> mints a child; `material-set-override {material, field, value}` writes one sparse override (`null`
> overrides becomes an object on `[]`). e2e `material_instance.test.ts` proves the three behaviors:
> pure inheritance (instance render == parent), override divergence (a roughness override changes it), and
> **reflow** (editing the parent's baseColor re-renders the instance while keeping its override). 122/122
> contract. **Follow-on:** the editor's override UI (greyed inherited fields + per-field override toggles)
> is React work for the node-graph editor pass (phase 20-era).

## Goal

A `.smat` may declare a `parent` material and a sparse set of `overrides`; at load, parent→child resolves
into the final flat parameter block. This is the UE Material Instance / Unity Material Variant model —
one master material drives many surfaces, each overriding only what differs. The PSO is shared; only the
`MaterialParams` entry differs.

## Why

It's the natural extension of shared assets and it's nearly free here: resolution already produces a flat
`MaterialParamsData`, so an instance is "resolve parent, apply sparse overrides." It massively reduces
duplication (50 rock variants from one master) and is the workflow artists expect.

## Design

- `.smat` gains optional `"parent": "<uuid>"` + `"overrides": { ...subset of factors/textures/features... }`.
  A material with a `parent` is an **instance**; without, a **master**.
- Resolve: load parent (recursively, with a cycle guard + depth cap) → flat params, then apply the child's
  `overrides` on top → final `MaterialParamsData`/`SubmeshMaterial`. Cache by `(materialId, version)`; an
  instance's effective version = max of its own + its ancestors' (editing the master reflows instances).
- Editor: the material panel shows inherited values greyed with a per-field **override toggle** (UE's
  checkbox model). Toggling on captures the current value into `overrides`; off reverts to the parent's.

## Files to touch

- `engine/source/saffron/assets/assets.cppm` — `parent`/`overrides` in `MaterialAsset` + the to/from-JSON;
  recursive resolve with cycle/depth guards in `resolveMaterialAsset`; version reflow.
- `engine/source/saffron/control/control_dto.cppm` + `control_commands_*.cpp` — `SmatDto` carries parent +
  overrides; `material.update` can set/clear an override or the parent; `material.create {parent}`.
- `tools/gen-control-dto/gen.ts` — regenerate for the DTO changes.
- `editor/src/panels/MaterialEditorPanel.tsx` — inherited/override UI (greyed rows + toggles); show the parent.

## Steps

1. Add `parent`/`overrides` to the format + resolve (cycle guard, depth cap, version reflow).
2. Extend `SmatDto` + `material.create/update` for parent/override ops; regenerate protocol.
3. Editor: render inherited values + per-field override toggles; "create instance of this material".
4. e2e: master + instance overriding roughness only; editing the master's baseColor reflows the instance;
   the instance's roughness stays overridden.

## Gate / done

- `make engine` + `make schema` clean; an instance overrides a subset and reflows on master edit.
- `make e2e` instance suite green; editor shows inherited vs overridden. `make prepare-for-commit` clean.
- Docs: material instances/variants.

## Risks

- **Cycles**: a parent chain can loop; guard with a visited set + a max depth, fall back to default on cycle.
- **Version reflow correctness**: an instance must invalidate when any ancestor changes; compute effective
  version transitively or bump descendants on master edit.
- **Override granularity**: per-field is the UE model; don't allow partial-texture overrides that desync
  channel routing — override whole texture slots.
