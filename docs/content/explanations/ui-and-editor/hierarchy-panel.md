+++
title = 'Hierarchy panel'
weight = 5
+++

# Hierarchy panel

The Hierarchy panel lists every entity in the scene and lets you create, copy, delete, and select them. It is a flat list today (the scene has no parenting yet), which keeps it to a single `forEach` over the entities that carry an id and a name.

## Listing entities

The panel iterates entities that have both an `IdComponent` and a `NameComponent`, drawing each as a selectable row. Clicking a row calls `setSelection`, which updates `ctx.selected` and publishes the [selection signal](../selection/) so the inspector and the viewport highlight follow along. The `PushID` keyed on the entity's stable id keeps ImGui's per-item state correct even when names collide.

## Adding entities

An "Add +" button opens a preset popup that mirrors the Create menu: Empty, Model, and the lights and camera. Each preset creates the entity, adds the right component, nudges its transform where useful, and selects it:

```cpp
if (ImGui::MenuItem("Point Light"))
{
    Entity e = createEntity(ctx.scene, "Point Light");
    addComponent<PointLightComponent>(ctx.scene, e);
    getComponent<TransformComponent>(ctx.scene, e).translation = glm::vec3(0.0f, 2.0f, 0.0f);
    setSelection(ctx, e);
}
```

"Model" spawns the bundled cube through `ctx.onCreateCube`, a closure the client wires in. The editor has no `AssetServer` to resolve and upload a mesh itself, so anything that touches GPU assets is delegated to the client this way.

## Deferred structural changes

Copy and delete are reached through a right-click context popup on each row. Both would be unsafe mid-iteration — destroying an entity while `forEach` walks the entt storage can invalidate the iteration — so the panel records the target and applies the change after the loop:

```cpp
Entity toDelete{ entt::null };
Entity toCopy{ entt::null };
forEach<IdComponent, NameComponent>(ctx.scene, [&](Entity entity, ...) { ...
    if (ImGui::MenuItem("Copy"))   { toCopy   = entity; }
    if (ImGui::MenuItem("Delete")) { toDelete = entity; }
});
// applied after the iteration
```

This is the standard ImGui pattern: collect the intent during the draw, mutate the model once the draw is done.

## Copy is registry-driven

Copying an entity doesn't enumerate component types by hand. It walks [the component registry](../../scene-and-ecs/component-registry/) and, for each component the source has, adds a default to the clone and replays the source's serialized form into it:

```cpp
for (const ComponentTraits& t : ctx.registry.rows)
{
    if (t.has(ctx.scene, toCopy))
    {
        t.addDefault(ctx.scene, fresh);
        static_cast<void>(t.deserialize(ctx.scene, fresh, t.serialize(ctx.scene, toCopy)));
    }
}
```

Round-tripping through serialize/deserialize gives a deep copy without a per-component copy function — the same serialize/deserialize the scene file uses. Add a new component type with one `registerComponent` call and copy supports it for free. Delete clears the selection first if the deleted entity was selected, then calls `destroyEntity`.

## In the code

| What | File | Symbols |
|---|---|---|
| The panel | `editor_panels.cpp` | `hierarchyPanel` |
| Selection on click | `editor_panels.cpp` | `setSelection`, `ctx.selected` |
| Add presets | `editor_panels.cpp` | the "Add +" popup, `ctx.onCreateCube` |
| Deferred copy/delete | `editor_panels.cpp` | `toCopy`/`toDelete`, `ctx.registry.rows` |

## Related

- [Inspector](../inspector/) — what shows for the selected entity
- [Selection](../selection/) — the `SubscriberList<Entity>` selection signal
- [Component registry](../../scene-and-ecs/component-registry/) — what copy iterates
