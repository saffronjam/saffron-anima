# Phase 4 — The component read/write bridge over the registry serde

**Status:** COMPLETED

**Depends on:** 12-scripting:phase-3-session-guard-and-entity-handle, 03-ecs-and-scene:phase-5-component-registry

## Goal

Bind `get_component` / `set_component` / `add_component` / `remove_component` / `has_component` on
`sa.Entity` over the scene component registry's type-erased serde — every registered component reachable
with zero per-type code — and enforce the structural-component write gate. This is the JSON↔Lua bridge
(`jsonToLua` / `luaToJson`) plus the registry lookups.

## Why this shape (NO LEGACY)

- **Type-erased over the registry, not per-component.** C++ resolved a `ComponentTraits*` by name from
  the `ComponentRegistry`, then called `traits->serialize`/`deserialize`/`has`/`addDefault`/`remove`
  (`script_runtime.cpp:302`–397). The Rust registry (area 10 phase-3 macro / area 03 registry) exposes
  the same type-erased `find_by_name` + serialize/deserialize/has/add_default/remove fn-pointer table,
  so the script bridge is one path for all components. `get_component` returns the serialized
  `serde_json::Value` converted to a Lua table; `set_component` converts a Lua table → `Value` →
  `deserialize` (a merge onto the live component — partial patches work).
- **`jsonToLua` / `luaToJson` port as total conversions.** `jsonToLua` (`script_runtime.cpp:46`–87):
  objects→tables, arrays→1-based tables, uuids stay decimal strings, null→nil, with the C++
  large-unsigned→f64 fallback. `luaToJson` (`script_runtime.cpp:92`–157): a `sa.Vec3` userdata →
  `{x,y,z}` object (the shape `vec3_from_json` reads), a string-keyed table → object, a 1-based sequence
  → array, scalars 1:1, else null. In Rust these are conversions between `mlua::Value` and
  `serde_json::Value`; the `sa.Vec3` userdata special case reads `x`/`y`/`z` off the userdata.
- **The structural-component gate is a fixed set, refused on write.** `set_component`/`add_component`
  refuse the cache/asset-backed structural components (`Relationship`, `SkinnedMesh`, `Bone`, `FootIk`,
  `BonePhysics`, `Collider`, `Rigidbody`, `KinematicBones`) with a logged `false` — a mid-play write
  would desync the live Jolt world / rig caches / hierarchy (`isStructuralComponent`,
  `script_runtime.cpp:163`–169,327,358). This ports as a `const` `&[&str]` set checked before the
  registry call. `remove_component` additionally honors the registry's `removable` flag.
- **Unknown name / deserialize failure is a logged `false`, never an error.** The contained-fault
  contract: an unknown component or a failed `deserialize` logs and returns `false`/`nil`, never aborts
  the tick (`script_runtime.cpp:332`–348).

## Grounding (real files / symbols)

- `engine-old/source/saffron/script/script_runtime.cpp`: `jsonToLua` (46–87), `luaToJson` (92–157),
  `kStructuralComponents` + `isStructuralComponent` (163–169), `getComponentSnapshot` (302–315),
  `setComponent` (320–349), `addComponent` (351–370), `removeComponent` (372–386), `hasComponent`
  (388–397).
- 03-ecs-and-scene component registry: `ComponentTraits` row (`has`/`serialize`/`deserialize`/
  `add_default`/`remove`/`removable`), `find_by_name`.
- 02-math-and-geometry / 03: `vec3_from_json` (the `{x,y,z}` shape the Vec3→object case must match).

## Acceptance gate

- `cargo build --workspace` succeeds; `#![deny(unsafe_code)]`; clippy + fmt clean.
- `#[test]`: `e:get_component("Transform")` returns a table with `translation`/`rotation`/`scale`
  `{x,y,z}` sub-tables matching the component's serde shape; `e:set_component("Transform", {...})` merges
  and the live component reflects it; a uuid field comes back as a decimal string.
- `#[test]`: `e:set_component("Collider", {...})` and `e:add_component("Rigidbody")` return `false` and
  log (structural gate); a non-structural `e:add_component("PointLight")` adds it and returns `true`;
  `e:remove_component` honors `removable`.
- `#[test]`: `e:get_component("NotARealComponent")` returns `nil`; `e:set_component` with a malformed
  table returns `false` and the tick continues.
- `#[test]`: round-trip — `set_component(get_component(x))` is idempotent on a non-structural component.
