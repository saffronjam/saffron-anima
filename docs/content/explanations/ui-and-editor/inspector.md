+++
title = 'Inspector'
weight = 6
+++

# Inspector

The Inspector shows the selected entity's components and lets you edit, add, and remove them. The panel knows nothing about any specific component — it iterates [the component registry](../../scene-and-ecs/component-registry/) and asks each row to draw itself. Adding a new component type needs zero inspector code, which is the whole point: a component's UI lives next to its data, not in a central editor switch.

## A loop over the registry

The panel walks the registry's rows. For each component the selected entity has, it draws a collapsing header and calls the component's own draw closure:

```cpp
for (const ComponentTraits& traits : ctx.registry.rows)
{
    if (!traits.has(ctx.scene, selected)) { continue; }
    ImGui::PushID(static_cast<int>(traits.id));
    const bool open = propertyGridHeader(traits.name);
    if (traits.removable && ImGui::BeginPopupContextItem())
    {
        if (ImGui::MenuItem("Remove component")) { toRemove = &traits; }
        ImGui::EndPopup();
    }
    if (open)
    {
        traits.drawInspector(ctx.scene, selected);
        ImGui::TreePop();
    }
    ImGui::PopID();
}
```

`traits.drawInspector` is the per-component UI lambda registered alongside its serializers. The panel never `#include`s a component header or branches on a type — `has`, `name`, `removable`, `drawInspector`, and `remove` are the entire vocabulary it uses.

## The draw closures

The concrete closures are registered in `registerBuiltinComponents`. Each is a small slice of ImGui editing the live component by reference. Transform uses the colored three-axis `vec3Control` and converts the stored Euler radians to degrees for editing:

```cpp
TransformComponent& t = getComponent<TransformComponent>(s, e);
vec3Control("Translation", &t.translation.x);
glm::vec3 degrees = glm::degrees(t.rotation);
if (vec3Control("Rotation", &degrees.x)) { t.rotation = glm::radians(degrees); }
vec3Control("Scale", &t.scale.x, 1.0f);
```

Material edits base color, metallic, roughness, emissive, and an unlit toggle, plus an albedo [asset picker](../asset-pickers-and-drag-drop/). Lights edit color, intensity, range, and angles. These are plain ImGui calls writing straight into the component — there's no commit step because the component *is* the model.

## Remove and add

Removal is deferred the same way the [hierarchy](../hierarchy-panel/) defers deletes: the right-click "Remove component" records a pointer and the actual `remove` runs after the loop, so the registry isn't mutated mid-iteration. A row is only removable if it registered as such — `NameComponent` and `TransformComponent` pass `false`, so the inspector won't strip an entity of its identity or its place in the world.

Adding works off the same registry. The "Add Component" popup lists every registered component the entity doesn't already have:

```cpp
for (const ComponentTraits& traits : ctx.registry.rows)
{
    if (!traits.has(ctx.scene, selected) && ImGui::MenuItem(traits.name.c_str()))
        traits.addDefault(ctx.scene, selected);
}
```

`addDefault` default-constructs and attaches the component, which then appears in the loop above on the next frame.

## In the code

| What | File | Symbols |
|---|---|---|
| The generic panel | `editor_panels.cpp` | `inspectorPanel`, `ctx.registry.rows` |
| Per-component draw closures | `editor_components.cpp` | `registerBuiltinComponents`, `drawInspector` lambdas |
| Section header + 3-axis control | `ui.cppm` | `propertyGridHeader`, `vec3Control` |
| Add/remove plumbing | `editor_panels.cpp` | `addDefault`, `remove`, `removable` |

## Related

- [Component registry](../../scene-and-ecs/component-registry/) — the itable the panel reads
- [Built-in components](../../scene-and-ecs/built-in-components/) — the structs being edited
- [Asset pickers](../asset-pickers-and-drag-drop/) — the mesh/material fields
- [Hierarchy panel](../hierarchy-panel/) — the same deferred-mutation pattern
