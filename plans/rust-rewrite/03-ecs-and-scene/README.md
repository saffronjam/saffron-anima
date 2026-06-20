# 03 — ECS and Scene

The `saffron-scene` crate: the ECS world that holds every entity, the component set, the hierarchy
math, the component registry that drives serde, and the byte-compatible JSON project format — plus the
`saffron-sceneedit` crate that wraps it with editor session state (selection, fly-camera, gizmo math,
play mode, asset preview, edit smoothing). These are the pure-CPU, SDL-free, Rendering-free core the
whole control/script/animation/assets/host stack reads through.

This area is **lower risk than its size suggests** — the C++ uses a deliberately tiny slice of entt
(one `view`, one `storage()` walk, generational handles, a handful of `all_of`/`get`/`emplace`), so the
ECS swap is mechanical once the crate is chosen. The two real obligations are (1) **proving** the chosen
crate matches entt on this engine's actual access patterns (a benchmark gate, per PP-4), and (2)
reproducing the JSON project format **byte-for-byte** (field names, decimal-string uuids, enum spellings,
the v1→v4 migrations, the `componentOrder` array) so the frozen wire and on-disk `project.json` are
unchanged. Everything else (hierarchy composition, ZYX euler, the play duplicate, the gizmo math) is a
faithful glam port.

## 1. The ECS crate decision (gated)

**Decision: `hecs`, contingent on the benchmark gate in `phase-2`; `bevy_ecs` standalone is the fallback
the gate selects only if `hecs` fails the per-frame iteration target or the `PoseOverrideComponent` churn
path.** This follows the feasibility study's §4.1 verdict and PP-4's charter: pick by measurement, not
folklore.

Why `hecs` is the default:

- **The real entt surface is tiny and iteration-shaped.** Across the entire engine tree the entt API is
  exactly: one `registry.view<C...>()` (in `forEach`), one `registry.storage()` walk (in
  `serializeEntity`), plus `valid` / `all_of` / `get` / `try_get` / `emplace` / `emplace_or_replace` /
  `remove` / `destroy` / `create` / `clear` / `type_hash`. **No** groups, **no** entt signals, **no**
  `observer`, **no** `snapshot`. `hecs`'s `World::query::<(&C, ...)>()` is a near-1:1 for `forEach`, its
  `Entity` is a generational handle (`World::contains` == `valid`), and `World::query_one`/`get` cover the
  rest. The minimalism matches the Go-style free-function idiom the C++ already uses.
- **The hot per-frame paths are iteration-dominated** (`forEach` for transform sync / draw enumeration /
  light gather, `update_world_transforms`, `joint_matrices`, `primary_camera`), which is archetype
  storage's strength. The paths that favor entt's sparse-set (`relink_hierarchy`, the `enter_play`
  duplicate, `PoseOverrideComponent` add/remove) are **edge-triggered, not per-frame** — except the one
  per-frame structural churn: `PoseOverrideComponent` is `emplace_or_replace`d / `remove`d every frame on
  every animated bone (`animation.cpp:68,754`; `physics.cpp:1366`). That single churn site is the one
  thing the benchmark must clear; if archetype moves cost too much there, the gate escalates to
  `bevy_ecs` (which offers `#[component(storage = "SparseSet")]` to make exactly that component O(1)
  add/remove while keeping iterated components in columns).

Rejected (per PP-4 charter, not re-litigated): `flecs_ecs` (alpha, soundness hole, re-adds a C-FFI dep),
`legion` (unmaintained), and writing our own (no measured requirement). The benchmark gate phase is what
turns "default `hecs`" into a locked decision.

**ECS is wrapped, not leaked.** Whichever crate wins, it is an *internal* detail of `saffron-scene`: the
public surface is `World`/`Entity`/the `for_each` family/the component structs. No downstream crate names
`hecs::` (or `bevy_ecs::`) directly — this preserves the "tiny surface" property and keeps the fallback
swap a one-crate change. This mirrors the C++ where `Entity` is "a bare entt handle" but every consumer
goes through the `sa::` free functions.

