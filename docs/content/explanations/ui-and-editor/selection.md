+++
title = 'Selection'
weight = 9
+++

# Selection

There is one selected entity at a time, broadcast as a signal so any panel can react. You select by clicking a row in the [hierarchy](../hierarchy-panel/), clicking a light or camera billboard in the viewport, or ray-picking a mesh in empty viewport space. Clicking empty space deselects.

## Selection is a signal, not a poll

The selected entity lives on the editor context, and changing it goes through one function that also publishes a signal:

```cpp
void setSelection(EditorContext& ctx, Entity entity)
{
    ctx.selected = entity;
    ctx.onSelectionChanged.publish(entity);
}
```

`onSelectionChanged` is a `SubscriberList<Entity>`, the engine's signal/slot type. Anything that cares about the selection subscribes to it instead of reading `ctx.selected` directly. Every source — hierarchy click, billboard click, ray-pick, delete-clears-selection — funnels through `setSelection`, so there is exactly one place selection changes and one place it's announced.

## Three ways to select in the viewport

The viewport offers three pointer interactions, resolved in priority order each frame:

1. **Billboards.** `drawEditorBillboards` projects each light and camera entity's world position to screen space and draws an icon. A click inside an icon's box returns that entity, and the selected icon tints gold so the current selection stays visible even for entities with no mesh.

2. **Mesh ray-pick.** If no billboard was hit, a left-click in the hovered viewport ray-picks the nearest mesh, but only when the gizmo isn't being interacted with:

   ```cpp
   if (billboardHit.handle == entt::null &&
       viewportHovered(app.ui) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
       !ImGuizmo::IsOver() && !ImGuizmo::IsUsing())
   {
       ... // build NDC from the click, then pickEntity
       setSelection(*state->editor, pickEntity(scene, assets, renderer, cam, ndc));
   }
   ```

   The `ImGuizmo::IsOver()` / `IsUsing()` guards stop a click on a gizmo handle from reading as a pick-the-empty-space, which would deselect the very thing you're dragging.

3. **Deselect.** `pickEntity` returns a null entity on a miss, and `setSelection` takes it as-is, so clicking empty space clears the selection.

## Ray-pick lives in Assets

`pickEntity` builds a camera ray from the click's NDC and tests it against each entity's world-space mesh AABB, keeping the nearest hit. It lives in `Saffron.Assets` because it needs the GPU mesh bounds the asset server caches, which the editor module has no access to. The ray cast, the AABB slab test, and the Vulkan-clip caveat are covered in [Picking](../../scene-and-ecs/picking/).

## In the code

| What | File | Symbols |
|---|---|---|
| The selection signal | `editor_context.cpp` | `setSelection`, `onSelectionChanged` |
| Selection state | `editor_context.cppm` | `EditorContext::selected`, `SubscriberList<Entity>` |
| Billboard hit-test | `editor_gizmo.cpp` | `drawEditorBillboards`, `glm::project` |
| Click → pick wiring | `editor_app.cppm` | the `onUi` pick block + gizmo guards |
| The ray cast | `assets.cppm` | `pickEntity` |

## Related

- [Picking](../../scene-and-ecs/picking/) — the ray + AABB math behind click-select
- [Hierarchy panel](../hierarchy-panel/) — selection by list click
- [Gizmo](../gizmo/) — why the pick is guarded while the gizmo is active
- [Signals and slots](../../core-and-conventions/signals-and-slots/) — the `SubscriberList` type
