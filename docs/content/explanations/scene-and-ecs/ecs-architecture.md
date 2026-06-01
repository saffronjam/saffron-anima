+++
title = 'ECS architecture'
weight = 1
+++

# ECS architecture

An ECS keeps game state as plain data in tight storage and runs logic as functions over that
data, which suits a renderer that walks thousands of objects per frame. SaffronEngine uses entt
for the storage but drops the usual member-function ergonomics: there is no `Entity` class with
methods, no `Scene` class hiding the registry. The world is a struct you pass around, and
everything you do to it is a free function.

## The world is a struct

`Scene` is the entt `registry` plus a borrowed pointer to the project's
[asset catalog](../asset-catalog-in-scene/), so the registry-driven inspector can resolve mesh
and material ids to names. `Entity` is a one-field wrapper over `entt::entity` — a plain index,
copyable, with no back-pointer to the scene, so it can never dangle against a relocated `Scene`.

```cpp
struct Scene
{
    entt::registry registry;
    const AssetCatalog* catalog = nullptr;  // borrowed; set per-frame, not owned or serialized
};

struct Entity { entt::entity handle = entt::null; };
```

The scene is always passed explicitly. This is the Go habit of passing the world rather than
reaching for `this`.

## Operations are free functions

Component access is a set of generic free functions over `(scene, entity)`, not member templates
on a class:

```cpp
template <typename C, typename... Args>
auto addComponent(Scene& scene, Entity entity, Args&&... args) -> C&;

template <typename C> auto getComponent(Scene&, Entity) -> C&;
template <typename C> auto hasComponent(const Scene&, Entity) -> bool;
template <typename C> void removeComponent(Scene&, Entity);
```

`createEntity` mints a fresh entity already carrying the three components every entity has: an
`IdComponent` with a new [Uuid](../scene-serialization/), a `NameComponent`, and a
`TransformComponent`. `destroyEntity` removes it.

## Iteration: forEach over a view

The one iteration primitive is `forEach`, a thin wrapper over an entt view. You list the
component types you want and get a callback per matching entity:

```cpp
template <typename... C, typename Fn>
void forEach(Scene& scene, Fn&& fn)
{
    auto view = scene.registry.view<C...>();
    for (entt::entity handle : view)
        fn(Entity{ handle }, view.template get<C>(handle)...);
}
```

A system is just a function that calls `forEach` with the components it cares about.
`renderScene` calls `forEach<TransformComponent, MeshComponent>` to gather renderables;
`primaryCamera` calls `forEach<TransformComponent, CameraComponent>` to find the first primary
camera and invert its model matrix into a view.

## Why this shape

entt already gives sparse-set storage, views, and grouping. Wrapping it in a class hierarchy
would hide those and re-add the OOP the engine dropped. Keeping `Scene` a struct and operations
free functions lets the same registry feed the renderer, the serializer, and the editor with
none of them owning a privileged scene API. The per-component behavior lives in the
[component registry](../component-registry/), which is data, not inheritance.

## In the code

| What | File | Symbols |
|---|---|---|
| World + handle | `scene.cppm` | `Scene`, `Entity`, `valid` |
| Component access | `scene.cppm` | `addComponent`, `getComponent`, `hasComponent`, `removeComponent` |
| Lifecycle | `scene.cppm` | `createEntity`, `destroyEntity` |
| Iteration | `scene.cppm` | `forEach` |
| Camera resolve | `scene.cppm` | `primaryCamera`, `CameraView`, `cameraProjection` |
| A system that walks it | `assets.cppm` | `renderScene` |

## Related
- [Component registry](../component-registry/) — per-component behavior as data, not methods
- [Built-in components](../built-in-components/) — the value structs `forEach` iterates
- [Go-flavored design](../../core-and-conventions/go-flavored-design/) — why structs + free functions
