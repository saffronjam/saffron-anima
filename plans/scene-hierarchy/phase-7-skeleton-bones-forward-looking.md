# Phase 7: Forward-looking: glTF skin import + bones-as-entities + skinning pass (research-gated)

**Status:** NOT STARTED
<!-- Flip to COMPLETED when the Done-when checklist passes validation-clean (round-trip + headless deform + se reparent/inspect). Delete this file only after COMPLETED + merged. -->

## Goal

Represent a skeleton as real `entt` bone entities parented into the same scene tree (the Bevy/Unity/Godot model), add a `SkinnedMeshComponent { Uuid rootBone; std::vector<Uuid> bones }` that lists joints by uuid, import glTF skins 1:1 as entity nodes, and add a GPU skinning pass that deforms the mesh from per-joint matrices. This phase is **forward-looking and research-gated**: it consumes the core hierarchy (phases 1-3 — `RelationshipComponent`, `updateWorldTransforms`, `setParent`, the uuid->handle relink) but does **not** block it. Skinning is genuinely unbuilt today (no bind matrices, no joint-matrix shader, no glTF skin import), so it is the largest phase and lands last. The architecture is already decided in `plans/scene-hierarchy/phase-0-research-and-architecture.md` (bones-as-entities, joints by uuid, filterable in the outliner) — do not re-litigate it here.

## Current state

