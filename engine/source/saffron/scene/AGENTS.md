# Saffron.Scene

The entt-backed scene model and the registry-driven JSON serialization that the whole editor/control/
script/animation stack reads. Module `Saffron.Scene`, namespace `sa`, classic `#include`, no `import std`.
An `Entity` is a bare entt handle; the `Scene` is always passed explicitly (Go-style free functions, not
methods). Imported by ~16 TUs across control, sceneedit, script, animation, assets, and host.

## Files

| File | Role |
|---|---|
| `scene.cppm` | Component structs, the `Scene`/`Entity` model + free functions, hierarchy math (`relinkHierarchy`, `setParent`, `updateWorldTransforms`, `jointMatrices`), the `ComponentTraits`/`ComponentRegistry` reflection layer, scene serde + versioning, `AssetCatalog`, `SceneEnvironment`, and two headless self-tests. |
| `scene_component_serde.generated.cpp` | Per-component `*ToJson`/`*FromJson` definitions. Committed and compiled — **do not hand-edit** (overwritten by the generator). |

## Adding or changing a component

1. Declare the struct in `scene.cppm` (plus its `*ToJson`/`*FromJson` forward declarations).
2. Edit the serde **body** inside `emitSceneSerde()` in `tools/gen-control-dto/gen.ts` — that is where
   the "generated" file's content actually lives — then run `bun run tools/gen-control-dto/gen.ts`.
3. Register it **once** in `engine/source/saffron/sceneedit/scene_edit_components.cpp`
   (`registerBuiltinComponents`) via `registerComponent<C>(…)`.

Miss step 3 and the component silently never serializes and never reaches the editor. The
`registerComponent` calls inside `scene.cppm` are **self-tests only**, not the real registration site.
The editor inspector is generic (protocol-driven `fieldRenderer`, no per-component code), so it follows
from the regenerated `sa-types.ts`.

## Rules that are easy to break

- **"Generated" is a half-truth.** `scene_component_serde.generated.cpp` is produced by a static,
  hand-maintained template in `gen.ts` (its `from the scene component DTO catalog` header is misleading
  — there is no catalog driving it). Edit `gen.ts`, never the committed file.
- **Uuid is the only stable cross-entity reference.** entt handles are not stable across a load and can
  alias between registries; cross-entity links resolve by `IdComponent` uuid (`findEntityByUuid`).
- **Some components are runtime-only — never serialized or copied:** `RelationshipComponent`'s
  `parentHandle`/`children`, `SkinnedMeshComponent.boneHandles`, `WorldTransformComponent`,
  `PoseOverrideComponent`. `IdComponent`/`WorldTransformComponent` are intentionally left unregistered.
- **`relinkHierarchy(scene)` rebuilds those caches from durable parent uuids**, *and* sanitizes
  self-parents/cycles/dangling parents, *and* resolves skinned joints. Call it after any structural
  change (load/reparent/copy) before traversing. `setParent` is the only sanctioned reparent.
  `updateWorldTransforms` walks roots-first each frame (entt views are unordered); `jointMatrices` runs
  after it.
- **Scene serde is versioned (`SceneVersion = 3`)** with migration branches in `sceneFromJson`. Bump the
  version and add a branch when the schema changes; extend `runSceneSerializationSelfTest` /
  `runSceneHierarchySelfTest` (the headless regression net) with the new behavior.
