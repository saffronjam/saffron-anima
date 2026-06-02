+++
title = 'Scene & ECS'
weight = 6
+++

# Scene & ECS

The scene is the game world, modelled as an entt registry of value components. At its centre is the
component registry, a struct-of-closures table that describes a component to the serializer and the
editor through one `registerComponent` call. No central switch needs editing when a component is
added.

## Pages

| Page | Covers | Code |
|---|---|---|
| `ecs-architecture` | entt `Scene`/`Entity`, value components, `forEach` | `scene.cppm` |
| `built-in-components` | Id, Name, Transform, Mesh, Material, Camera, the three light types | `scene.cppm` |
| `transform-and-matrices` | `TransformComponent` (Euler XYZ radians), matrix composition | `scene.cppm` · `transformMatrix` |
| `component-registry` | the closures itable, `registerComponent<C>`, lookup by name/id | `scene.cppm` · `ComponentRegistry` |
| `scene-serialization` | registry-driven JSON save/load, UUID stability | `scene.cppm` |
| `asset-catalog-in-scene` | `AssetCatalog` lives here; `Scene` borrows a `const AssetCatalog*` | `scene.cppm` · `AssetCatalog` |
| `picking` | ray vs. world-space mesh AABB, click-to-select | `assets.cppm` · `pickEntity` |