- **No skin, no skeleton, no bind matrices.** `importModel` (`engine/source/saffron/assets/assets.cppm:578`) calls `importModelWithMaterial` (`assets.cppm:580`) and emits a single mesh + its primary material; it never reads glTF nodes, `skin`, `joints`, or `inverseBindMatrices`. There is no per-node entity creation and no skeleton.
- **Mesh vertex shader is unskinned.** `engine/assets/shaders/mesh.slang` — `VertexInput` (`mesh.slang:13-18`) carries only `position`/`normal`/`uv0`; there are no `joints`/`weights` attributes. `vertexMain` (`mesh.slang:292`) transforms by `inst.model` from the per-instance `Instance { model, normalMatrix }` SSBO (`mesh.slang:128-131`, `[[vk::binding(0,2)]] instances` at `mesh.slang:137`). No joint-matrix buffer exists.
- **The draw path is per-instance, not per-vertex-skinned.** `renderScene`'s mesh loop (`assets.cppm:792`) writes `DrawItem.model`/`normalMatrix` (`assets.cppm:853-854`) → `submitDrawList` (`renderer_types.cppm:1185`) → `InstanceData { model, normalMatrix }` (`renderer_types.cppm:1141`). Nothing feeds vertices joint indices/weights.
- **Components are flat POD.** `scene.cppm` defines `NameComponent`/`IdComponent`/`TransformComponent`/`MeshComponent`/`MaterialComponent`/light + camera components only (`scene.cppm:28-95`). `registerBuiltinComponents` (`engine/source/saffron/sceneedit/scene_edit_components.cpp:17-72`) wires them to generated serde; uuid-by-ref components follow the `MeshComponent` pattern (registration `scene_edit_components.cpp:31`, generated body `meshComponentFromJson` at `scene_component_serde.generated.cpp:111-115`, `c.mesh = Uuid{ jsonU64Or(j,"mesh",0) }`).
- **Hierarchy substrate (this phase's hard dependency, delivered by phases 1-3):** `RelationshipComponent { Uuid parent; entt::entity parentHandle; std::vector<entt::entity> children }`, `updateWorldTransforms(Scene&)` writing the cached `WorldTransformComponent{mat4}` in parent-before-child order, `worldMatrix(Scene&,Entity)`, `setParent(Scene&,Entity,Entity,bool)`, recursive `destroyEntity`, and the uuid->handle resolve pass that replaced the dormant `static_cast<void>(uuidToHandle)` hook at `scene.cppm:619-621`. `createEntity` (`scene.cppm:306`) emplaces a default root `RelationshipComponent`. `SceneVersion` is bumped to 3 (`scene.cppm:422`). This phase reuses all of it unchanged; the skinned-mesh node and every bone are ordinary parented entities.
- **Wire/editor (delivered by phases 1-3):** `list-entities` (`control_commands_scene.cpp:115-124`) emits `parentId` per entry on a dedicated `EntityListEntry` DTO; the editor builds the tree client-side from the flat list + `parentId` (`editor/src/panels/HierarchyPanel.tsx`, `editor/src/state/store.ts`), ids stay strings, the reconcile poll is focus-gated and version-keyed. `set-parent` exists as a control command + `se` CLI.

## Implementation

### 1. `SkinnedMeshComponent` — joints by uuid (engine)

Add to `engine/source/saffron/scene/scene.cppm`, near `MeshComponent` (`scene.cppm:46`):

```cpp
struct SkinnedMeshComponent
{
    Uuid mesh;                    // the skinned mesh asset (same asset id space as MeshComponent.mesh)
    Uuid rootBone;                // the skeleton root entity (a bone entity in the tree)
    std::vector<Uuid> bones;      // ordered joint list; index == glTF joint index == jointMatrices[] slot
    std::vector<glm::mat4> inverseBind;  // per-joint inverse bind matrix, parallel to `bones`
};
```

- `bones` is **ordered**: entry `i` is the entity whose world transform drives `jointMatrices[i]`. `inverseBind[i]` is that joint's glTF `inverseBindMatrices[i]`. Both vectors are parallel and the same length; getting the order wrong silently corrupts deformation, so the import (step 4) is the single point that defines it.
- Joints are referenced **by uuid**, resolved to live handles through the same uuid->handle path the loader and `setParent` use (the resolve pass that replaced the dormant hook at `scene.cppm:619-621`). A skinned mesh holds no live handles — only durable uuids — so it survives reload and copy like every other ref component (`MeshComponent` precedent: registration `scene_edit_components.cpp:31`, generated serde `scene_component_serde.generated.cpp:106-115`).
- `mesh` is the renderable asset. The skinned mesh **node itself** is an ordinary entity with a `TransformComponent` + `RelationshipComponent`, but per glTF its own node transform is ignored for skinning (step 4); vertices live in skeleton/mesh space and are placed entirely by joint matrices.

### 2. Per-joint matrix derivation (engine, reuses the world-transform pass)

Add to `scene.cppm`, near `updateWorldTransforms`/`worldMatrix` (the phase-1 helpers near `transformMatrix` at `scene.cppm:119`):

```cpp
/// Fill `out` with worldMatrix(scene, bones[i]) * inverseBind[i] for each joint.
/// Caller runs this AFTER updateWorldTransforms(scene) so bone world matrices are current.
void jointMatrices(Scene& scene, const SkinnedMeshComponent& skin, std::vector<glm::mat4>& out);
```

- Implementation: for each `i`, resolve `skin.bones[i]` to its handle (via the cached `parentHandle`/uuid map already maintained for the hierarchy — do **not** re-scan), read the cached `WorldTransformComponent.matrix`, and write `out[i] = worldBone * skin.inverseBind[i]`. No new traversal: the bone entities are already in the scene tree, so `updateWorldTransforms(scene)` (run once per frame before render) has already produced their world matrices in parent-before-child order. This is the load-bearing reuse: a skeleton is animated by exactly the propagation pass the core hierarchy built.
- A dangling/unresolvable joint uuid → identity for that slot + `logWarn` once (mirrors the dangling-parent handling in the loader resolve pass).

### 3. Register + serialize `SkinnedMeshComponent` by uuid (engine + generator)

Component serde is generated: add `SkinnedMeshComponent` to the scene-component catalog in `tools/gen-control-dto/gen.ts` (the same catalog that emits `meshComponentToJson`/`FromJson`), then wire the generated named functions in `registerBuiltinComponents` (`scene_edit_components.cpp:17-72`) with a `registerComponent<SkinnedMeshComponent>` block after the lights, alongside `ReflectionProbe` (`scene_edit_components.cpp:67-71`). The host is present-only, so the registration's `drawInspector` lambda is empty like the others.

- Generated `toJson`: `{ {"mesh", c.mesh.value}, {"rootBone", c.rootBone.value}, {"bones", <array of c.bones[i].value>}, {"inverseBind", <array of 16-float mat4s>} }`. Emit each uuid as a raw u64 integer (the `MeshComponent` precedent, `meshComponentToJson`, `scene_component_serde.generated.cpp:106-115`), and the bone-uuid array as raw integers.
- Generated `fromJson`: `c.mesh = Uuid{ jsonU64Or(j,"mesh",0) }`, `c.rootBone = Uuid{ jsonU64Or(j,"rootBone",0) }`, read `bones` as a u64 array into `std::vector<Uuid>`, read `inverseBind` as a flat/nested float array into `std::vector<glm::mat4>`. Leave any resolved-handle caches default.
- `removable = true` (a renderable can drop its skin; unlike `RelationshipComponent` which is non-removable). The component round-trips through `serializeEntity`/`deserializeEntity` (`scene.cppm:511`/`530`) and `copy-entity`'s round-trip loop for free; because it stores only uuids, copy-entity — which serialize->deserializes each registered component (`control_commands_scene.cpp:686-692`, `t.deserialize(scene, fresh, t.serialize(scene, *src))`) — keeps the copy referencing the same skeleton (single-entity copy semantics, consistent with phase-3). `copyTo` (`scene.cppm:462-468`) exists but has no callers; the serialize->deserialize round-trip never emits caches, so the uuid-only surface is correct under both paths.
- **DTO-first (the wire contract obligation):** the wire contract is DTO-first — there are no hand-authored payload schemas. Adding `SkinnedMeshComponent` to the gen.ts scene-component catalog regenerates the C++ serde (`scene_component_serde.generated.cpp`), the TS protocol (`editor/src/protocol/se-types.ts`), and the schemas (`schemas/control/openrpc.generated.json` + `command-manifest.generated.json`), each as a closed object (`additionalProperties:false`, gen.ts `schemaFor`). Run `bun run tools/gen-control-dto/gen.ts` (the editor's `bun run gen:protocol` shim spawns the same generator) and commit the generated files. The contract test `tools/check-control-schema/check.ts` validates the live shape against the generated manifest/OpenRPC; add `mesh`/`rootBone`/`bones` to the `assertRawU64` allowlist regex (`check.ts:134`, today `id|mesh|albedoTexture|skyTexture|texture|entity`) so the u64-as-string-via-BigInt invariant (`check.ts:141-146`) covers the new id fields. The `se` CLI reaches the component via `inspect` with no formatter change (default pretty-JSON satisfies the minimum); a skinned mesh node is reparentable via the existing `se set-parent` like any entity.

### 4. glTF skin import → bone-entity hierarchy (engine, reuses the hierarchy import path)

Extend the import path under `importModelWithMaterial` / `importModel` (`assets.cppm:578-580`). Today it produces one mesh; skin import makes it produce a **subtree of entities** in addition:

1. **One entity per glTF node** with a `TransformComponent` (from the node's local TRS) + a `RelationshipComponent` whose `parent` is the uuid of the node's glTF parent (root nodes → parent 0). This is exactly the phase-1-3 hierarchy import path — reuse it; do not invent a second parenting mechanism. Mint a fresh `Uuid` per node and record a `gltfNodeIndex -> Uuid` map for the joint-list resolve below.
2. **Tag joint nodes.** A node referenced by `skin.joints` is a bone; carry its `inverseBindMatrices[i]` into the matching `SkinnedMeshComponent.inverseBind[i]` slot. (Joints need no marker component for correctness — they are ordinary entities listed in `bones`; an optional `BoneComponent {}` tag-only struct, registered non-serialized like `IdComponent`, makes the outliner "hide bones" filter cheap, step 6. If added, add it to the gen.ts scene-component catalog and wire it in `scene_edit_components.cpp` (regenerating serde/TS/OpenRPC/manifest) — but prefer deriving "is a bone" from membership in some `SkinnedMeshComponent.bones` if a tag proves redundant.)
3. **The skinned mesh node** gets a `SkinnedMeshComponent` with `mesh` = the imported mesh asset uuid, `rootBone` = the skin's skeleton-root node uuid, `bones` = `skin.joints` mapped through the `gltfNodeIndex -> Uuid` map **in glTF joint order**, and `inverseBind` parallel to it. Per the glTF spec the skinned node's **own** `TransformComponent` is ignored for deformation (vertices are pre-placed by joint matrices); set it to identity or keep the node transform but never compose it into `jointMatrices` — document this at the call site so it is not "fixed" later.
4. Parent the whole imported subtree under the scene root (or under a caller-supplied parent) via `setParent`/the loader relink, then call the uuid->handle resolve so `parentHandle`/`children` caches and joint resolution are live.

### 5. Skinning pass — feature-gated GPU deform (engine + shader)

Gate the entire skinned path behind a feature flag so the unskinned path is byte-for-byte unchanged.

- **Vertex attributes:** add `joints` (`uint4`, `[[vk::location(3)]]`) + `weights` (`float4`, `[[vk::location(4)]]`) to a **new** skinned `VertexInput` variant in `engine/assets/shaders/mesh.slang` (keep the existing `VertexInput` at `mesh.slang:13-18` untouched for the unskinned PSO). The skinned `.smesh` import (step 4) must write these attributes; non-skinned meshes keep the current vertex layout.
- **Joint-matrix SSBO:** add a new `[[vk::binding(N,2)]] StructuredBuffer<float4x4> jointMatrices` to the skinned shader and a skinned `vertexMain` that blends `sum_j weights[j] * jointMatrices[joints[j]]`, applies it to `input.position`/`normal`, then multiplies by `inst.model` (the node placement) as the unskinned path does at `mesh.slang:296-299`. The CPU side fills that buffer per skinned draw from `jointMatrices(scene, skin, out)` (step 2).
- **PSO/draw wiring:** add a skinned PSO variant and, in `renderScene`'s mesh loop (`assets.cppm:792`), branch entities carrying `SkinnedMeshComponent` onto the skinned draw (upload the joint buffer, bind the skinned PSO) instead of the plain `DrawItem` path at `assets.cppm:822-854`. Entities with only `MeshComponent` take the existing path verbatim. The feature gate (a renderer capability bool / build switch) makes the skinned branch a no-op when off, so a regular scene renders identically.
- **Research gate (mandatory before committing the GPU pass):** validate `worldMatrix(bone) * inverseBind` blending against a reference rigged glTF (e.g. a simple two-bone asset) — confirm joint **order**, `inverseBindMatrices` handedness/column-major load, and the ignored-node-transform rule reproduce the reference deformation in a headless render before the pass is merged. Getting joint order or inverseBind wrong corrupts silently.

### 6. Editor — bones as tree nodes with a "hide bones" filter (editor)

Bones are real entities, so they already arrive in `list-entities` with `parentId` and render as ordinary tree rows in the phase-1-3 client-built tree — no new fan-out call, ids stay strings, the focus-gated poll is unchanged. Add:

- A **`hideBones` toggle** in the hierarchy header (a plain UI boolean in `editor/src/state/store.ts`, outside the `sceneVersion`-gated poll like the other UI flags) that filters bone rows out of the derived tree in `editor/src/panels/HierarchyPanel.tsx`. "Is a bone" is derived client-side: an entity whose id appears in some skinned mesh's `bones`/`rootBone` (read from the selected entity's `inspect` `SkinnedMesh` component, already in `componentsBySelected`) or, if the optional `BoneComponent` tag is added (step 4), a flag on the list entry. Prefer the cheap source that needs no extra control calls.
- Filtering hides bone **rows** but their children must re-anchor to the nearest visible ancestor when hidden, so a skinned mesh parented under a bone does not vanish — fold this into the existing client-side tree builder, not a server change.
- **X11 overlay constraint:** all of this is sidebar DOM (rows, the toggle, the filter). Do not introduce drag images or portal'd indicators that stray over the X11 viewport rect; keep the existing Radix-anchored context menu and in-DOM affordances.

## Done when

- [ ] A rigged glTF imports as a bone-entity hierarchy: one entity per node, joint nodes tagged/listed, parented via `RelationshipComponent`, and the renderable carries a `SkinnedMeshComponent` whose `bones`/`inverseBind` are in glTF joint order. Verifiable headless: import the reference asset, then `se list-entities` shows the bone subtree with `parentId` links.
- [ ] Joints resolve by uuid: `se inspect` on the skinned mesh node returns a `SkinnedMesh` component whose `rootBone` + `bones` uuids resolve to entities present in `list-entities`.
- [ ] Round-trip a parented + skinned scene through `writeScene`/`readScene` (extend `runSceneSerializationSelfTest` at `scene.cppm:660`, or an `e2e` test): the bone hierarchy (parent uuids), `SkinnedMeshComponent.bones`/`inverseBind`, and the resolved `parentHandle`/joint links are identical after reload; a pre-`SceneVersion`-3 doc with no skin still loads clean (all roots, no skin).
- [ ] `se set-parent <bone> <other>` then `se inspect <bone>` shows the bone reparented (a bone is an ordinary entity); the skinned mesh still deforms via the new world chain.
- [ ] A skinned mesh deforms via per-joint matrices in a headless render (`SAFFRON_EXIT_AFTER_FRAMES`), matching the reference asset within tolerance (the research-gate comparison).
- [ ] The unskinned render path is unchanged with the feature gate off: a non-skinned scene's headless output and `render-stats` are identical to before this phase.
- [ ] Bones are filterable in the outliner: the `hideBones` toggle removes bone rows without dropping their non-bone descendants, and survives a `sceneVersion` poll refresh (it is plain UI state).
- [ ] The generated `command-manifest.generated.json` + `openrpc.generated.json` reflect `SkinnedMesh`, the contract test `tools/check-control-schema/check.ts` validates the live component against the generated manifest/OpenRPC, `assertRawU64` (`check.ts:134`) covers the new id fields, the gen.ts outputs are regenerated (`bun run tools/gen-control-dto/gen.ts`) and committed, and the headless run is validation-clean.
- [ ] Engine builds clean in the `saffron-build` toolbox; `make check` / `make e2e` pass.

## Risks / seams

- **Skinning is genuinely unbuilt** — no bind matrices, no joint-matrix shader, no glTF skin import. This is the largest, research-gated phase; it must not block or gate the core hierarchy (phases 1-3 ship and are useful without it).
- **Joint order / inverseBind correctness is silent.** Wrong glTF joint ordering, a column-major/handedness mistake loading `inverseBindMatrices`, or composing the skinned node's own transform (which the spec says to ignore) corrupts deformation with no error. The research gate against a reference asset is mandatory before the GPU pass merges; `bones` and `inverseBind` must be filled at exactly one site (step 4) in lockstep.
- **Bone-entity counts can be large.** A rig is dozens-to-hundreds of bone entities, all flowing through `updateWorldTransforms` and the outliner. If the unconditional per-frame flatten pass or the tree render strains, the **dirty-flag world-transform optimization deferred from phase 1** becomes necessary — this phase is the likely trigger. Keep the flatten pass and the joint resolve free of per-call uuid re-scans (use the cached handles) so cost stays O(entities), not O(entities²).
- **Feature gate must be airtight.** The skinned PSO/SSBO/vertex-layout branch must be fully bypassed when off, or a regular scene regresses. Keep the existing `VertexInput`, `Instance`, and `vertexMain` (`mesh.slang:13-18`, `:128-131`, `:292`) untouched and add skinned variants beside them.
- **Schema/contract drift.** Adding `SkinnedMesh` to the gen.ts scene-component catalog touches the generated serde + TS protocol + `openrpc.generated.json`/`command-manifest.generated.json` + `check.ts` (including the `assertRawU64` allowlist at `check.ts:134`). Landing any subset without regenerating breaks the contract test or the editor protocol types.
- **Copy/reparent semantics.** `copy-entity` serialize->deserializes the `SkinnedMeshComponent` (`control_commands_scene.cpp:686-692`), and because it stores only uuids the copied skinned node references the **same** skeleton (single-entity copy, per phase-3) — deep subtree copy with remapped joint uuids is out of scope. Reparenting a bone is allowed (bones are entities) but moving a joint out of the skeleton it serves changes its world matrix and thus the deformation — acceptable and authored deliberately via `set-parent`.
