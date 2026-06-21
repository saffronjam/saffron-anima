+++
title = 'ECS architecture'
weight = 1
+++

# ECS architecture

An entity-component-system (ECS) is a data-oriented architecture: game state is plain data in tight
storage, entities are identifiers that group components, and logic runs as functions over that data.
The layout suits a renderer that walks thousands of objects per frame.

The scene crate builds its world on `hecs` for the storage, but that choice is wrapped, never
exposed. `Scene` is a struct that owns a `hecs::World`; the world field is private, and every
operation downstream code performs goes through a method on `Scene`. No other crate ever names
`hecs::` directly, so swapping the storage backend (a future `bevy_ecs`, say) is a one-crate change.

## The world is a struct

`Scene` holds the `hecs::World`, the scene-wide [environment](../asset-catalog-in-scene/), and an
optional shared handle to the project's [asset catalog](../asset-catalog-in-scene/), so the
registry-driven inspector can resolve mesh and material ids to names. `Entity` is a copyable handle
that wraps the storage's generational id, so it never dangles against a relocated `Scene`; a handle
that outlives its entity is caught by `valid`.

```rust
pub struct Scene {
    world: hecs::World,
    pub environment: SceneEnvironment,
    pub catalog: Option<Arc<AssetCatalog>>,  // borrowed; set per-frame, never serialized
}

pub struct Entity(hecs::Entity);
```

`Entity::NULL` is the sentinel non-entity, used by the runtime caches that store a flat
`Vec<Entity>` where an unresolved slot needs a value rather than an `Option`. `valid` reports
`false` for it.

## Operations are methods on the scene

Component access is a set of generic methods on `Scene`, each bounded on `crate::Component` (the
re-exported storage trait, so callers never name the ECS):

```rust
impl Scene {
    pub fn add_component<C: Component>(&mut self, entity: Entity, c: C) -> Result<()>;
    pub fn has_component<C: Component>(&self, entity: Entity) -> bool;
    pub fn remove_component<C: Component>(&mut self, entity: Entity);
    pub fn with_component<C: Component, R>(&self, e: Entity, f: impl FnOnce(&C) -> R) -> Result<R>;
    pub fn with_component_mut<C, R>(&mut self, e: Entity, f: impl FnOnce(&mut C) -> R) -> Result<R>;
    pub fn component<C: Component + Copy>(&self, entity: Entity) -> Result<C>;
}
```

Reads are scoped to a borrow (`with_component` runs a closure against the component rather than
handing out a long-lived reference into the storage), and `component` is the convenience copy-out
for a small `Copy` component. A stale handle or a missing component is a typed
[`Error`](../scene-serialization/), never a panic.

`create_entity` mints a fresh entity already carrying the standard authored set: an `IdComponent`
with a new [`Uuid`](../scene-serialization/), a `Name`, a default `Transform`, a root
`Relationship`, and a `ComponentOrder`. `destroy_entity` removes it and its whole subtree.

## Iteration: for_each over a query

The one iteration primitive is `for_each`, generic over a `hecs` query tuple of component
references. It runs a callback for each entity that carries all of them:

```rust
pub fn for_each<Q, F>(&mut self, mut f: F)
where
    Q: Query,
    F: for<'a> FnMut(Entity, <Q as Query>::Item<'a>),
{
    for (handle, item) in self.world.query_mut::<(hecs::Entity, Q)>() {
        f(Entity(handle), item);
    }
}
```

The query tuple spells the access exactly: `for_each::<&Transform, _>` reads, `for_each::<(&Transform,
&mut Camera), _>` reads one and mutates the other. A system is just a function that calls `for_each`
with the components it cares about — `render_scene` walks `(&Transform, &Mesh)` to gather
renderables; `primary_camera` walks `(&Transform, &Camera)` to find the first primary camera and
inverts its world matrix into a view.

## Why this shape

`hecs` already provides archetype storage, queries, and generational handles. Exposing it directly
would bind every downstream crate to one ECS and scatter `hecs::` across the tree. Keeping the world
field private and the access a fixed method surface lets the same world feed the renderer, the
serializer, and the editor, with none of them owning a privileged ECS API, and keeps the backend
swap to this one crate. Per-component behavior lives in the [component registry](../component-registry/),
which is data, not a trait hierarchy.

## In the code

| What | File | Symbols |
|---|---|---|
| World + handle | `scene/src/scene.rs` | `Scene`, `Entity`, `Scene::valid` |
| Component access | `scene/src/scene.rs` | `add_component`, `has_component`, `remove_component`, `with_component`, `with_component_mut`, `component` |
| Lifecycle | `scene/src/scene.rs` | `create_entity`, `spawn_with_id`, `destroy_entity` |
| Iteration | `scene/src/scene.rs` | `for_each` |
| Re-exported storage traits | `scene/src/scene.rs` | `Component`, `Query` |
| Camera resolve | `scene/src/hierarchy.rs` | `primary_camera`, `CameraView`, `camera_projection` |
| A system that walks it | `assets/src/render_scene.rs` | `render_scene` |

## Related
- [Component registry](../component-registry/) — per-component behavior as data, not methods
- [Components](../built-in-components/) — the value structs `for_each` iterates
- [Go-flavored design](../../core-and-conventions/go-flavored-design/) — why structs + free-standing methods
