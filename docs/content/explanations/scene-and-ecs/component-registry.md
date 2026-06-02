+++
title = 'Component registry'
weight = 4
+++

# Component registry

A component registry is a runtime table that pairs each component type with the operations
cross-cutting features perform on it: serialize, deserialize, add, remove, clone, and draw. Each
entry is a row of closures, so a feature dispatches on a component without naming its type.

Several subsystems need per-component knowledge. The serializer converts a component to and from
JSON, the editor draws, adds, and removes it, and entity cloning copies it. A registry holds that
knowledge in one place. Registering a component is a single call, and no central code changes when
a new component is added.

## The itable

`ComponentTraits` is a struct of `std::function` fields — a Go-interface vtable built by hand,
one field per operation a cross-cutting feature needs:

```cpp
struct ComponentTraits
{
    entt::id_type id = 0;   // == entt::type_hash<C>::value(); the storage() join key
    std::string name;       // stable JSON key + UI header, e.g. "Transform"
    bool removable = true;
    std::function<bool(Scene&, Entity)> has;
    std::function<void(Scene&, Entity)> addDefault;
    std::function<void(Scene&, Entity)> remove;
    std::function<void(Scene&, Entity, Scene&, Entity)> copyTo;        // clone src -> dst
    std::function<nlohmann::json(Scene&, Entity)> serialize;
    std::function<Result<void>(Scene&, Entity, const nlohmann::json&)> deserialize;
    std::function<void(Scene&, Entity)> drawInspector;  // ImGui body lives in the editor
};
```

The `ComponentRegistry` is a vector of these rows plus two indexes: `byId` (keyed by the entt
type hash, the same id entt's `storage()` iteration yields) and `byName` (keyed by the stable
JSON string). Both map to the same row.

## Registering is one call

`registerComponent<C>` takes the type, a name, and three closures: a draw function, a to-JSON, and
a from-JSON. It synthesizes everything else from the generic component functions. `has`,
`addDefault`, `remove`, `copyTo`, and the `Scene/Entity` adapters around `serialize`/`deserialize`
are all generated.

```cpp
template <typename C>
void registerComponent(ComponentRegistry& reg, std::string name,
                       std::function<void(Scene&, Entity)> drawFn,
                       std::function<nlohmann::json(const C&)> toJson,
                       std::function<Result<void>(C&, const nlohmann::json&)> fromJson,
                       bool removable = true);
```

The caller writes only the genuinely per-component parts: how it draws, and how its fields map to
JSON. The `deserialize` closure adds the component with defaults before calling `fromJson`, so a
load never assumes the component already exists. All eight built-in components are registered this
way in `editor_components.cpp`, one call each.

## Lookup feeds both directions

```cpp
auto findById(const ComponentRegistry&, entt::id_type) -> const ComponentTraits*;
auto findByName(const ComponentRegistry&, const std::string&) -> const ComponentTraits*;
```

Serialization walks `scene.registry.storage()`, takes each storage's id, and calls `findById` to
discover what to write. Loading reads JSON keys and calls `findByName`. The two indexes let one
table drive both the type-keyed and string-keyed paths.

## Why closures, not entt::meta

entt ships a reflection system, `entt::meta`, that could carry this data. The engine uses a
hand-built struct-of-closures for the reason the rest of the codebase avoids heavy machinery. A
`std::function` table is plain, debuggable data read top to bottom, and it keeps the per-component
UI and JSON code next to the registration call rather than scattered across reflection attributes.
The `drawInspector` field stays opaque at the scene layer — its ImGui body is defined in the editor
module — so `Saffron.Scene` never imports ImGui.

> [!TIP]
> The `name` string is a stable contract, not a display nicety. It is the JSON key on disk and
> the editor's component header. Renaming it silently breaks every saved scene that used the old
> name (the loader logs `unknown component '<old>', skipping`). Treat it like a serialization
> version.

## In the code

| What | File | Symbols |
|---|---|---|
| The itable | `scene.cppm` | `ComponentTraits`, `ComponentRegistry` |
| One-call registration | `scene.cppm` | `registerComponent` |
| Lookup | `scene.cppm` | `findById`, `findByName` |
| The eight registrations | `editor_components.cpp` | `registerComponent<...>` |

## Related
- [Components](../built-in-components/) — the structs registered here
- [Serialization](../scene-serialization/) — the registry driving save/load
- [Inspector](../../ui-and-editor/inspector/) — the `drawInspector` closures in action
- [Go-flavored design](../../core-and-conventions/go-flavored-design/) — struct-of-closures as an itable
