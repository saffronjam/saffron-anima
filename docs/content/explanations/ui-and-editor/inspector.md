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

A hint also carries min/max/step, slider-vs-drag, the option list for a closed enum (drawn as a Select — `Rigidbody.motion`, `Collider.shape`, `AnimationPlayer.wrap` and `transitionMode`), and the asset kind a uuid field picks from: `mesh`, `texture`, `material`, `model` (the `.smodel` a `ModelInstance` came from), or `animation` (an `AnimationPlayer` clip). Without a hint an enum or id would fall to step 3 and render as a free-text box, so each closed enum and each id reference is hinted. Field labels are sentence-cased from the wire key (`humanizeFieldName`: `albedoTexture` → "Albedo texture"), and color fields open a saturation/hue (and alpha) canvas in a popover rather than the native OS picker. Selecting **(none)** in a mesh or material-texture picker clears the slot — the engine treats the `0` asset id as "unassigned" rather than rejecting it.

One unit conversion lives at the widget boundary. `Transform.rotation` is radians on the wire but shown in degrees, driven by the hint's `convertRadians` flag. SpotLight `innerAngle`/`outerAngle` are degrees on both sides, so their `unit:"deg"` is a label and clamp only, no conversion. Material's `baseColor`/`emissive` use color swatches, `metallic`/`roughness` are sliders, and `albedoTexture` and `Mesh.mesh` are [asset pickers](../asset-pickers-and-drag-drop/).

## Import-derived components

A few components carry data the importer fills, not the user. A rig's `SkinnedMesh` holds the bone-entity array and the per-joint inverse-bind matrices; `FootIk.chains` and `KinematicBones.driven` hold integer indices into that bone array. Through the generic grid these become an editable JSON blob of ids and matrices — noise no one hand-edits and easy to corrupt. So those components get a small bespoke body: the editable scalars (`FootIk.enabled`/`groundHeight`, `KinematicBones.enabled`) stay live widgets, while the import-derived parts render read-only and resolved to names — the mesh and root bone by catalog/entity name, the IK chains and driven set by joint name (each index resolved through `SkinnedMesh.bones` to that joint entity's `Name`), and a joint count in place of the raw matrices. `BonePhysics` shows a one-line body-count readout for the same reason; its ragdoll blend is driven from the [Physics panel](../physics-panel/). The resolution is entirely client-side — the joint names are already in the [hierarchy](../hierarchy-panel/) entity list, so no extra engine round-trip is needed.

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
- `MaterialSet` renders one editor per slot and routes each field through `set-material` with the slot index.
- Any other uuid field uses the single-field merge `set-component-field`.

## Smoothed drags, drag-local widgets

A drag samples at the webview's pointer rate (~60 Hz), far below the engine's frame rate, so writing each sample directly would render as visible steps. Material and Transform edits borrow the [gizmo's](../gizmo/) answer: mid-drag sends carry `smooth:1`, which makes `set-material`/`set-transform` record the numeric fields as per-entity targets instead of writing them, and the engine converges the live component toward those targets every rendered frame with the same ~25ms exponential the gizmo uses for pointer samples (`stepEditSmoothing`). Once within epsilon the value snaps exactly and the entry is dropped. Transform smoothing yields to a live gizmo drag on the same entity, and applies exact under preserve-children (each write must rebase the subtree).

The widgets themselves never wait on that round trip. Every scrub widget (NumberDrag, SliderField, VectorEditor, ColorField) renders drag-local state through `useScrubValue`: the pointer updates the widget immediately, changes flow outward at most once per animation frame, and the prop only drives the widget when no gesture is active — so the color canvas or a scrubbed axis tracks the cursor exactly while the store, wire, and viewport follow.

The release always ends the stream with one exact write: the widget flushes its pending emit, then `onFieldDragEnd` re-pushes the field's latest optimistic value after clearing `dragActive`, and a non-smooth send both writes verbatim and cancels any pending animation for that entity. Texture and `unlit` are not animatable and apply immediately either way.

## Add and remove

`add-component` and `remove-component` are guarded the same way the engine guards them. Remove only shows for removable components: `Name`, `Transform`, and the import-managed identity components `ModelInstance` and `SkinnedMesh` are in `NON_REMOVABLE`, so the inspector cannot strip an entity of its identity, its place in the world, or its rig. The Add Component dropdown lists every registered component the entity lacks, minus two exclusions: components written only by import (`ModelInstance`, `SkinnedMesh`, and `MaterialSet`) never appear, and the rig sidecars that index a skeleton (`AnimationPlayer`, `FootIk`, `KinematicBones`, `BonePhysics`) appear only on an entity that already carries a `SkinnedMesh`. Selecting one calls `add-component`, and the new component appears on the next reconcile tick.

## In the code

| What | File | Symbols |
|---|---|---|
| The generic panel | `editor/src/panels/InspectorPanel.tsx` | `InspectorPanel`, `NON_REMOVABLE`, `NON_ADDABLE`, `RIG_ONLY` |
| Component order + rig bodies | `editor/src/lib/componentOrder.ts`, `editor/src/panels/InspectorPanel.tsx` | `COMPONENT_ORDER`, `orderedComponentNames`, `ReadonlyRow` |
| Field-kind dispatch | `editor/src/components/fieldRenderer.tsx` | `renderField`, `resolveHint`, `FIELD_HINTS`, `AssetKind` |
| Color canvas + label casing | `editor/src/components/ColorField.tsx`, `editor/src/lib/humanize.ts` | `ColorField`, `humanizeFieldName` |
| Read-modify-write routing | `editor/src/panels/InspectorPanel.tsx` | `onFieldChange`, `sendWrite`, `coalescerFor` |
| Optimistic overlay | `editor/src/state/store.ts` | `applyOptimisticComponent`, `dragActive` |
| Edits (engine) | `control_commands_scene.cpp` | `set-component`, `set-transform`, `set-material`, `set-component-field`, `add-component`, `remove-component` |
| Smoothed drags (engine) | `scene_edit_gizmo.cpp`, `scene_edit_context.cppm` | `stepEditSmoothing`, `MaterialSmoothTarget`, `TransformSmoothTarget` |
| Drag-local widgets | `editor/src/lib/useScrubValue.ts` | `useScrubValue`, `ScrubValue` |

## Related

- [Component registry](../../scene-and-ecs/component-registry/) — the registered components `inspect` enumerates
- [Built-in components](../../scene-and-ecs/built-in-components/) — the structs being edited
- [Asset pickers](../asset-pickers-and-drag-drop/) — the mesh/material uuid fields
- [Scene commands](../../tooling-and-control/scene-commands/) — `inspect` and the component edit commands
