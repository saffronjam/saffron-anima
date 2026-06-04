# Phase 1: Relationship component + cached world-transform propagation

**Status:** COMPLETED

<!-- Flip to COMPLETED when the Done-when checklist passes validation-clean (the hierarchy selftest green + engine builds in the saffron-build toolbox). Delete this file only after COMPLETED + merged. -->

## Goal

Make parenting *exist* in the engine. Add a `RelationshipComponent` (a durable parent `Uuid`
plus non-serialized `parentHandle`/`children` runtime caches), a once-per-frame
`updateWorldTransforms` flatten pass that writes a cached `WorldTransformComponent`, a
cycle-guarded world-preserving `setParent`, and a recursive `destroyEntity`. The registry stays
flat; this phase introduces the relationship + world-transform substrate **without** rewiring any
renderer/gizmo/pick consumer (that is phase 3) and **without** serializing the parent yet (that
is phase 2). The architecture is settled in `phase-0-research-and-architecture.md`: parent-uuid is
the only durable surface, children are a derived cache, world transform is a single flatten pass
(not recompute-on-read, not a dirty-flag scheme).

## Current state

- `Scene` is a flat `entt::registry` + `SceneEnvironment` + borrowed `catalog` (struct
  `scene.cppm:260`, `environment` member `scene.cppm:263`); `Entity` is a bare `entt::entity`
  handle (`scene.cppm:270-273`). No parent/child anything.
- Components are plain POD structs near `TransformComponent` (`scene.cppm:38-43`):
  `NameComponent` (`:28`), `IdComponent` (`:33`), `MeshComponent` (`:46`), etc.
- `transformMatrix(const TransformComponent&)` (`scene.cppm:119`) builds a **local** `T*R*S`
  matrix. No world-transform derivation exists anywhere.
- `createEntity` (`scene.cppm:306-313`) emplaces `Id` + `Name` + `Transform` only.
- `destroyEntity` (`scene.cppm:315-318`) is a single `registry.destroy(handle)` with **no**
  recursion.
- `forEach<C...>` wraps `registry.view<C...>()` — entt views are **unordered**, so it cannot give
  parent-before-child order.
- `registerComponent` synthesizes a `copyTo` value copy (`scene.cppm:462-468`, currently uncalled)
  and a `deserialize` that defaults-then-`fromJson`; copy-entity round-trips each component through
  serialize -> deserialize instead. Either path carrying live handles would alias the source or emit
  non-portable entt ids.
- `runSceneSerializationSelfTest` (`scene.cppm:660`) is the headless round-trip self-test;
  registrations it uses are inline (`Name` `:663`, `Transform` `:668`).
- `glm::decompose` is **not yet used** in the tree (no include); the matrix-decompose header
  must be added. `logWarn` is the warning sink (`core.cppm:151`).

## Implementation

All edits are in `engine/source/saffron/scene/scene.cppm`, `export namespace se`, unless noted.

### 1. Add the two component structs near `TransformComponent` (`scene.cppm:43`)

`<vector>` is already included. Add the matrix-decompose include in the global module fragment
(after the other `glm/gtc` includes):

```cpp
#include <glm/gtx/matrix_decompose.hpp>
```

Then, immediately after `TransformComponent` (`scene.cppm:43`):

```cpp
// A node in the scene tree. `parent` (a Uuid; 0 == root) is the ONLY durable/serialized
// field. parentHandle + children are a runtime cache rebuilt by relinkHierarchy after any
// structural mutation; they are NEVER serialized or value-copied (entt ids are not stable
// across load, and copy-entity round-trips each component through serialize -> deserialize).
struct RelationshipComponent
{
    Uuid parent;                              // 0 == root
    entt::entity parentHandle = entt::null;   // resolved cache, not serialized/copied
    std::vector<entt::entity> children;       // derived cache, not serialized/copied
};

// Cached world matrix, overwritten every frame by updateWorldTransforms. Unregistered
// (like IdComponent), so serializeEntity skips it (scene.cppm:523).
struct WorldTransformComponent
{
    glm::mat4 matrix{ 1.0f };
};
```

`RelationshipComponent` is registered (phase 2 wires the generated serde) but marked
**non-removable** and filtered out of the Inspector — parenting is edited via the tree /
`set-parent`, never as a raw uuid field. `WorldTransformComponent` is intentionally
**unregistered**, so `serializeEntity` (`scene.cppm:511-528`) skips it the way it skips
`IdComponent`.

### 2. Default-parent every new entity in `createEntity` (`scene.cppm:306-313`)

