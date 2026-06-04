# Phase 2: Serialize parent by uuid, two-pass resolve, SceneVersion 3 migration

**Status:** COMPLETED

<!-- Flip to COMPLETED when the Done-when checklist passes validation-clean (selftest + e2e + contract test). Delete this file only after COMPLETED + merged. -->

## Goal

Make parenting durable. `RelationshipComponent` (added in phase 1) currently lives only at
runtime — `createEntity` emplaces a default `{parent:0}`, `relinkHierarchy` rebuilds the
caches, but nothing writes the parent uuid to disk or reads it back. This phase:

- Generates `RelationshipComponent` serde (a `toJson`/`fromJson` pair emitting **only** the
  durable parent `Uuid`) by adding it to the scene-component catalog in
  `tools/gen-control-dto/gen.ts`, then registers it in `registerBuiltinComponents` so
  `serializeEntity` / `deserializeEntity` round-trip it.
- Bumps `SceneVersion` 2 → 3 and extends the version-history comment + the upper-bound check.
- Turns the dormant `uuidToHandle` resolve hook in `sceneFromJson` into the parent-resolution
  + children-rebuild pass that runs after the create+deserialize loop.
- Migrates v1/v2 scenes (no `RelationshipComponent` on disk ⇒ every entity defaults to root).
- Relinks the hierarchy after `copy-entity` so a duplicate appears in its source's children.

It depends on phase 1 (the `RelationshipComponent` struct, `relinkHierarchy(Scene&)`,
`setParent`, recursive `destroyEntity`, and the world-transform pass). It does **not** touch
the wire/editor — that is phase 4 (`list-entities` parentId, `set-parent`, the React tree).

## Current state

Grounded anchors (current line numbers):

- `SceneVersion = 2` — `engine/source/saffron/scene/scene.cppm:422`. Version-history comment
  at `:420-421` ("1 = entities only; 2 = adds the top-level environment block").
- `sceneFromJson` (opens `:569`) upper-bound check rejects `version > SceneVersion` —
  `engine/source/saffron/scene/scene.cppm:576`.
- The loader: a single create-preserving-uuid + `deserializeEntity` loop at
  `engine/source/saffron/scene/scene.cppm:593-617`; the `uuidToHandle` map built at `:589`
  with `emplace` at `:606`.
- The dormant resolve hook — `static_cast<void>(uuidToHandle);` with the "hook is ready for
  them" comment — `engine/source/saffron/scene/scene.cppm:619-621`. This is exactly where
  parent-uuid → live-handle resolution belongs.
- `serializeEntity` (opens `:511`) skips storages with no registered `ComponentTraits` (e.g.
  `IdComponent`, the unregistered `WorldTransformComponent`) —
  `engine/source/saffron/scene/scene.cppm:520-524`, skip comment at `:523`. So
  `RelationshipComponent` only round-trips once registered.
- Component serde bodies are **generated**: each component's `toJson`/`fromJson` is emitted
  into `engine/source/saffron/scene/scene_component_serde.generated.cpp` (functions declared
  `scene.cppm:396-415`) from the scene-component catalog inside `tools/gen-control-dto/gen.ts`
  (the `emitSceneSerde` block). The uuid-by-reference pattern to follow is `MeshComponent`
  (`meshComponentToJson` / `meshComponentFromJson`,
  `scene_component_serde.generated.cpp:106-115`): `toJson { mesh: uuidToJson(c.mesh.value) }`,
  `fromJson Uuid{ jsonU64Or(j,"mesh",0) }`.
- `registerBuiltinComponents` wires the generated named functions for Name, Transform, Mesh,
  Camera, Material, DirectionalLight, PointLight, SpotLight, ReflectionProbe with empty
  `drawInspector` lambdas (the host is present-only) —
  `engine/source/saffron/sceneedit/scene_edit_components.cpp:17-72` (the whole file is 73
  lines); the `Mesh` registration is `:31-35`.
