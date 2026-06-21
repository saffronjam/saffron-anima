+++
title = 'Component registry'
weight = 4
+++

# Component registry

A component registry is a runtime table that pairs each component type with the structural
operations cross-cutting features perform on it: serialize, deserialize, add, remove, and clone.
Each entry is a row of function pointers, so a feature dispatches on a component without naming its
type.

Several subsystems need per-component knowledge. The serializer converts a component to and from
JSON, the editor adds and removes it over the control plane, and the play-mode duplicate copies it
across worlds. A registry holds that knowledge in one place. Registering a component is one macro
line, and no central code changes when a new component is added.

## The itable

`ComponentTraits` is a struct of plain `fn` pointers — a Go-interface itable built by hand, one
field per operation a cross-cutting feature needs. Each pointer is monomorphic over the component
type and captures nothing, so the whole row is `Copy`:

```rust
pub struct ComponentTraits {
    pub name: &'static str,                                   // stable JSON key + UI header
    pub removable: bool,
    pub has: fn(&Scene, Entity) -> bool,
    pub add_default: fn(&mut Scene, Entity),
    pub remove: fn(&mut Scene, Entity),
    pub copy_to: fn(&Scene, Entity, &mut Scene, Entity),      // clone src -> dst
    pub serialize: fn(&Scene, Entity) -> Value,
    pub deserialize: fn(&mut Scene, Entity, &Value) -> Result<()>,
}
```

The `ComponentRegistry` is a vector of these rows plus two indexes: `by_id` (keyed by
`TypeId::of::<C>()`, Rust's stable in-process type identity) and `by_name` (keyed by the stable JSON
string). Both map to the same row. There is no per-type draw hook — the inspector is the React
editor, which builds each field from the DTO catalog over the control plane.

## Registering is one macro line

`ComponentRegistry::register::<C>` synthesizes the structural pointers (`has`, `add_default`,
`remove`, `copy_to`) from the generic component access and takes the two serde trampolines as bare
`fn` pointers. The `register_component!` macro is the one-line registration *surface* over it: it
expands to a single `register` call, building the serde trampolines from the supplied (or defaulted)
`to_json` / `from_json` paths.

```rust
register_component!(reg, Transform, "Transform", false);   // serde defaults to the SceneSerialize impl
register_component!(reg, Mesh, "Mesh");                     // removable defaults to true
```

When the serde paths are omitted they default to the type's `SceneSerialize` impl, the
byte-compatible body every built-in component carries. The deserialize trampoline default-constructs
the component if absent, then fills it in place, so a load never assumes the component already
exists. Every built-in is registered this way in `register_builtin_components`, one line each.

## Lookup feeds both directions

```rust
pub fn find_by_id(&self, id: TypeId) -> Option<&ComponentTraits>;
pub fn find_by_type<C: Component>(&self) -> Option<&ComponentTraits>;
pub fn find_by_name(&self, name: &str) -> Option<&ComponentTraits>;
```

`serialize_entity` walks the registry rows, asks each `has(scene, entity)`, and writes
`{ name: serialize(...) }` for every present row. Loading reads JSON keys and calls `find_by_name`.
The two indexes let one table drive both the type-keyed and string-keyed paths.

## Why fn pointers, not a derive

The serde could ride a `#[derive]` or a reflection crate. The registry uses a hand-built
struct-of-fn-pointers for the reason the rest of the codebase avoids heavy machinery: it is plain,
debuggable data read top to bottom, and it keeps the per-component JSON body (one `SceneSerialize`
impl) next to the registration line rather than scattered across attributes. Because every pointer
captures nothing, the row stays `Copy` with no `Box<dyn Fn>` allocation.

The registration list is a deliberate explicit sequence, not a link-time collection: registration
order is the canonical `component_order` and the OpenRPC/manifest emit order, so
`register_builtin_components` is one function listing the calls in a fixed order, and a
`registry_is_complete` test pins the row set to `BUILTIN_COMPONENT_NAMES`.

> [!TIP]
> The `name` string is a stable contract, not a display nicety. It is the JSON key on disk and the
> editor's component header. Renaming it silently breaks every saved scene that used the old name
> (the loader logs `unknown component '<old>', skipping`). Treat it like a serialization version.

## In the code

| What | File | Symbols |
|---|---|---|
| The itable + table | `scene/src/registry.rs` | `ComponentTraits`, `ComponentRegistry` |
| One-line registration | `scene/src/registry.rs` · `scene/src/macros.rs` | `ComponentRegistry::register`, `register_component!` |
| Lookup | `scene/src/registry.rs` | `find_by_id`, `find_by_type`, `find_by_name` |
| Per-entity serde walk | `scene/src/registry.rs` | `serialize_entity`, `deserialize_entity` |
| The built-in registrations | `scene/src/registry.rs` | `register_builtin_components`, `BUILTIN_COMPONENT_NAMES` |
| Per-component serde bodies | `scene/src/serde.rs` | `SceneSerialize` |

## Related
- [Components](../built-in-components/) — the structs registered here
- [Serialization](../scene-serialization/) — the registry driving save/load
- [Go-flavored design](../../core-and-conventions/go-flavored-design/) — struct-of-fn-pointers as an itable