## 2. The world + generational-handle model

The C++ `Scene` (`scene.cppm:578`) is `{ entt::registry registry; SceneEnvironment environment; const
AssetCatalog* catalog; }`, and `Entity` (`scene.cppm:588`) is a bare `entt::entity` handle, with the
scene passed explicitly to every free function (Go-style "pass the world"). The Rust port keeps this
shape:

- `pub struct Scene { world: World, pub environment: SceneEnvironment, pub catalog: Option<Arc<AssetCatalog>> }`
  where `World` is the wrapped ECS. `catalog` was a borrowed raw pointer (`const AssetCatalog*`, set
  per-frame, not owned/serialized); it becomes `Option<Arc<AssetCatalog>>` (read-shared catalog, per
  PP-1 Ref policy bucket 1) so there is no lifetime tangle — the asset layer constructs the catalog and
  hands the scene a shared handle. It is never serialized.
- `pub struct Entity(hecs::Entity)` (or the bevy equivalent) — a `Copy` newtype wrapper, so it never
  leaks the ECS type and `Entity` stays the lightweight, copyable handle the C++ promised.
- The component access functions stay free: `add_component`, `get_component`, `get_component_mut`,
  `has_component`, `remove_component`, `valid`, `for_each`, `find_entity_by_uuid`. In Rust these are
  methods on `Scene` where they read naturally (`scene.for_each::<(&Transform, &Camera), _>(|e, (t, c)| …)`)
  — PP-1 drops the free-function-over-method dogma — but the *shape* (generic over component tuples,
  callback receives `(Entity, &mut C…)`) is preserved.

The two entt sites that need explicit thought:

- **`forEach<C...>`** (`scene.cppm:730`) → a generic `for_each` over a `hecs::Query` tuple. The callback
  contract is `fn(Entity, components…)`; the C++ note that "entt views are unordered" carries (iteration
  order is never depended on — roots-first ordering comes from the hierarchy walk, not the view).
- **`serializeEntity`'s `registry.storage()` walk** (`scene.cppm:1494`) → there is no portable
  "iterate every component type present on this entity" in `hecs`. This is solved structurally by the
  component registry (§3): instead of walking ECS storages and joining by `type_hash`, the Rust port
  walks the **registry rows** and asks each `has(scene, entity)` — producing the identical
  `{ "ComponentName": {...} }` object. This is a *better* shape (no `type_hash` join, no reliance on an
  ECS storage-introspection API some crates lack) and is the explicit re-architecture PP-4 calls for.

## 3. The component registry (the `std::function` itable → a fn-pointer table)

The C++ `ComponentTraits` (`scene.cppm:1209`) is a struct of `std::function` fields — a hand-rolled
Go-interface itable — synthesized once per type by the `registerComponent<C>` template
(`scene.cppm:1301`) from the generic `add/get/has/remove` plus a `toJson`/`fromJson` pair. The
`ComponentRegistry` (`scene.cppm:1224`) holds the rows plus `byId`/`byName` maps. This drives
`serializeEntity` / `deserializeEntity`, `presentComponentNames`, the component-order logic, and the
editor's `add-component`/`remove-component`.

