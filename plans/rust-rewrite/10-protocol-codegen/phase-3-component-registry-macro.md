# Phase 3 — The `register_component!` macro and the one-place registration discipline

**Status:** COMPLETED

**Depends on:** 10-protocol-codegen:phase-1-dto-crate-and-derives, 03-ecs-and-scene:phase-5-component-registry

## Goal

Provide the **mechanism** that collapses the C++ four-place component-registration hand-sync to one
ordered place: a `register_component!` declarative macro that expands a single
`register_component!(reg, NameComponent, "Name", to_json, from_json)` line into the fn-pointer
`ComponentTraits` row area 03 phase-5 defined, and the ordered `register_builtin_components` discipline
the codegen and runtime both honor. The registry *table shape* and the serde *bodies* belong to area 03;
this phase owns only the macro and the ordering contract, with the registry-completeness tripwire.

## Why this shape (NO LEGACY)

- **The four-place hand-sync is the trap `gen.ts` exists to mitigate.** `scene/AGENTS.md`: "Register it
  once ... miss step 3 and the component silently never serializes." The C++ already collapsed the closure
  boilerplate into the `registerComponent<C>` template (`scene.cppm:1301`), but registration is still a
  loose call list in `scene_edit_components.cpp` and the serde lives in the hand-maintained
  `scene_component_serde.generated.cpp`. The Rust design makes **one macro line per component** the entire
  registration, with the serde supplied inline (the `to_json`/`from_json` from area 03 phase-6).
- **A declarative macro over an explicit ordered list — NOT `inventory`.** Registration order is
  load-bearing twice over: it is the `componentOrder` canonical order (`scene.cppm:1370`) and it is the
  order the OpenRPC/manifest emitters and `help` iterate. `inventory`'s collection order is link-order
  defined and not guaranteed stable across builds, which would silently reorder the wire. So
  `register_builtin_components` stays an explicit ordered sequence of `register_component!` calls — one
  place, deterministic order. The macro removes the per-call closure boilerplate; it does not hide the
  order.
- **`register_component!` generates the fn-pointers, mirroring the C++ template.** Each closure in
  `ComponentTraits` is monomorphic over `C` (no captures), so the macro expands to plain `fn` pointers for
  `has`/`add_default`/`remove`/`copy_to` and to the supplied `to_json`/`from_json` fn paths for
  serialize/deserialize — exactly the synthesis `registerComponent<C>` does (`scene.cppm:1310`–1329), minus
  the deleted `drawInspector` (always a no-op in the headless host; PP-4 drops it).
- **The completeness tripwire replaces the silent-no-serialize failure mode.** Area 03 phase-5 already
  specifies a registry-completeness `#[test]`; this phase makes the macro the thing that test counts, so
  "added a component DTO but forgot to register it" is a failing test (the C++ silent gap becomes loud).
- **Why this is PP-7's and not purely area 03's:** the pre-plan assigns PP-7 the "component-registry macro
  replacing `scene_component_serde.generated.cpp`" and "the inventory/registration-macro discipline." Area
  03 owns the table and the bodies; PP-7 owns the *single-registration mechanism* shared in spirit with the
  command table (phase-4) — one macro/list discipline, two collection sites.

## Grounding (real files / symbols)

- `engine-old/source/saffron/scene/scene.cppm`: `registerComponent<C>` (1301, the template the macro
  mirrors), `ComponentTraits` (1209), `serializeEntity` (1491, the registry-row walk the order feeds).
- `engine-old/source/saffron/sceneedit/scene_edit_components.cpp`: `registerBuiltinComponents` and the 24
  `registerComponent<C>(reg, "Name", drawFn, toJson, fromJson)` calls (20–138) — the ordered list the macro
  list reproduces.
- `engine-old/source/saffron/scene/scene_component_serde.generated.cpp`: the hand-maintained `*ToJson`/
  `*FromJson` bodies (area 03 phase-6 supplies the Rust equivalents the macro consumes).
- `scene/AGENTS.md`: the "register once or it silently never serializes" rule the completeness test guards.

## Acceptance gate

- `cargo build --workspace` succeeds; clippy + fmt clean; `#![deny(unsafe_code)]` holds.
- `register_component!(reg, C, "Name", to_json, from_json [, removable])` expands to a `ComponentTraits`
  row with `has`/`add_default`/`remove`/`copy_to` fn-pointers + the supplied serde, no `drawInspector`
  field; a `#[test]` registers two stub components via the macro and asserts the rows carry the right
  names, `removable` flags, and a working `has`/`serialize` round-trip.
- `register_builtin_components` is a single ordered function (the only registration site); a
  **registry-completeness `#[test]`** asserts the registered name set equals the intended 24-component set
  in the C++ registration order (the `componentOrder` canonical order), so a missing registration fails.
- A `#[test]` asserts registration order is stable across builds (the macro list, not `inventory`) by
  checking the row order equals the source-list order.
