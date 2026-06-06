+++
title = 'Inspector'
weight = 6
+++

# Inspector

The Inspector is the editor panel for viewing and editing the components of the selected entity. It is data-driven: it holds no per-component code, rendering whatever components and fields the engine returns and choosing a widget for each field from its value shape.

A component registered engine-side with `registerComponent` appears here automatically, with a sensible widget even before it has an explicit hint. The panel describes itself entirely from the live `inspect` result, so the editor and the engine never drift on what an entity holds.

## How it works

The panel reads `componentsBySelected` (the `inspect` result, kept fresh by the [reconcile poll](../selection/)) and iterates its `components` map. For each present component it draws a header and walks its fields, handing every `(component, field, value)` to `renderField`:

```tsx
{Object.entries(dto).map(([field, value]) => (
  renderField(component, field, value,
    (next) => onFieldChange(component, field, next),
    { onDragStart, onDragEnd })
))}
```

The render path has no `if (component === "Transform")` branch. Components draw in a canonical order (`Name`, `Transform`, `Mesh`, …) and any unknown component falls in after, but that is ordering only, never a render switch.

## Picking a widget

`renderField` resolves a field to a widget in three steps:

1. The explicit `FIELD_HINTS` table, keyed `Component.field`, which mirrors the C++ per-component widgets.
2. The value's shape — `{x,y,z}`→vec3, `{x,y,z,w}`→vec4, number, boolean, string.
3. A read-only text fallback, so an unmapped field is still visible.

A hint also carries min/max/step, slider-vs-drag, and the asset kind for uuid fields. Field labels are sentence-cased from the wire key (`humanizeFieldName`: `albedoTexture` → "Albedo texture"), and color fields open a saturation/hue (and alpha) canvas in a popover rather than the native OS picker. Selecting **(none)** in a mesh or material-texture picker clears the slot — the engine treats the `0` asset id as "unassigned" rather than rejecting it.

One unit conversion lives at the widget boundary. `Transform.rotation` is radians on the wire but shown in degrees, driven by the hint's `convertRadians` flag. SpotLight `innerAngle`/`outerAngle` are degrees on both sides, so their `unit:"deg"` is a label and clamp only, no conversion. Material's `baseColor`/`emissive` use color swatches, `metallic`/`roughness` are sliders, and `albedoTexture` and `Mesh.mesh` are [asset pickers](../asset-pickers-and-drag-drop/).

## Read-modify-write

The engine's `set-component` rewrites the whole component rather than merging, so a single-field edit builds the full DTO with that one field patched and sends the lot:

```ts
const onFieldChange = (component, field, next) => {
  const current = (componentsObj[component] ?? {}) as Record<string, unknown>;
  const patched = { ...current, [field]: next };
  applyOptimisticComponent(component, patched);  // overlay immediately
  coalescerFor(component, field).push(patched);  // coalesced send
};
```

`applyOptimisticComponent` overlays the change on the live inspect result so the widget updates without waiting a poll interval. High-frequency edits — dragging a number, moving a slider — funnel through a per-(component,field) coalescer, and the drag brackets flip `store.dragActive` so the reconcile poll will not overwrite the optimistic value mid-drag.

A few fields skip the full-DTO write because the engine offers merge helpers for them:

- `Transform` uses `set-transform` and `Material` uses `set-material`, sending only the changed field.
- `Mesh.mesh` and `Material.albedoTexture` use the dedicated `assign-asset`.
- Any other uuid field uses the single-field merge `set-component-field`.

## Add and remove

`add-component` and `remove-component` are guarded the same way the engine guards them. Remove only shows for removable components: `Name` and `Transform` are in `NON_REMOVABLE`, so the inspector cannot strip an entity of its identity or place in the world. The Add Component dropdown lists every registered component the entity does not already have; selecting one calls `add-component`, and the new component appears on the next reconcile tick.

## In the code

| What | File | Symbols |
|---|---|---|
| The generic panel | `editor/src/panels/InspectorPanel.tsx` | `InspectorPanel`, `orderedComponentNames`, `NON_REMOVABLE` |
| Field-kind dispatch | `editor/src/components/fieldRenderer.tsx` | `renderField`, `resolveHint`, `FIELD_HINTS` |
| Color canvas + label casing | `editor/src/components/ColorField.tsx`, `editor/src/lib/humanize.ts` | `ColorField`, `humanizeFieldName` |
| Read-modify-write routing | `editor/src/panels/InspectorPanel.tsx` | `onFieldChange`, `sendWrite`, `coalescerFor` |
| Optimistic overlay | `editor/src/state/store.ts` | `applyOptimisticComponent`, `dragActive` |
| Edits (engine) | `control_commands_scene.cpp` | `set-component`, `set-transform`, `set-material`, `set-component-field`, `add-component`, `remove-component` |

## Related

- [Component registry](../../scene-and-ecs/component-registry/) — the registered components `inspect` enumerates
- [Built-in components](../../scene-and-ecs/built-in-components/) — the structs being edited
- [Asset pickers](../asset-pickers-and-drag-drop/) — the mesh/material uuid fields
- [Scene commands](../../tooling-and-control/scene-commands/) — `inspect` and the component edit commands
