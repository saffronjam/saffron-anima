+++
title = 'Inspector'
weight = 6
+++

# Inspector

The Inspector shows the selected entity's components and lets you edit, add, and remove them. It is a React panel with no per-component code: it reads the live `inspect` result from the store and renders whatever components and fields the engine returned, picking a widget for each field by its value shape. A component added engine-side with `registerComponent` shows up here automatically, with a sensible widget even before it has an explicit hint.

## Data-driven, no switch

The panel reads `componentsBySelected` (the `inspect` result, kept fresh by the [reconcile poll](../selection/)) and iterates its `components` map. For each present component it draws a header and walks its fields, handing every `(component, field, value)` to `renderField`:

```tsx
{Object.entries(dto).map(([field, value]) => (
  renderField(component, field, value,
    (next) => onFieldChange(component, field, next),
    { onDragStart, onDragEnd })
))}
```

There is deliberately no `if (component === "Transform")` branch in the render path. Components draw in a canonical order (`Name`, `Transform`, `Mesh`, …) and any unknown component falls in after, but that is ordering only — never a render switch.

## Picking a widget

`renderField` resolves a field to a widget in three steps: (1) the explicit `FIELD_HINTS` table keyed `Component.field`, which mirrors the old C++ per-component widgets; else (2) the value's shape — `{x,y,z}`→vec3, `{x,y,z,w}`→vec4, number, boolean, string; else (3) a read-only text fallback so an unmapped field is still visible. The hint also carries min/max/step, slider-vs-drag, and the asset kind for uuid fields.

The one unit conversion lives at the widget boundary: `Transform.rotation` is **radians on the wire** but shown in **degrees**, driven by the hint's `convertRadians` flag. (SpotLight `innerAngle`/`outerAngle` are degrees on *both* sides — their `unit:"deg"` is just a label and clamp, no conversion.) Material's `baseColor`/`emissive` use color swatches; `metallic`/`roughness` are sliders; `albedoTexture` and `Mesh.mesh` are [asset pickers](../asset-pickers-and-drag-drop/).

## Read-modify-write

The engine's `set-component` rewrites the whole component (no merge), so a single-field edit builds the full DTO with that one field patched and sends the lot:

```ts
const onFieldChange = (component, field, next) => {
  const current = (componentsObj[component] ?? {}) as Record<string, unknown>;
  const patched = { ...current, [field]: next };
  applyOptimisticComponent(component, patched);  // overlay immediately
  coalescerFor(component, field).push(patched);  // coalesced send
};
```

`applyOptimisticComponent` overlays the change on the live inspect result so the widget updates without waiting a poll interval. High-frequency edits (drag a number, move a slider) funnel through a per-(component,field) coalescer, and the drag brackets flip `store.dragActive` so the reconcile poll won't overwrite the optimistic value mid-drag.

A few fields skip the full-DTO write because the engine has merge helpers for them: `Transform` uses `set-transform` and `Material` uses `set-material` (send only the changed field); `Mesh.mesh` and `Material.albedoTexture` use the dedicated `assign-asset`; any other uuid field uses the single-field merge `set-component-field`.

## Add and remove

`add-component` and `remove-component` are guarded the same way the engine is. Remove only shows for removable components — `Name` and `Transform` are in `NON_REMOVABLE`, so the inspector won't strip an entity of its identity or place in the world. The **Add Component** dropdown lists every registered component the entity doesn't already have; selecting one calls `add-component`, and the new component appears on the next reconcile tick.

## In the code

| What | File | Symbols |
|---|---|---|
| The generic panel | `editor/src/panels/InspectorPanel.tsx` | `InspectorPanel`, `orderedComponentNames`, `NON_REMOVABLE` |
| Field-kind dispatch | `editor/src/components/fieldRenderer.tsx` | `renderField`, `resolveHint`, `FIELD_HINTS` |
| Read-modify-write routing | `editor/src/panels/InspectorPanel.tsx` | `onFieldChange`, `sendWrite`, `coalescerFor` |
| Optimistic overlay | `editor/src/state/store.ts` | `applyOptimisticComponent`, `dragActive` |
| Edits (engine) | `control_commands_scene.cpp` | `set-component`, `set-transform`, `set-material`, `set-component-field`, `add-component`, `remove-component` |

## Related

- [Component registry](../../scene-and-ecs/component-registry/) — the registered components `inspect` enumerates
- [Built-in components](../../scene-and-ecs/built-in-components/) — the structs being edited
- [Asset pickers](../asset-pickers-and-drag-drop/) — the mesh/material uuid fields
- [Scene commands](../../tooling-and-control/scene-commands/) — `inspect` and the component edit commands
