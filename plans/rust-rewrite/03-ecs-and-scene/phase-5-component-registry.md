# Phase 5 — The component registry (the `std::function` itable → a fn-pointer table)

**Status:** COMPLETED

**Depends on:** 03-ecs-and-scene:phase-4-hierarchy-and-transform-math

## Goal

Port the `ComponentRegistry` / `ComponentTraits` reflection layer and the `register_component::<C>`
generator, plus the component-order machinery built on top. This is the table that drives
serialize/deserialize, `add-component`/`remove-component`, and `present_component_names` — without it a
component "silently never serializes." Serde *bodies* are phase-6; this phase delivers the table shape,
the registration generator, and the order logic, with placeholder/passthrough serde fn-pointers so it
compiles and tests structurally.

## Why this shape (NO LEGACY)

- **`ComponentTraits` (a struct of `std::function`) → a struct of fn-pointers.** PP-1 maps a "per-type
  registration record keyed by type" to "a registration table of fn-pointers/`Box<dyn Fn>` keyed by
  type." Each trait closure is monomorphic over `C`, so plain `fn(...)` pointers suffice — no captures, no
  `Box<dyn Fn>` allocation. `register_component::<C>()` generates them from the generic
  `add/get/has/remove` exactly as the C++ template does (`scene.cppm:1301`), so adding a component is
  still one call. The `id` field (entt `type_hash`) becomes `TypeId::of::<C>()` — Rust's stable
  in-process type identity, the direct analogue of the `storage()` join key.
- **`drawInspector` is deleted (NO LEGACY).** The C++ field is always a no-op closure in the headless host
  (the inspector is the React editor; `scene_edit_components.cpp` passes `[](Scene&, Entity) {}`
  everywhere). It carries zero behavior, so the Rust `ComponentTraits` drops it entirely — the registry is
  purely serialize/deserialize/has/add/remove/copy + name/removable.
- **`serializeEntity` is re-architected off the `storage()` walk.** The C++ walks `registry.storage()`
  and joins each storage id to a registry row by `type_hash` (`scene.cppm:1494`) — relying on an ECS
  storage-introspection API that `hecs` does not portably expose. The Rust port instead walks the
  **registry rows** and calls `traits.has(scene, entity)`, emitting `{ name: serialize(...) }` for each
  present row. Same output object, no `type_hash` join, no ECS-storage dependency — the explicit
  re-architecture PP-4 names. Row order is the registration order, which the C++ `storage()` walk did not
  guarantee but `componentOrder` did; the doc-level ordering is owned by `component_order`, so emit order
  here is incidental (the document carries `componentOrder` explicitly).
- **One registration site, enforced by a test.** `register_builtin_components` (phase-8) is the only
  place; PP-7 decides whether it is a derive/`inventory` collection or the explicit 24-call list. This
  phase adds a **registry-completeness `#[test]`**: every component type intended to serialize is present
  in the registry by name. The C++ failure mode ("miss the registration → silent no-serialize") becomes a
  failing test, not a silent gap.
- **Component-order logic ported faithfully.** `component_order` / `set_component_order` /
  `sort_component_order` / `append_component_order` / `remove_component_order` reconcile a stored order
  against the canonical present set (drop absent, append new, dedup), and `present_component_names`
  filters out `Relationship` and `Bone` (`scene.cppm:1357`–1487). These drive the editor's component
  panel ordering and round-trip through the scene document.

## Grounding (real files / symbols)

- `engine-old/source/saffron/scene/scene.cppm`: `ComponentTraits` (1209), `ComponentRegistry` (1224),
  `registerComponent<C>` (1301), `findById`/`findByName` (1337/1347), `presentComponentNames` (1357),
  `canonicalComponentOrder`/`componentOrder`/`setComponentOrder`/`appendComponentOrder`/
  `removeComponentOrder`/`sortComponentOrder` (1370–1487), `serializeEntity` (1491), `deserializeEntity`
  (1510).
- `scene/AGENTS.md`: "Register it once… miss step 3 and the component silently never serializes"; the
  `registerComponent` calls in `scene.cppm` are self-tests only, the real site is
  `scene_edit_components.cpp`.

## Acceptance gate

- Cargo workspace compiles; `ComponentRegistry`/`ComponentTraits`/`register_component::<C>` exist with
  fn-pointer traits and no `draw_inspector` field.
- `cargo test -p saffron-scene`:
  - registration round-trip: register a couple of types, `find_by_name`/`find_by_id` resolve them,
    `present_component_names` filters `Relationship`/`Bone`.
  - component-order: `set_component_order` rejects a wrong-length / non-present / duplicate list;
    `component_order` reconciles a stale order against the present set; `sort_component_order` yields the
    canonical order.
  - `serialize_entity` walks rows (not ECS storage) and produces a `{name: value}` object for present
    components only; unregistered components (Id/WorldTransform/PoseOverride) are absent.
  - registry-completeness: every intended serialized component is registered (the anti-silent-gap test).
- Workspace build green; prior phases still pass.
