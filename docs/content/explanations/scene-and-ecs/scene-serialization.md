+++
title = 'Serialization'
weight = 5
+++

# Serialization

Saving and loading a scene is entirely registry-driven. There is no per-component code in the
save/load path — it walks entt storage, asks the [component registry](../component-registry/)
what each thing is, and dumps or restores it. The result is a JSON document that round-trips
entities, their components, and their stable identities.

## The document shape

`sceneToJson` produces a `{ version, entities: [...] }` document. Each entity is its Uuid plus a
map of component name to component JSON:

```json
{
  "version": 1,
  "entities": [
    {
      "id": 7421550...,
      "components": {
        "Name": { "name": "Cube" },
        "Transform": { "translation": {"x":1,"y":2,"z":3}, "scale": {...}, "rotation": {...} }
      }
    }
  ]
}
```

`sceneToJson` returns the document without file IO, so it can be embedded inside the larger
`project.json` (see [project serialization](../../geometry-and-assets/project-serialization/)).
`writeScene`/`readScene` add the file layer on top.

## Serialize: walk storage, look up by id

`serializeEntity` iterates the entity's storage sets, and for each one whose id the registry
knows, calls that row's `serialize` closure:

```cpp
for (auto&& [id, set] : scene.registry.storage())
{
    if (!set.contains(entity.handle)) continue;
    const ComponentTraits* traits = findById(reg, id);
    if (traits == nullptr) continue;   // unregistered/internal storage — skipped
    components[traits->name] = traits->serialize(scene, entity);
}
```

A storage with no registered row is skipped silently — that is how `IdComponent` stays out of the
`components` map (it is written as the top-level `id` instead). `sceneToJson` iterates
`forEach<IdComponent>` so every entity with an identity gets an entry.

## Deserialize: look up by name, add then fill

`deserializeEntity` reads each JSON key, finds the row by name, and runs its `deserialize` closure
(which adds the component with defaults if missing, then fills it from JSON). An unknown key logs
a warning and is skipped rather than failing the load, so a file from a build with an extra
component still opens. A parse failure inside a known component propagates as a
[`Result` error](../../core-and-conventions/error-handling/) with the component name prefixed.

```cpp
const ComponentTraits* traits = findByName(reg, it.key());
if (traits == nullptr)
{
    logWarn(std::format("unknown component '{}', skipping", it.key()));
    continue;
}
auto result = traits->deserialize(scene, entity, it.value());
if (!result) return Err(std::format("{}: {}", it.key(), result.error()));
```

## UUID stability is the whole point

entt entity handles are recycled and not stable across runs, so they can never be the on-disk
identity. Every serialized entity carries a [Uuid](../built-in-components/) in its `IdComponent`,
and that is what gets written. The load path is deliberately not `createEntity` (which mints fresh
Uuids); it preserves the stored ids:

```cpp
scene.registry.clear();
std::unordered_map<u64, entt::entity> uuidToHandle;
for (const nlohmann::json& entry : doc["entities"])
{
    const u64 uuid = jsonU64Or(entry, "id", 0);
    entt::entity handle = scene.registry.create();
    scene.registry.emplace<IdComponent>(handle, Uuid{ uuid });
    uuidToHandle.emplace(uuid, handle);
    // ... deserialize components ...
}
```

> [!NOTE]
> The `uuidToHandle` map is built but not yet consumed. It is the seam for cross-entity references
> (parenting, light targets) that resolve a stored Uuid to a live handle after every entity
> exists. No reference-holding component exists yet, so the map is filled and left for the future.

## Versioning

The document carries `version` (`SceneVersion`, currently `1`). `sceneFromJson` rejects any other
version up front rather than guessing at an old layout. Bumping the version is how a breaking
layout change announces itself. A headless `runSceneSerializationSelfTest` registers Name +
Transform, builds a two-entity scene, writes and reads it back, and confirms the cube's
translation survived — a round-trip smoke test with no GPU.

> [!WARNING]
> nlohmann/json is compiled `JSON_NOEXCEPTION`, which turns a would-be throw into `std::abort`.
> The load path therefore validates before it indexes — `is_object`, `contains`, `is_array` —
> and reads fields through `jsonU64Or`-style helpers that default rather than throw. A malformed
> file returns an `Err`, it does not crash.

## In the code

| What | File | Symbols |
|---|---|---|
| Per-entity to/from JSON | `scene.cppm` | `serializeEntity`, `deserializeEntity` |
| Whole scene to/from JSON | `scene.cppm` | `sceneToJson`, `sceneFromJson` |
| File layer | `scene.cppm` | `writeScene`, `readScene` |
| Version + round-trip test | `scene.cppm` | `SceneVersion`, `runSceneSerializationSelfTest` |
| Stable identity | `core.cppm` | `Uuid`, `newUuid` |

## Related
- [Component registry](../component-registry/) — what `serialize`/`deserialize` dispatch through
- [Project serialization](../../geometry-and-assets/project-serialization/) — where this scene doc is embedded
- [JSON gateway](../../core-and-conventions/json-gateway/) — the no-throw parse/access helpers
- [Error handling](../../core-and-conventions/error-handling/) — the `Result` the load path returns
