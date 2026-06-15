+++
title = 'Add a component'
weight = 4
math = false
+++

# Add a component

Add a new ECS component type. A single `registerComponent` call wires up serialization, the CLI, and the Inspector.

## Steps

1. Declare the value struct in `scene.cppm` — a plain struct, registered with entt like the existing ones.
2. Register it once in `registerBuiltinComponents` (`scene_edit_components.cpp`) with four things: the stable name, a draw closure, a `toJson`, and a `fromJson`:
   ```cpp
   registerComponent<HealthComponent>(reg, "Health",
       [](Scene&, Entity) {},
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
   The draw closure is empty: the engine renders no UI. The Inspector is the React/Tauri frontend, which builds each field from the DTO catalog over the control plane. The two closures that matter are `toJson`/`fromJson`, which carry the component's fields on the wire and to disk. `registerComponent` synthesizes `has` / `addDefault` / `remove` / `copyTo` / `serialize` / `deserialize` from the generic helpers. The last argument is `removable`, which is `false` for always-present types like Name and Transform.
3. Rebuild with `cmake --build build/debug -j1`.

The stable name is the JSON key, the Inspector header, and the CLI token. Keep it consistent across all three.

## Verify

- The type shows up: `sa list-components`.
- Set it on an entity and read it back:
  ```sh
  sa add-component MyEntity Health
  sa set-component MyEntity Health --json '{"hp":42}'
  sa inspect MyEntity
  ```
- The Inspector shows it (with fields derived from the DTO catalog) under **Add Component**, and saving the scene serializes it under `"Health"`.

## In the code

| What | File | Symbols |
|---|---|---|
| `registerComponent` template | `scene.cppm` | `registerComponent`, `ComponentTraits`, `ComponentRegistry` |
| Where built-ins register | `scene_edit_components.cpp` | `registerBuiltinComponents` |
| Generic add/get/has/remove | `scene.cppm` | `addComponent`, `getComponent`, `hasComponent` |
| CLI add/set/inspect | `control_commands_scene.cpp` | `add-component`, `set-component`, `inspect` |

## Related

- [Component registry](../../explanations/scene-and-ecs/component-registry/)
- [ECS architecture](../../explanations/scene-and-ecs/ecs-architecture/)
- [Scene serialization](../../explanations/scene-and-ecs/scene-serialization/)