Per PP-1, this "per-type registration record keyed by type" maps to **a registration table of
fn-pointers** (PP-1 idiomRules: "per-type registration record → a registration table of
fn-pointers/`Box<dyn Fn>` keyed by type, one-place registration via PP-7 macro/derive"). The Rust shape:

```
struct ComponentTraits {
    name: &'static str,
    removable: bool,
    has:        fn(&Scene, Entity) -> bool,
    add_default:fn(&mut Scene, Entity),
    remove:     fn(&mut Scene, Entity),
    copy_to:    fn(&Scene, Entity, &mut Scene, Entity),
    serialize:  fn(&Scene, Entity) -> serde_json::Value,
    deserialize:fn(&mut Scene, Entity, &serde_json::Value) -> Result<()>,
}
```

Each closure is a *monomorphic fn-pointer* generated by a generic `register_component::<C>()` that closes
over `C` exactly as the C++ template does — no per-type hand-writing. `byId` keyed on `type_hash` becomes
keyed on `TypeId::of::<C>()` (Rust's stable in-process type identity, the direct analogue of entt's
`type_hash`); `byName` stays a `HashMap<String, usize>`. `drawInspector` is **dropped entirely** (NO
LEGACY): the C++ field was always a no-op in the headless host (the inspector is the React editor), so it
carries no behavior — the registry exists purely for serialize/deserialize/has/order.

**The single registration site** is `register_builtin_components` (the Rust port of
`scene_edit_components.cpp`'s `registerBuiltinComponents`, `scene_edit_components.cpp:18`) — 24 component
types registered once. PP-7 decides whether this becomes a derive-macro/`inventory` collection vs the
explicit list; this area's contract is only that adding a component touches **one** place. The C++
AGENTS.md warns that missing the registration site means a component "silently never serializes" — the
Rust port should make that a *compile-or-test* failure (a registry-completeness `#[test]` that asserts
every `#[derive(Component)]`-tagged struct is registered).

## 4. The component set + JSON project serde (byte-compatible)

The 24 serialized component structs (`scene.cppm:32`–`408`) plus `SceneEnvironment` (`scene.cppm:563`),
`AtmosphereSettings` (`scene.cppm:545`), `AssetCatalog`/`AssetEntry` (`scene.cppm:441`–463) port as plain
Rust structs with `glam` fields (`Vec3`/`Vec4`/`Quat`/`Mat4`/`BVec3`). The runtime-only,
never-serialized components (`RelationshipComponent`'s caches, `WorldTransformComponent`,
`PoseOverrideComponent`, `SkinnedMeshComponent.boneHandles`) stay unregistered exactly as today.

The **frozen wire contract** is the byte-format obligation. The C++ serde lives in the *hand-maintained*
`scene_component_serde.generated.cpp` (727 lines, authored in `gen.ts`'s `emitSceneSerde`, NOT
schema-driven). The Rust port must reproduce its output exactly. Locked details, each load-bearing:

- **Vectors are named objects, never positional:** `vec3` → `{"x","y","z"}`, `vec4` →
  `{"x","y","z","w"}`, `bvec3` → `{"x","y","z"}` booleans (`scene.cppm:1123`–1141). Defaults differ by
  field (vec3 default 0, vec4 default 1) — the `jsonF32Or` defaults must match per field.
- **Uuids are decimal *strings*** via `uuidToJson` (the 2^53 JS-safe contract); readers accept
  string-or-number via `jsonU64Or` / `u64FromJson` (`scene_component_serde.generated.cpp:36`). This is the
  saffron-json imperative helper from PP-1; `MaterialAssetComponent`/`ModelInstanceComponent` use a
  hand-inlined `std::to_string` emit + string-or-number read (`scene_edit_components.cpp:39,59`) — same
  contract, must match.
- **Enums are lowercase string names**, not integers, with a default-on-unknown read: `SkyMode`
  ("color"/"texture"/"procedural"), `AnimationPlayer::Wrap` ("once"/"loop"/"pingpong"),
  `Transition` ("inertialize"/"crossfade"), `Rigidbody::Motion` ("static"/"kinematic"/"dynamic"),
  `Collider::Shape` ("box"/"sphere"/"capsule"/"convexhull"/"mesh"), `BonePhysics::Joint`
  ("fixed"/"hinge"/"swingtwist"/"free"). Key spellings like `"near"`/`"far"` (not `nearPlane`) and the
  exact per-component key set must match.
- **`inverseBind` matrices serialize as 16-element flat float arrays in column-major glm order**
  (`scene_component_serde.generated.cpp:411`); `glam::Mat4::to_cols_array` is the byte match.
- **`ScriptSlot.overrides` is an opaque JSON object** passed through verbatim (`scene.cppm:342`) — a
  `serde_json::Value` field, defaulted to `{}`, with non-object inputs coerced to `{}`.
- **The scene document** is `{version, environment, entities:[{id, components, componentOrder}]}`
  (`scene.cppm:1532`); `SceneVersion = 4`. The migrations in `sceneFromJson` (`scene.cppm:1551`) must port
  exactly: v1 has no `environment` (defaulted), pre-v3 has no `Relationship` (every entity → root),
  pre-v4 has no `componentOrder` (derive canonical), unknown component → warn+skip, dangling/self/cycle
  parent → root+warn (in `relinkHierarchy`).

PP-7 owns *how* this is emitted (serde derives + the `serde_with`/imperative-helper choices); this area
owns the structs, the registry wiring, the document assembly (`scene_to_json`/`scene_from_json`/
`write_scene`/`read_scene`, `scene.cppm:1532`–1654), and the contract test that asserts byte-equality
against captured C++ fixtures. The C++ `runSceneSerializationSelfTest` (`scene.cppm:1658`) is the oracle
ported into `#[test]`s (NO in-engine self-test — PP-1).

## 5. Hierarchy + transform math

A faithful glam port of the hierarchy core, all pure math:

- `transform_matrix` (T·R·S, Euler-XYZ → quat, `scene.cppm:410`), `local_matrix` (pose-override-aware,
  `scene.cppm:858`), `compose_world_matrix` (parent-chain walk, `scene.cppm:870`), `world_matrix`/
  `world_translation`/`world_rotation` (`scene.cppm:889`–913).
- `relink_hierarchy` (`scene.cppm:762`): rebuilds `parentHandle`/`children` caches from durable parent
  uuids, defaults a root `Relationship` onto entities missing one, sanitizes self-parents / dangling /
  cycles to root with a warning, and resolves `SkinnedMeshComponent.boneHandles`. Call after any
  structural change. O(N).
- `update_world_transforms` (`scene.cppm:920`): roots-first recursive write of `WorldTransformComponent`
  (full mat4 to preserve non-uniform parent scale); `joint_matrices` (`scene.cppm:957`):
  `world(bone) · inverseBind` per joint, identity for unresolved.
- `set_parent` (`scene.cppm:1016`): the only sanctioned reparent — refuses self/cycle, `keep_world`
  rebases the local TRS, calls `relink_hierarchy`. `set_local_from_matrix` (`scene.cppm:995`) decomposes
  TRS via `glam` (the C++ uses `glm::decompose`; glam's `Mat4::to_scale_rotation_translation` is the
  analogue, returning false-equivalent on a non-decomposable matrix).
- **`quat_to_euler_zyx`** (`scene.cppm:984`): the numerically stable Rz·Ry·Rx Euler extraction. glam has
  no `extractEulerAngleZYX`, so this is **hand-ported** matrix extraction (the feasibility study flags
  "ZYX euler stability glam doesn't give for free"); covered by a dedicated `#[test]` against C++ values
  including the yaw ±90° degenerate case.
- `destroy_entity` (`scene.cppm:637`, subtree gather before destroy), `create_entity`
  (`scene.cppm:624`, seeds Id/Name/Transform/Relationship/ComponentOrder), `primary_camera`/
  `camera_projection` (`scene.cppm:1093`,1116), `model_root_of`/`animatable_descendant` (`scene.cppm:673`,
  707), `find_entity_by_uuid` (`scene.cppm:742`).

## 6. SceneEdit: session state, play, gizmo, camera, preview

`saffron-sceneedit` wraps a `Scene` with the editor's mutable session state. It depends on
`{core, signal, scene, json}` — **no Rendering, no SDL** (the gizmo *geometry* `buildNativeGizmo` lives
in the host; only the hit-test/projection/drag *math* lives here, per the C++ split). Locked pieces:

- **`SceneEditContext`** (`scene_edit_context.cppm:213`): the big session struct — `scene`, `registry`,
  `selected`, `onSelectionChanged` (a `SubscriberList<Entity>` from saffron-signal), the project path
  fields, the fly-camera, the version stamps (`sceneVersion`/`selectionVersion`/`playVersion`/
  `animationVersion` — the control-plane diff-poll keys, every bump preserved), the gizmo op/space, the
  smoothing queues, play state, and the asset-preview block. The C++ heap-owns it
  (`newSceneEditContext`/`destroySceneEditContext`, `scene_edit_context.cpp:94`) to keep the heavy
  destructor out of the client TU — in Rust it is just an owned struct (`Default`/`new`), and Drop is
  automatic; the heap-ownership dance disappears (a PP-3 subtraction).
- **`active_scene`** (`scene_edit_context.cppm:292`): the single sanctioned scene accessor — preview view
  → play duplicate → authored scene. The Rust signature returns `&mut Scene`; the three-way branch is a
  match on `(previewScene & previewActiveView, playState)`. Nothing else may branch to pick a scene. The
  `Option<Scene>` for `playScene`/`previewScene` is a clean idiomatic fit for the C++ `std::optional<Scene>`.
- **Play mode** (`scene_edit_play.cpp`): the `Edit → Playing ↔ Paused → Edit` machine. `enter_play`
  duplicates the scene via **`scene_to_json` then `scene_from_json`** (NOT a `World::clone` — the JSON
  round-trip *is* the duplicate, "what a save/load would produce", sharing the catalog `Arc`); `stop_play`
  drops the duplicate (the discard is the restore — no undo). `tick_play` gates on state + `stepFrames`,
  clamps `dt` to `PlayMaxDelta`, fixed `PlayFixedStep = 1/60` for stepped frames, and invokes the
  `simTick` seam. `simTick` is a `std::function` the host fills (points at the script runtime) → per
  PP-1, a `Box<dyn FnMut(&mut Scene, f32)>` field (or a host-implemented trait object), keeping
  sceneedit free of script/physics deps. Selection re-resolves by uuid across the duplicate boundary.
- **Gizmo math** (`scene_edit_gizmo.cpp`, ~650 lines): `viewport_project`, `pixel_to_ndc`,
  `hit_native_gizmo`, `apply_native_gizmo_drag`, `step_native_gizmo_drag`, `ring_basis`, `gizmo_axes`,
  the `preserveChildren` rebasing (`startChildWorlds`, `snapshot_native_gizmo_start`), and the
  `tau = 0.025` exponential smoothing shared by the look-drain, gizmo drag, and `step_edit_smoothing`.
  `sync_native_gizmo` mirrors the source-of-truth `GizmoOp`/`GizmoSpace` onto the `NativeGizmo` per-frame
  mirror.
- **Camera** (`scene_edit_camera.cpp`): fly-cam forward/view, `update_scene_edit_camera` (look smoothing
  + WASD), `scene_edit_camera_to_json`/`from_json` (round-tripped into `project.json` by the control
  caller).
- **Component registration** (`scene_edit_components.cpp`): `register_builtin_components` — the one
  canonical registration site for all 24 components.

The C++ `runPlayModeSelfTest` (`scene_edit_play.cpp:232`) is the oracle ported into `#[test]`s.

## 7. Ref / ownership sites in this area (per PP-1 §3)

- `Scene.catalog`: `Option<Arc<AssetCatalog>>` — read-shared, never serialized (was a borrowed
  `const AssetCatalog*`). Bucket 1.
- `SceneEditContext.playScene` / `.previewScene`: `Option<Scene>` — owned, single-threaded. No shared
  handle; the scene is moved in/out. Bucket: plain ownership.
- `SceneEditContext.simTick`: `Box<dyn FnMut(&mut Scene, f32)>` (!Send, single-thread host loop). Bucket:
  trait-object/closure, not a Ref.
- `onSelectionChanged` / `onPlayStateChanged`: the saffron-signal `SubscriberList<T>` (!Send `FnMut`
  handlers, snapshot dispatch). Not a Ref.
- No `Arc<Mutex>` in this area — it is single-threaded pure CPU (the multi-thread shared-mutable sites are
  all in rendering/assets per PP-1, not here).

## Grounding (real files / symbols)

| What | File (`engine-old/source/saffron/`) | Symbols |
|---|---|---|
| Component structs (24 serialized + runtime-only) | `scene/scene.cppm` | `NameComponent`, `TransformComponent`, `RelationshipComponent`, `WorldTransformComponent`, `ComponentOrderComponent`, `MeshComponent`, `MaterialComponent`, `MaterialSetComponent`/`MaterialSlot`, `MaterialAssetComponent`, `ModelInstanceComponent`, `ScriptComponent`/`ScriptSlot`, `CameraComponent`, `DirectionalLightComponent`, `PointLightComponent`, `SpotLightComponent`, `ReflectionProbeComponent`, `SkinnedMeshComponent`, `BoneComponent`, `AnimationPlayerComponent`, `PoseOverrideComponent`, `FootIkComponent`/`FootChain`, `BonePhysicsComponent`/`BonePhysics`, `RigidbodyComponent`, `ColliderComponent`/`PhysicsMaterial`, `KinematicBonesComponent`, `CharacterControllerComponent` |
| World + handle + access | `scene/scene.cppm` | `Scene`, `Entity`, `valid`, `addComponent`, `getComponent`, `hasComponent`, `removeComponent`, `createEntity`, `destroyEntity`, `forEach`, `findEntityByUuid` |
| entt surface (the whole of it) | `scene/scene.cppm`, `animation/animation.cpp` | `registry.view<C...>` (`scene.cppm:733`), `registry.storage()` (`scene.cppm:1494`), `emplace_or_replace` (`animation.cpp:754`), `type_hash` (`scene.cppm:1307`), `entt::null`/`valid`/`all_of`/`get`/`try_get` |
| Hierarchy + transform math | `scene/scene.cppm` | `transformMatrix`, `localMatrix`, `composeWorldMatrix`, `worldMatrix`, `worldRotation`, `relinkHierarchy`, `updateWorldTransforms`, `jointMatrices`, `quatToEulerZYX`, `setLocalFromMatrix`, `setParent`, `primaryCamera`, `cameraProjection`, `modelRootOf`, `animatableDescendant` |
| Component registry (itable) | `scene/scene.cppm` | `ComponentTraits`, `ComponentRegistry`, `registerComponent<C>`, `findById`, `findByName`, `presentComponentNames`, `componentOrder`/`setComponentOrder`/`sortComponentOrder`/`appendComponentOrder` |
| Scene serde + versioning | `scene/scene.cppm` | `SceneVersion=4`, `serializeEntity`, `deserializeEntity`, `sceneToJson`, `sceneFromJson`, `writeScene`, `readScene`, `runSceneSerializationSelfTest`, `runSceneHierarchySelfTest` |
| Component serde bodies (byte contract) | `scene/scene_component_serde.generated.cpp` | `*ToJson`/`*FromJson`, `vec3ToJson`/`vec3FromJson`, `u64FromJson`, `skyModeName`/`skyModeFromName`, `atmosphereToJson`/`atmosphereFromJson`, enum name↔value helpers |
| Assets catalog types | `scene/scene.cppm` | `AssetType`, `Colorspace`, `AssetEntry`, `AssetCatalog`, `findAsset`, `putAsset`, `renameAsset`, `uniqueName` |
| Environment / sky | `scene/scene.cppm` | `SkyMode`, `AtmosphereSettings`, `SceneEnvironment`, `environmentToJson`/`environmentFromJson` |
| SceneEdit context + session | `sceneedit/scene_edit_context.cppm`, `sceneedit/scene_edit_context.cpp` | `SceneEditContext`, `activeScene`, `previewing`, `setSelection`, `newSceneEditContext`/`destroySceneEditContext`, version stamps, `ScriptInputState`/`deriveScriptInputEdges` |
| Play mode | `sceneedit/scene_edit_play.cpp` | `PlayState`, `enterPlay`, `pausePlay`, `resumePlay`, `stepPlay`, `stopPlay`, `tickPlay`, `renderCameraView`, `PlayFixedStep`/`PlayMaxDelta`, `pushScriptError`/`pushScriptLog`, `runPlayModeSelfTest` |
| Gizmo math | `sceneedit/scene_edit_gizmo.cpp` | `syncNativeGizmo`, `viewportProject`, `pixelToNdc`, `hitNativeGizmo`, `applyNativeGizmoDrag`, `stepNativeGizmoDrag`, `ringBasis`, `gizmoAxes`, `handleAxis`, `gizmoPlaneCorners`, `snapshotNativeGizmoStart`, `materialSmoothEntryFor`/`transformSmoothEntryFor`, `stepEditSmoothing` (`tau=0.025`) |
| Fly-camera | `sceneedit/scene_edit_camera.cpp` | `sceneEditCameraForward`, `sceneEditCameraView`, `updateSceneEditCamera`, `sceneEditCameraToJson`/`fromJson` |
| Component registration site | `sceneedit/scene_edit_components.cpp` | `registerBuiltinComponents` (24 `registerComponent<C>` calls) |
| Editor module guidance | `scene/AGENTS.md`, `sceneedit/AGENTS.md` | one-registration-site rule, runtime-only components, version-stamp rules, `activeScene` rule, play-is-the-restore |

## Phases

1. `phase-1-scene-crate-skeleton-and-ecs-adapter.md` — `saffron-scene` crate, the wrapped `World`/`Entity`,
   the component-access surface, `create/destroy/valid/for_each/find_by_uuid`. Compiles green; smoke `#[test]`.
2. `phase-2-ecs-benchmark-gate.md` — **(go/no-go)** port `for_each` + a few-thousand-entity scene + the
   `PoseOverrideComponent` churn path; benchmark vs entt; lock `hecs` (or escalate to `bevy_ecs`).
3. `phase-3-component-structs-and-glam.md` — the 24 component structs + environment/atmosphere/catalog
   types with glam fields; runtime-only components unregistered.
4. `phase-4-hierarchy-and-transform-math.md` — `transform_matrix`/`relink_hierarchy`/
   `update_world_transforms`/`joint_matrices`/`set_parent`/`quat_to_euler_zyx`/`primary_camera`; ported
   `runSceneHierarchySelfTest` as `#[test]`s.
5. `phase-5-component-registry.md` — the fn-pointer `ComponentTraits` table + `register_component::<C>`
   + the component-order logic + the registry-completeness test.
6. `phase-6-component-serde-bytecompat.md` — every `*_to_json`/`*_from_json` body reproducing the C++
   wire bytes; uuid-string / enum-name / named-vector contracts; captured-fixture byte-equality test.
7. `phase-7-scene-document-and-migrations.md` — `scene_to_json`/`scene_from_json`/`write_scene`/
   `read_scene` + the v1→v4 migrations; ported `runSceneSerializationSelfTest`.
8. `phase-8-sceneedit-crate-and-context.md` — `saffron-sceneedit` crate, `SceneEditContext`, version
   stamps, `set_selection`, `active_scene`/`previewing`, `register_builtin_components`, `ScriptInputState`.
9. `phase-9-fly-camera.md` — the editor fly-camera math + serde.
10. `phase-10-play-mode.md` — the play state machine + the JSON-roundtrip duplicate + `tick_play`/`simTick`
    seam + script error/log rings; ported `runPlayModeSelfTest`.
11. `phase-11-gizmo-math-and-smoothing.md` — projection/hit-test/drag math, `preserveChildren` rebasing,
    the `tau=0.025` smoothing steppers, `sync_native_gizmo`.
