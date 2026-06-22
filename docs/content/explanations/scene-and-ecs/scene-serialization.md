+++
title = 'Serialization'
weight = 5
+++

# Serialization

Scene serialization converts a live scene into a JSON document and back, preserving every entity, its
components, and its stable identity. The save and load paths are registry-driven: they hold no
per-component code, instead walking the [component registry](../component-registry/) rows and asking
each one what to write and how to read it.

This keeps the format open to extension. Adding a component to the registry makes it serializable
without touching the save/load path, and the round-trip is byte-stable — a document written by one
build reads back into an equivalent scene, and re-serializes to the same bytes.

## The document shape

`scene_to_json` produces a `{ version, environment, entities: [...] }` document. Each entity is its
`Uuid`, its components, and the authored component order:

```json
{
  "version": 4,
  "environment": { "skyMode": "procedural", "exposure": 1.0, ... },
  "entities": [
    {
      "id": "1024",
      "components": {
        "Name": { "name": "Cube" },
        "Transform": { "translation": {"x":1,"y":2,"z":3}, "scale": {...}, "rotation": {...} },
        "Relationship": { "parent": "0" }
      },
      "componentOrder": ["Name", "Transform"]
    }
  ]
}
```

Ids are written as decimal strings (a `u64` does not fit a JSON number safely), and keys are emitted
sorted, so the document is byte-stable. `scene_to_json` returns the document without file IO, so it
can be embedded inside the larger `project.json` (see
[project serialization](../../geometry-and-assets/project-serialization/)). `write_scene` and
`read_scene` add the file layer on top.

## Serialize: walk rows, emit by name

`serialize_entity` walks the registry rows and, for each one whose `has` reports the component present
on the entity, calls that row's `serialize` pointer:

```rust
for traits in &self.rows {
    if (traits.has)(scene, entity) {
        components.insert(traits.name.to_string(), (traits.serialize)(scene, entity));
    }
}
```

Walking rows (rather than introspecting ECS storage, which `hecs` does not expose) is how
`IdComponent` stays out of the `components` map — it has no registry row, and is written as the
top-level `id` instead. `WorldTransform`, `PoseOverride`, and `ComponentOrder` are unregistered for
the same reason; `componentOrder` is written separately by the document assembler.

## Deserialize: look up by name, add then fill

`deserialize_entity` reads each JSON key, finds the row by name, and runs its `deserialize` pointer.
That pointer adds the component with defaults if missing, then fills it from JSON. An unknown key
warns and is skipped rather than failing the load, so a file from a build with an extra component
still opens. A parse failure inside a known component propagates as an
[`Error`](../../core-and-conventions/error-handling/) with the component name prefixed.

```rust
let Some(&index) = self.by_name.get(name.as_str()) else {
    tracing::warn!("unknown component '{name}', skipping");
    continue;
};
(self.rows[index].deserialize)(scene, entity, value)
    .map_err(|e| Error::Deserialize(format!("{name}: {e}")))?;
```

## UUID stability

ECS handles are recycled and not stable across runs, so they cannot serve as the on-disk identity.
Every serialized entity instead carries a [`Uuid`](../built-in-components/) in its `IdComponent`, and
that is what gets written. The load path does not call `create_entity`, which would mint fresh uuids;
it uses `spawn_with_id` to preserve the stored ids:

```rust
self.clear();
for entry in entries {
    let uuid = json_u64_or(entry, "id", 0);
    let entity = self.spawn_with_id(Uuid(uuid));
    // ... deserialize components, then componentOrder ...
}
self.relink_hierarchy();
```

> [!NOTE]
> Cross-entity references resolve only after the loop. The [scene hierarchy](../scene-hierarchy/)
> stores each entity's parent as a uuid, and a child's entry may precede its parent in the array, so
> `relink_hierarchy` maps every stored parent uuid to a live handle once all entities exist. A
> pre-hierarchy document simply has no Relationship keys, and every entity loads as a root.

## Versioning

The document carries `version` (`SCENE_VERSION`, currently `4`: 1 = entities only, 2 = adds the
top-level environment block, 3 = adds the per-entity Relationship component, 4 = adds the per-entity
`componentOrder` array). `scene_from_json` rejects anything outside `[1, SCENE_VERSION]` up front
rather than guessing at an unknown layout, and migrates older documents by defaulting what they lack —
a pre-v2 scene gets a default environment, a pre-v3 scene roots every entity, a pre-v4 scene derives
the canonical component order. Bumping the version announces a breaking layout change.

A `document_bytes_match_captured_block` test pins the whole-document shape (key order, decimal-string
ids, the environment block) to the frozen `project.json` scene block: it asserts byte-equality
against a hand-assembled document, then re-parses and re-serializes it to confirm the reader is
byte-stable. The other tests cover the migration cases — parent uuids survive the round trip, a child
entry before its parent still resolves, a v2 document migrates every entity to root, and a dangling
parent downgrades to root with a warning.

> [!WARNING]
> The load path validates before it indexes — checking `is_object` / `as_array` and reading scalar
> fields through `json_u64_or`-style helpers that default rather than fault. A malformed file returns
> an `Err`, it does not crash.

## In the code

| What | File | Symbols |
|---|---|---|
| Per-entity to/from JSON | `scene/src/registry.rs` | `serialize_entity`, `deserialize_entity` |
| Whole scene to/from JSON | `scene/src/document.rs` | `scene_to_json`, `scene_from_json` |
| File layer | `scene/src/document.rs` | `write_scene`, `read_scene` |
| Version + migration | `scene/src/document.rs` | `SCENE_VERSION` |
| JSON helpers | `json/src/lib.rs` | `parse_json`, `dump_json_sorted`, `json_u64_or`, `uuid_to_json` |
| Stable identity | `core/src/uuid.rs` | `Uuid`, `Uuid::new` |

## Related
- [Component registry](../component-registry/) — what `serialize`/`deserialize` dispatch through
- [Project serialization](../../geometry-and-assets/project-serialization/) — where this scene doc is embedded
- [JSON gateway](../../core-and-conventions/json-gateway/) — the no-fault parse/access helpers
- [Error handling](../../core-and-conventions/error-handling/) — the `Result` the load path returns