Emplace a default root `RelationshipComponent` alongside `Id`/`Name`/`Transform`, so every entity
is uniformly hierarchy-addressable:

```cpp
addComponent<RelationshipComponent>(scene, entity);  // RelationshipComponent{ parent:0, ... } => root
```

`WorldTransformComponent` is **not** emplaced here; the flatten pass adds/overwrites it for every
entity carrying a `TransformComponent`.

### 3. `relinkHierarchy(Scene&)` — rebuild the caches (new, near `destroyEntity` `scene.cppm:315`)

The single source of truth for the `parentHandle`/`children` caches. Walk every entity with a
`RelationshipComponent`, resolve its `parent` Uuid to a live handle, and rebuild both caches.
Build a `uuid -> handle` map by scanning `forEach<IdComponent>` (the loader has its own map at
`scene.cppm:589/606` and calls a specialized version inline — phase 2):

```cpp
void relinkHierarchy(Scene& scene)
{
    std::unordered_map<u64, entt::entity> uuidToHandle;
    forEach<IdComponent>(scene, [&](Entity e, IdComponent& id) {
        uuidToHandle.emplace(id.id.value, e.handle);
    });
    // pass 1: clear caches; pass 2: resolve parent + push into the parent's children.
    forEach<RelationshipComponent>(scene, [&](Entity, RelationshipComponent& rel) {
        rel.parentHandle = entt::null;
        rel.children.clear();
    });
    forEach<RelationshipComponent>(scene, [&](Entity e, RelationshipComponent& rel) {
        if (rel.parent.value == 0) { return; }                 // root
        auto it = uuidToHandle.find(rel.parent.value);
        if (it == uuidToHandle.end()) {                        // dangling parent -> root
            logWarn(std::format("relationship parent {} not found; treating as root", rel.parent.value));
            rel.parent = Uuid{ 0 };
            return;
        }
        rel.parentHandle = it->second;
        getComponent<RelationshipComponent>(scene, Entity{ it->second }).children.push_back(e.handle);
    });
}
```

This is O(N) over the registry, called after every structural mutation (load, `setParent`,
`destroyEntity`, copy-entity). Note `forEach<RelationshipComponent>` is unordered — that is fine
here because `relinkHierarchy` only links pairs, it does not propagate; ordering is the flatten
pass's concern.

### 4. `updateWorldTransforms(Scene&)` — the once-per-frame flatten pass (new, near `transformMatrix` `scene.cppm:119`)

Walk **roots first, then children via the `children` cache** (never `forEach`, which is
unordered), composing `parentWorld * transformMatrix(local)`, writing/overwriting a
`WorldTransformComponent` on every entity with a `TransformComponent`:

```cpp
void updateWorldTransforms(Scene& scene)
{
    // Roots = RelationshipComponent.parentHandle == entt::null (or no RelationshipComponent).
    // Recurse through children; full mat4 composition preserves non-uniform parent scale, so
    // normalMatrix = transpose(inverse(mat3(world))) downstream stays correct.
    auto writeSubtree = [&](auto&& self, Entity e, const glm::mat4& parentWorld) -> void {
        glm::mat4 world = parentWorld;
        if (hasComponent<TransformComponent>(scene, e)) {
            world = parentWorld * transformMatrix(getComponent<TransformComponent>(scene, e));
            if (!hasComponent<WorldTransformComponent>(scene, e)) {
                addComponent<WorldTransformComponent>(scene, e);
            }
            getComponent<WorldTransformComponent>(scene, e).matrix = world;
        }
        if (hasComponent<RelationshipComponent>(scene, e)) {
            for (entt::entity child : getComponent<RelationshipComponent>(scene, e).children) {
                self(self, Entity{ child }, world);
            }
        }
    };
    forEach<RelationshipComponent>(scene, [&](Entity e, RelationshipComponent& rel) {
        if (rel.parentHandle == entt::null) { writeSubtree(writeSubtree, e, glm::mat4(1.0f)); }
    });
}
```