- `copy-entity` deep-duplicates by looping `ctx.sceneEdit.registry.rows` and round-tripping
  each present component through `t.deserialize(scene, fresh, t.serialize(scene, *src))` —
  `engine/source/saffron/control/control_commands_scene.cpp:670-697`, loop at `:686-692`,
  name fixup at `:693`, `sceneVersion += 1` at `:694`.
- `runSceneSerializationSelfTest` round-trips a hand-built scene —
  `engine/source/saffron/scene/scene.cppm:660` (registers Name + Transform inline, `:663-672`).
- `RelationshipComponent { Uuid parent; entt::entity parentHandle; std::vector<entt::entity>
  children; }`, `createEntity` default emplace, and `relinkHierarchy(Scene&)` are phase-1
  deliverables in `engine/source/saffron/scene/scene.cppm` (near `TransformComponent` ~`:43`
  and `destroyEntity` `:315-318`). Only `parent` is durable; `parentHandle` / `children` are
  runtime caches that must never be serialized or value-copied.

## Implementation

### 1. Generate `RelationshipComponent` serde, then register it (parent uuid only)

Component serde is generated, so add `RelationshipComponent` to the scene-component catalog in
`tools/gen-control-dto/gen.ts` (the `emitSceneSerde` block, alongside `MeshComponent`), then
regenerate. The generator emits a named `relationshipComponentToJson` / `…FromJson` pair into
`engine/source/saffron/scene/scene_component_serde.generated.cpp` (declared in `scene.cppm`),
following the `MeshComponent` uuid pattern:

```cpp
auto relationshipComponentToJson(const RelationshipComponent& c) -> nlohmann::json
{
    return nlohmann::json{ { "parent", uuidToJson(c.parent.value) } };
}

auto relationshipComponentFromJson(RelationshipComponent& c, const nlohmann::json& j)
    -> Result<void>
{
    c.parent = Uuid{ jsonU64Or(j, "parent", 0) };
    // parentHandle / children stay default — caches, rebuilt by the resolve pass.
    return {};
}
```

Then wire those generated functions in `registerBuiltinComponents`
(`engine/source/saffron/sceneedit/scene_edit_components.cpp`, after the `ReflectionProbe`
block at `:67-71`), with an empty `drawInspector` lambda like every other entry (the host is
present-only):

```cpp
registerComponent<RelationshipComponent>(reg, "Relationship",
    [](Scene&, Entity) {},
    relationshipComponentToJson,
    relationshipComponentFromJson,
    false);   // non-removable: parenting is edited via set-parent / the tree, not as a raw field
```

- `toJson` emits **only** `{ "parent": uuidToJson(c.parent.value) }` — never `parentHandle`
  or `children` (live `entt::entity` handles are not stable across load, and `copyTo`
  (`scene.cppm:462-468`) value-copies the component, so a serialized children vector would
  alias or emit non-portable ids).
- `fromJson` reads **only** the parent uuid; the caches stay default and are rebuilt by the
  resolve pass (§3) / `relinkHierarchy`.
- `removable = false`. Phase-4 editor work must filter it out of the Inspector's removable set
  (like the existing `NON_REMOVABLE` filter) so parenting is not edited as a raw uuid field;
  note that here so the contract test's `inspect` validation accounts for the new component.

`uuidToJson` / `jsonU64Or` are the same helpers `MeshComponent` uses, so a parent uuid
serializes as a bare non-negative integer in the file (string in TS once phase 4 puts it on
the wire).

### 2. Bump `SceneVersion` 2 → 3

In `engine/source/saffron/scene/scene.cppm`:

- `:422` — `inline constexpr int SceneVersion = 3;`.
- `:420-421` — extend the version-history comment: "… 2 = adds the top-level environment block;
  3 = adds a per-entity Relationship component (durable parent uuid). A v1/v2 document has no
  Relationship; the loader defaults every entity to root."
- `:576` — the upper-bound check `version > SceneVersion` now admits 3; no logic change, the
  bumped constant does the work. v1/v2 still load (lower bound stays `version < 1`), and the
  missing-relationship migration in §3 keeps them clean.

