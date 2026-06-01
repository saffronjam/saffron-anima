+++
title = 'Scene & ECS'
weight = 6
+++

# Scene & ECS

The game world is an entt registry of value components. The design choice worth knowing is
the component registry: a struct-of-closures table that teaches the serializer and the editor
about a component in one `registerComponent` call, with no central switch to edit.

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