This relies on the `children` cache being current; it is the caller's job to `relinkHierarchy`
after structural changes (phase 2's loader does, `setParent`/`destroyEntity` below do). The host
main loop calls `updateWorldTransforms(scene)` once before render in phase 3; this phase only
defines it and exercises it from the selftest.

### 5. Read helpers (new, near `updateWorldTransforms`)

The world-space accessors every consumer migrates to in phase 3. They read the cache; the
`worldMatrix` fallback walks parents when the cache is stale (e.g. a test that mutated a local
transform without re-running the pass):

```cpp
auto worldMatrix(Scene& scene, Entity e) -> glm::mat4;       // cached WorldTransformComponent, else walk parents
auto worldTranslation(Scene& scene, Entity e) -> glm::vec3;  // = glm::vec3(worldMatrix(scene,e)[3])
auto worldRotation(Scene& scene, Entity e) -> glm::quat;     // for Local-space gizmo axes + spot/cam aiming
```

`transformMatrix` (`scene.cppm:119`) stays the **local** builder, unchanged.

### 6. `setParent(Scene&, Entity child, Entity newParent, bool keepWorld = true) -> Result<void>` (new, near `destroyEntity` `scene.cppm:315`)

```cpp
auto setParent(Scene& scene, Entity child, Entity newParent, bool keepWorld = true) -> Result<void>;
```

Algorithm:

1. **Self-parent guard:** `child.handle == newParent.handle` (when `newParent` is non-null) =>
   `Err("cannot parent an entity to itself")`.
2. **Cycle guard:** walk `newParent`'s `parentHandle` chain (and `newParent` itself); if it
   reaches `child.handle`, `Err("reparent would create a cycle")`. This is O(depth).
3. **Capture world** (if `keepWorld`): `glm::mat4 childWorld = worldMatrix(scene, child)`.
4. **Relink the durable field:** set `getComponent<RelationshipComponent>(scene, child).parent`
   to `newParent`'s `IdComponent.id` (or `Uuid{0}` when `newParent.handle == entt::null`, i.e.
   detach to root).
5. **World-preserving rebase** (if `keepWorld`): compute the child's new local matrix
   `local = inverse(worldMatrix(scene, newParent)) * childWorld` (identity parent-world when root),
   decompose it with `glm::decompose` into translation / rotation (quat -> `glm::eulerAngles`) /
   scale, and write back into the child's `TransformComponent`. **Lossy under sheared /
   non-uniform-scaled parents** — TRS-only is the accepted contract; inherited shear is not
   representable in Euler+scale (document at the call site).
6. **Rebuild caches:** `relinkHierarchy(scene)` (re-stamps `parentHandle`/`children` from the
   updated parent uuids).
7. **Refresh world** for the moved subtree: the next `updateWorldTransforms` recomputes; the
   selftest calls it explicitly to assert.

This is the engine-authoritative reparent the phase-4 `set-parent` control command wraps; the
world-preserving math belongs next to `TransformComponent`, not in the editor.

### 7. Recursive `destroyEntity(Scene&, Entity)` (`scene.cppm:315`)

Replace the single `registry.destroy` with a subtree destroy:

```cpp
void destroyEntity(Scene& scene, Entity entity)
{
    // Collect the whole subtree (this entity + all descendants) BEFORE any destroy —
    // registry.destroy invalidates handles, so children must be gathered first.
    std::vector<entt::entity> doomed;
    auto gather = [&](auto&& self, entt::entity h) -> void {
        doomed.push_back(h);
        if (scene.registry.all_of<RelationshipComponent>(h)) {
            for (entt::entity c : scene.registry.get<RelationshipComponent>(h).children) {
                self(self, c);
            }
        }
    };
    gather(gather, entity.handle);

    // Detach from the parent's children cache so it does not dangle.
    if (scene.registry.all_of<RelationshipComponent>(entity.handle)) {
        entt::entity parent = scene.registry.get<RelationshipComponent>(entity.handle).parentHandle;
        if (parent != entt::null && scene.registry.all_of<RelationshipComponent>(parent)) {
            auto& kids = scene.registry.get<RelationshipComponent>(parent).children;
            std::erase(kids, entity.handle);
        }
    }

    for (entt::entity h : doomed) { scene.registry.destroy(h); }
}
```

The phase-4 `destroy-entity` command (`control_commands_scene.cpp:147-164`) clears
`ctx.sceneEdit.selected` only when the root was selected (`:157`); its selection-clear must learn
to clear when **any** destroyed descendant was selected (not just the root); that is a control-side
concern noted here, wired in phase 4.

### 8. Cover it in a hierarchy selftest

Extend `runSceneSerializationSelfTest` (`scene.cppm:660`), or add a sibling
`runSceneHierarchySelfTest()` next to it (preferred — keeps the round-trip test focused), invoked
from the same headless self-test entry point. It registers `Name` + `Transform` +
`RelationshipComponent` inline (mirroring the existing inline registrations at `scene.cppm:663`/
`:668`) and asserts:

- Build `parent` (at translation `(10,0,0)`) and `child` (local translation `(0,2,0)`),
  `setParent(scene, child, parent)`, `updateWorldTransforms(scene)`; assert
  `worldMatrix(child) == worldMatrix(parent) * transformMatrix(localChild)` within fp tolerance.
