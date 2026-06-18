# Phase 1 — `saffron-scene` crate skeleton + the ECS adapter

**Status:** COMPLETED

**Depends on:** 00-foundations:phase-2-core-crate, 00-foundations:phase-4-json-crate

## Goal

Stand up the `saffron-scene` crate with the wrapped ECS world, the `Entity` newtype, and the
component-access surface (`add_component` / `get_component` / `get_component_mut` / `has_component` /
`remove_component` / `valid` / `for_each` / `create_entity` / `destroy_entity` / `find_entity_by_uuid`),
parameterized over a single internal ECS choice (default `hecs`, locked by phase-2's gate). No
components, no serde, no hierarchy yet — just the world model and the access functions, compiling green
with a smoke test. This is the seam that keeps the ECS crate an internal detail: every later phase and
every downstream crate talks to these wrappers, never to `hecs::` directly.

## Why this shape (NO LEGACY)

- **The ECS crate is wrapped, not exposed.** The C++ `Entity` is "a bare entt handle" but every consumer
  goes through `sa::` free functions — only `scene.cppm` itself names `entt::`. The Rust port preserves
  that property exactly: `pub struct Entity(EcsEntity)` (a `Copy` newtype) and a `World` wrapper inside
  `Scene`, so no downstream crate names `hecs::Entity`/`hecs::World`. This is what makes the phase-2
  fallback (swap to `bevy_ecs`) a one-crate change rather than a tree-wide churn.
- **`Scene` keeps the C++ field shape, with the borrowed catalog made safe.** `Scene` is
  `{ world, environment, catalog }`. The C++ `catalog` was a borrowed raw `const AssetCatalog*` set
  per-frame (`scene.cppm:582`); it becomes `Option<Arc<AssetCatalog>>` (PP-1 Ref bucket 1, read-shared,
  never serialized) so there is no dangling-pointer or lifetime tangle — the asset layer hands the scene
  a shared handle. No second "scene with no catalog" type; one `Scene`, one optional field.
- **Access functions are methods where they read naturally (PP-1 drops the free-function dogma).** The
  C++ `addComponent(scene, entity, args)` becomes `scene.add_component::<C>(entity, c)` etc. The
  *generic-over-component-tuple* shape of `forEach<C...>` is kept; the callback still receives
  `(Entity, &mut C…)`. This is idiomatic and reads like the engine intended.
- **No self-test function.** The C++ ships `runSceneHierarchySelfTest`/`runSceneSerializationSelfTest`
  as runtime functions; this crate has none — a smoke `#[cfg(test)]` replaces the startup self-test
  (PP-1: no in-engine self-test functions survive).

## Grounding (real files / symbols)

- `engine-old/source/saffron/scene/scene.cppm`: `Scene` (line 578), `Entity` (588), `valid` (593),
  `addComponent`/`getComponent`/`hasComponent`/`removeComponent` (600–622), `createEntity` (624),
  `destroyEntity` (637), `forEach<C...>` (730), `findEntityByUuid` (742). The whole entt surface is
  `registry.view` (733), `valid`, `all_of`, `get`, `try_get`, `emplace`, `emplace_or_replace`
  (`animation.cpp:754`), `remove`, `destroy`, `create`, `clear` — nothing else.
- PP-1 contract: `saffron-scene` depends on `{saffron-core, saffron-json}`; `Entity` is `Copy`; the ECS
  crate identity is deferred to PP-4 (this phase, phase-2).

## Acceptance gate

- Cargo workspace compiles; `saffron-scene` builds with `#![deny(unsafe_code)]`.
- `cargo test -p saffron-scene` passes a smoke test: create N entities, `valid` each, `for_each` counts
  them, `destroy_entity` removes one (subtree handling lands in phase-4), `find_entity_by_uuid` resolves a
  known id and returns null for an absent one.
- No `hecs::`/`bevy_ecs::` type appears in the crate's public API (a `cargo doc`/grep check, or a doc note
  asserting it).
- `make engine`-equivalent (the workspace `cargo build`) is green; everything built so far still passes.
