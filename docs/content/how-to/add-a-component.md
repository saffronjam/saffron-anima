+++
title = 'Add a component'
weight = 4
math = false
+++

# Add a component

Add a new ECS component type. One `registerComponent` call wires up serialization, the CLI, and the Inspector.

## Steps

1. Declare the value struct in `scene.cppm` (a plain struct, registered with entt like the existing ones).
2. Register it once in `registerBuiltinComponents` (`editor_components.cpp`) with four things: the stable name, an ImGui draw closure, a `toJson`, and a `fromJson`:
   ```cpp
   registerComponent<HealthComponent>(reg, "Health",
       [](Scene& s, Entity e)
       {
           HealthComponent& h = getComponent<HealthComponent>(s, e);
           ImGui::DragFloat("HP", &h.hp, 1.0f, 0.0f, 1000.0f);
       },
       [](const HealthComponent& c) -> nlohmann::json
       {
           return nlohmann::json{ { "hp", c.hp } };
       },
       [](HealthComponent& c, const nlohmann::json& j) -> Result<void>
       {
           c.hp = jsonF32Or(j, "hp", 100.0f);
           return {};
       },
       true);  // removable
   ```
   `registerComponent` synthesizes `has` / `addDefault` / `remove` / `copyTo` / `serialize` / `deserialize` from the generic helpers. The last arg is `removable` (`false` for always-present types like Name and Transform).
3. Rebuild: `cmake --build build/debug -j1`.

The stable name is the JSON key, the Inspector header, and the CLI token. Keep it consistent.

## Verify

- The type shows up: `se list-components`.
- Set it on an entity and read it back:
  ```sh
  se add-component MyEntity Health
  se set-component MyEntity Health --json '{"hp":42}'
  se inspect MyEntity
  ```
- The Inspector shows it (with your draw closure) under **Add Component**, and saving the scene serializes it under `"Health"`.

## In the code

| What | File | Symbols |
|---|---|---|
| `registerComponent` template | `scene.cppm` | `registerComponent`, `ComponentTraits`, `ComponentRegistry` |
| Where built-ins register | `editor_components.cpp` | `registerBuiltinComponents` |
| Generic add/get/has/remove | `scene.cppm` | `addComponent`, `getComponent`, `hasComponent` |
| CLI add/set/inspect | `control_commands_scene.cpp` | `add-component`, `set-component`, `inspect` |

## Related

- [Component registry](../../explanations/scene-and-ecs/component-registry/)
- [ECS architecture](../../explanations/scene-and-ecs/ecs-architecture/)
- [Scene serialization](../../explanations/scene-and-ecs/scene-serialization/)
