# Phase 13 — Material Editor panel

**Status:** NOT STARTED
**Depends on:** 10, 12

## Goal

The visual material editor: a new `RightTool: "material"` panel (slot inspector) built on the existing
field renderer, the catalog plumbing to make materials visible/selectable in the editor, and a **live
preview sphere** that updates as you edit (via `preview.render`, coalesced). This is the UE Material
Instance editor equivalent — fixed slots, instant feedback, no node graph yet.

## Why

Artists need to edit materials visually. The editor already has every primitive: `renderField`
(auto-dispatch from `FIELD_HINTS`), `AssetPicker`, `makeCoalescer`, the thumbnail blob pipeline. This
phase assembles them into a material panel and adds the bits the store is missing (material catalog
state + selection).

## Design

- **Catalog plumbing**: `AssetKind`/`PickerAssetKind` gain `"material"` (`fieldRenderer.tsx`/`AssetPicker.tsx`);
  `client.listAssets` already returns the catalog (materials appear once `AssetType::Material` exists);
  add `selectedMaterialId` to the store. `se-types.ts` regenerates with the `Material` asset type.
- **Panel** (`RightTool: "material"`): renders the selected material (`material.get`). One row per field
  via `renderField`: `baseColor` (color4), `metallic`/`roughness`/`normalStrength`/`heightScale`/`alphaCutoff`
  (slider), `emissive`+`emissiveStrength`, `blend`/`unlit`/`doubleSided` (enum/bool), and one `AssetPicker("texture")`
  per role (albedo, ORM, normal, emissive, height). Normal row adds a GL/DX badge; ORM row shows the channel
  routing. Writes go through `client.update`-material (phase 10) wrapped in `makeCoalescer` for smooth sliders;
  `dragActive` gates the reconcile poll during drags.
- **Live preview**: a sphere `<img>` at the top of the panel, sourced from `preview.render(materialId)`;
  re-request (debounced) after each coalesced write completes. Reuse the `base64ToBlob`/object-URL pattern.

## Files to touch

- `editor/src/state/store.ts` — `RightTool` += `"material"`; `selectedMaterialId` + setter;
  `openRightTool("material")`.
- `editor/src/panels/MaterialEditorPanel.tsx` — new panel (slot rows + preview sphere).
- `editor/src/panels/RightSidebar.tsx` — `TOOL_LABEL["material"]` + the panel switch case.
- `editor/src/components/fieldRenderer.tsx` / `AssetPicker.tsx` — `AssetKind`/`PickerAssetKind` += `"material"`;
  Material field hints for the new fields.
- `editor/src/control/client.ts` — `material.create/list/get/update/assign` + `preview.render` wrappers
  (typed via the regenerated `se-types.ts`).

## Steps

1. Regenerate protocol (from phase 10/12) so `client` + `se-types` have the material/preview calls + the
   `"material"` asset type.
2. Add `selectedMaterialId` + `RightTool: "material"`; wire `RightSidebar`.
3. Build `MaterialEditorPanel`: field rows via `renderField`, texture `AssetPicker`s, coalesced writes.
4. Add the live preview `<img>` driven by `preview.render`; debounce re-render after edits.
5. `bun run check` (regenerate + typecheck), `bun run lint`; manually edit a material and watch the sphere update.

## Gate / done

- `bun run build` + `bun run check` clean; editing roughness/baseColor/textures updates the preview sphere live.
- `make engine` (unchanged) + `make prepare-for-commit` clean.
- Docs: the material editor UI page + hub row.

## Risks

- **Selection source**: the panel needs a selected *material asset* (catalog selection), distinct from the
  selected *entity*. Add `selectedMaterialId` explicitly; opening a material from the assets panel sets it.
- **Preview latency**: `preview.render` is one-frame; debounce so a slider drag doesn't queue dozens of
  renders. Show the last good frame during a pending render (don't flicker).
- **Coalescing**: reuse the InspectorPanel coalescer pattern exactly; mid-drag `dragActive` must gate the poll.