### 3. Parent resolve pass (replace the dormant hook)

Replace `static_cast<void>(uuidToHandle);` at `engine/source/saffron/scene/scene.cppm:619-621`
(the hook position, after the single create+deserialize loop at `:593-617`) with the resolve
pass. Resolution must run after the whole loop: `sceneToJson` emits entities in
`forEach<IdComponent>` order (`:558`), which is arbitrary, so a child's parent uuid may appear
*after* the child in the array — resolution runs only after all entities + uuids exist.

```cpp
// Resolve parent uuids to live handles and rebuild the children caches.
// (relinkHierarchy specialized to the loader's uuidToHandle map.)
forEach<RelationshipComponent>(scene, [&](Entity e, RelationshipComponent& rel) {
    if (rel.parent.value == 0) {
        rel.parentHandle = entt::null;   // explicit root
        return;
    }
    auto it = uuidToHandle.find(rel.parent.value);
    if (it == uuidToHandle.end()) {
        logWarn(std::format("entity {} references missing parent uuid {}; treating as root",
                            getComponent<IdComponent>(scene, e).id.value, rel.parent.value));
        rel.parent = Uuid{ 0 };
        rel.parentHandle = entt::null;
        return;
    }
    rel.parentHandle = it->second;
    getComponent<RelationshipComponent>(scene, Entity{ it->second }).children.push_back(e.handle);
});
```

Notes:

- **v1/v2 migration.** A pre-v3 document has no `Relationship` key, so `deserializeEntity`
  never touches it. But every entity created in the loop at `:604-605` only gets an
  `IdComponent` emplaced by hand — `deserializeEntity` add-defaults a component **only** when
  its JSON key is present. So after the loop, walk all entities and ensure a default
  `RelationshipComponent`
  exists before the resolve pass (use `addComponent<RelationshipComponent>` /
  `addDefault` if missing, or iterate `uuidToHandle` and emplace where absent). With a default
  `{parent:0}` every legacy entity resolves to root in the pass above. **Decided: the loader
  calls `relinkHierarchy(scene)` at the hook position** — it defaults a root Relationship onto
  every entity missing one, `logWarn`s + roots dangling parents, and cuts source-data cycles,
  so one implementation serves load, reparent, copy, and raw `set-component` writes. The
  loader's local `uuidToHandle` map became dead weight and was removed; `relinkHierarchy`
  builds its own.
- **Dangling parent** (uuid not in `uuidToHandle`) ⇒ root + `logWarn`, never a crash. Reset
  `rel.parent` to 0 so the in-memory state matches "root" and a subsequent save does not
  re-emit the dead uuid.
- **`forEach` is unordered** — fine here: this pass touches each entity once and only writes
  its own `parentHandle` + pushes into its parent's `children`; order does not matter because
  all handles already exist.

### 4. Relink after `copy-entity`

In `engine/source/saffron/control/control_commands_scene.cpp`, the copy round-trips each
present component through `t.deserialize(scene, fresh, t.serialize(scene, *src))` (loop at
`:686-692`), duplicating the source's `RelationshipComponent.parent` uuid — correct: the copy
joins the source's parent. After the name fixup at `:693`, call `relinkHierarchy(scene)` before
the `sceneVersion += 1` at `:694`:

```cpp
getComponent<NameComponent>(scene, fresh).name = copyName;
relinkHierarchy(scene);            // register the copy in its parent's children + resolve handles
ctx.sceneEdit.sceneVersion += 1;
```