- `setParent(scene, child, ancestorChain...)` onto a descendant returns `Err`; self-parent
  returns `Err`.
- `setParent(child, parent, /*keepWorld=*/true)` leaves `worldMatrix(child)` unchanged within
  tolerance while the child's local TRS changes.
- `destroyEntity(scene, parent)` removes the whole subtree; assert no live `RelationshipComponent`
  still lists a destroyed handle in its `children` (no dangling handles).

Use `logInfo`/`logError` for pass/fail like the existing self-test (`logError`
`scene.cppm:683/:691`, `logInfo` `:706`).

## Done when

- [ ] A headless selftest asserts `worldMatrix(child) == worldMatrix(parent) *
      transformMatrix(localChild)` for a 2-level chain (within fp tolerance), after
      `updateWorldTransforms`.
- [ ] `setParent` onto a descendant returns `Err` (cycle guard); self-parent returns `Err`.
- [ ] `setParent(keepWorld=true)` leaves the child's world matrix unchanged within fp tolerance
      while its local `TransformComponent` TRS changes.
- [ ] Recursive `destroyEntity` of a parent removes every descendant and leaves no
      `RelationshipComponent.children` entry pointing at a destroyed handle.
- [ ] `createEntity` emplaces a default root `RelationshipComponent{ parent:0 }`; every entity is
      hierarchy-addressable (verified by the selftest counting roots).
- [ ] `transformMatrix` is unchanged (still the local builder); no renderer/gizmo/pick consumer is
      touched this phase.
- [ ] Engine builds clean in the saffron-build toolbox: `cmake --build build/debug -j1`.
- [ ] The present-only smoke run stays validation-clean (`SAFFRON_EXIT_AFTER_FRAMES=N
      ./build/debug/bin/SaffronEngine`).

## Risks / seams

- **Ordering:** `forEach`/entt views are unordered, so `updateWorldTransforms` MUST walk roots ->
  children via the `children` cache, never a view. A `forEach`-based flatten would compose children
  before parents non-deterministically.
- **Stale caches:** `parentHandle`/`children` go stale after any structural mutation. Every
  mutator (`setParent`, `destroyEntity`, phase-2 load, phase-4 copy-entity) must call
  `relinkHierarchy` (or maintain the caches in place) before traversal, or the flatten pass walks a
  stale tree.
- **Destroy-before-gather:** `registry.destroy` invalidates handles, so descendants must be
  collected **before** the first destroy (the `gather` lambda does this).
- **Copy/serialize hazard:** copy-entity round-trips each component through serialize ->
  deserialize (`control_commands_scene.cpp:686-692`); the synthesized `copyTo` value copy
  (`scene.cppm:462-468`) exists but has no callers. Either path requires the `parentHandle`/`children`
  caches to stay out of the serialized/copied surface (only `parent` is durable). Phase 2's generated
  `toJson`/`fromJson` touch only `parent`, so the serde round-trip never emits the caches; they are
  rebuilt by `relinkHierarchy`.
- **`glm::decompose` lossiness:** decomposing `inverse(parentWorld) * childWorld` back to Euler +
  scale is lossy under sheared / non-uniform-scaled parents. The accepted contract is TRS-only
  parents; inherited shear is not representable and `keepWorld` will drift under a sheared parent.
  Document this at `setParent` (step 5).
- **Cycle guard is load-bearing:** without the ancestor check, `setParent` can create a cycle that
  makes `updateWorldTransforms` and `destroyEntity` recurse infinitely.
- **`WorldTransformComponent` must stay unregistered:** registering it would serialize a derived
  per-frame matrix into the scene file and pollute the generated component schemas
  (`schemas/control/openrpc.generated.json`). It is the `IdComponent` pattern
  (`scene.cppm:523`) — present in the registry storage but absent from `ComponentRegistry`.
- **Phase boundary:** this phase deliberately does **not** touch the transform consumers — mesh draw
  (`assets.cppm:822`), pick (`assets.cppm:1061`), point/spot lights (`assets.cppm:727-766`),
  reflection-probe placement (`assets.cppm:916`, `transform.translation` read `:927`), `primaryCamera`
  (`scene.cppm:345`), the gizmo, host billboards — or the serde/`SceneVersion`. Phase 3 adopts
  `worldMatrix`; phase 2 serializes the parent and bumps `SceneVersion` 2 -> 3. Keeping them out keeps
  this phase's blast radius to additive engine machinery a headless selftest can verify.
