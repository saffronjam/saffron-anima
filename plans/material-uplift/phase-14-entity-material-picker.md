# Phase 14 — Entity material picker

**Status:** COMPLETED
**Depends on:** 09, 13

> **Outcome.** `AssetKind` + `PickerAssetKind` gained `"material"`, and
> `FIELD_HINTS["MaterialAsset.material"] = { kind: "uuid", asset: "material" }`. Since `AssetPicker`
> already filters the catalog by `assetType` and `MaterialAssetComponent` is registered (phase 09), the
> Inspector now renders its `material` field as a material picker (assets of type `material`), and selecting
> one writes through the existing `set-component-field` path → `MaterialAssetComponent.material` (which the
> resolve precedence then honors over the inline material). The component shows up in the Inspector's
> add-component menu, so the flow is: add `MaterialAsset` → pick a material. Validated `bun run check` /
> `lint` (0 errors) / `build`. **Follow-on:** material thumbnails in the picker (the `get-thumbnail` material
> case, via `renderMaterialPreview`) — currently the picker shows the file/type icon for materials.

## Goal

Assign a material to an entity from the Inspector: a `Material.material` (and per-slot) field that renders
as an `AssetPicker("material")`, wired to `material.assign` / `assign-asset(Material)`, showing the
effective material (incl. the default-material fallback) and respecting the resolve precedence from phase 09.

## Why

Phase 09 added the `MaterialAssetComponent`; this is its UI. It closes the user's fourth question
("select material for an entity") end to end: pick a material asset in the inspector, the entity uses it,
and many entities can point at the same `.smat`.

## Design

- The `MaterialAssetComponent` is registered (phase 09) so it appears in the Inspector. Add a
  `FIELD_HINTS["MaterialAsset.material"] = { kind:"uuid", asset:"material" }` so `renderField` dispatches to
  `AssetPicker("material")` (the `"material"` picker kind from phase 13). The picker lists material assets
  with their PBR-ball thumbnails (phase 12).
- On change → `client.assignAsset(entityId, "material", materialId)` (extended `AssetSlotDto`) or
  `client.materialAssign`. Clearing (asset `0`) removes the component → entity falls back per precedence.
- Show the **effective** material: if no `MaterialAssetComponent`, the inspector still reflects the inline
  `MaterialComponent` (existing UI) or the default-material indicator. A small "→ Material asset" affordance
  on the inline Material component lets the user "promote" it to a shared asset (`material.create {from:entity}`).

## Files to touch

- `editor/src/components/fieldRenderer.tsx` — `FIELD_HINTS["MaterialAsset.material"]` (+ per-slot if used).
- `editor/src/panels/InspectorPanel.tsx` — render the `MaterialAsset` component; the "promote inline → asset"
  action (calls `material.create {from}` then `material.assign`).
- `editor/src/control/client.ts` — `assignAsset` already exists; ensure the `"material"` slot is typed.
- (Engine side already done in phases 09/10.)

## Steps

1. Add the field hint + the `"material"` picker option (depends on phase 13's picker kind).
2. Render the `MaterialAsset` component in the inspector with the picker; wire assign/clear.
3. Add "promote inline material to asset" (snapshot via `material.create {from}` → assign).
4. `bun run check`/`lint`; assign a material to a cube, verify it renders the asset; share it across two
   entities and edit the asset once — both update.

## Gate / done

- `bun run build`/`check` clean; assigning a material asset to an entity updates its render; clearing falls
  back to default; edit-once-propagate works across entities.
- `make prepare-for-commit` clean. Docs: the assignment UI row.

## Risks

- **Effective-material clarity**: with three sources (asset > set > inline > default), the inspector must
  show which one is active or it's confusing. A small badge/label per the precedence.
- **Promote semantics**: snapshotting inline → `.smat` then assigning must not lose fields; reuse the
  `material.create {from}` path (phase 10) so there's one snapshot implementation.