Rationale: the round-trip duplicates the `parent` uuid safely (children/parentHandle are
non-serialized, so the loop's `serialize`/`deserialize` never touches live handles), but
without `relinkHierarchy` the new entity's `parentHandle` / membership in its parent's
`children` vector are stale — the copy is invisible to its parent. v1 is single-entity copy
(no subtree recursion); the copy is a sibling of the source under the same parent.

## Done when

- [ ] `RelationshipComponent` serde is generated from the gen.ts scene-component catalog and
  wired in `registerBuiltinComponents` (`scene_edit_components.cpp`), `removable=false`,
  serializing only `{ "parent": <uuid> }`; the generated serde file is regenerated and committed.
- [ ] `SceneVersion == 3`; the version-history comment (`scene.cppm:420-421`) and the
  upper-bound check (`:576`) reflect v3; v1/v2 documents still load.
- [ ] Round-trip: build a two-entity scene where child's `RelationshipComponent.parent` is the
  parent's uuid, `writeScene` then `readScene` (or `sceneToJson`/`sceneFromJson`); after load
  the child's `parentHandle` resolves and it appears in the parent's `children` — checked by an
  added case in `runSceneSerializationSelfTest` (`scene.cppm:660`) or a `tests/e2e` round-trip
  via save/load over the control plane.
- [ ] **Order-independent:** a scene file whose child entry precedes its parent entry in the
  `entities` array still resolves (the post-loop resolve pass handles it) — assert in the
  selftest by emitting the child first.
- [ ] **v2 migration:** loading a v2 scene (no `Relationship` key anywhere) succeeds with every
  entity defaulting to root (`parent == 0`, `parentHandle == entt::null`) — e2e load of a
  fixture v2 scene, validation-clean, no warnings.
- [ ] **Dangling parent:** a scene whose child references a parent uuid not in the file loads
  the child as a root with a single `logWarn`, not a crash — selftest/e2e assertion.
- [ ] `se copy-entity <parent-or-child>` followed by `se inspect <copy>` shows the copy
  carrying the source's parent uuid, and the copy is present in its parent's children (verified
  after the phase-4 `list-entities parentId` lands; until then, assert via the round-trip that
  the copied parent uuid survives a save/load).
- [ ] `tools/ci/check.sh` (engine build → present-only smoke → control-schema contract test →
  frontend build) and `make e2e` run validation-clean.

## Risks / seams

- **Post-loop resolve is essential.** `sceneToJson`'s `forEach<IdComponent>` (`scene.cppm:558`)
  emits entities in arbitrary order; the resolve must run after the full single
  create+deserialize loop, on the complete `uuidToHandle` map. Folding resolution into the
  per-entity loop would fail on a forward reference.
- **Durable surface is the uuid only.** Serialize/copy `parent` (a `Uuid`); never an
  `entt::entity` handle (`parentHandle`) or the `children` vector — entt ids are not stable
  across load, copy-entity round-trips each component through serialize→deserialize, and
  `copyTo` (currently uncalled) value-copies the component. The §1 generated `toJson`/`fromJson`
  touching only `parent` is the load-bearing safety choice; do not widen it.
- **Migration must default missing relationships.** `deserializeEntity` only add-defaults a
  component whose JSON key is present, so legacy entities have no `RelationshipComponent` after
  the loop unless explicitly ensured. Missing this leaves entities the resolve pass / world
  pass cannot address. Reusing `relinkHierarchy` (which must tolerate + default missing) is the
  least-duplicated path; confirm phase 1's `relinkHierarchy` defaults rather than asserts.
- **`copy-entity` relink ordering.** `relinkHierarchy` must run after the component loop and
  the name fixup but before/at the `sceneVersion` bump, or the copy is orphaned from its
  parent's `children`. Single-entity copy only (no subtree); a subtree copy that remints child
  uuids is out of scope for this phase.
- **Selftest/contract scope.** `runSceneSerializationSelfTest` registers components inline (it
  uses the generated `nameComponentToJson` / `transformComponentToJson`); the new round-trip
  case must register `RelationshipComponent` the same way (or call `registerBuiltinComponents`).
  The contract test (`tools/check-control-schema`) validates wire payloads against the generated
  manifest/OpenRPC, not hand-authored per-command schemas. Once `RelationshipComponent` is in
  the catalog and regenerated, the contract test sees it through the generated DTO schema (a
  closed object, `additionalProperties:false`); this phase must keep the test green, so
  regenerate and commit the generated outputs alongside the registration. The wire-facing
  `relationship` exposure (`list-entities` parentId) is phase 4.
