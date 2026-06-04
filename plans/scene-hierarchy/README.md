# Scene Hierarchy Plan

This plan adds scene-tree (sub-entity) support to SaffronEngine's flat entt scene: a
Bevy/Unity-style parent/child relationship carried by a single `RelationshipComponent`, a
derived per-frame world transform, uuid-based serialization, and a React tree-view outliner
driven over the DTO-first control plane. It is dependency-ordered: the engine relationship +
world-transform machinery lands first, renderer/gizmo/pick adopt the world transform, then the
control commands and editor tree, with environment-in-tree and skeleton handled as deliberate,
scoped decisions. It supersedes the editor's flat-list/no-parenting non-goal (set by the completed
`typescript-ui-migration` plan; the flat list is documented in
`docs/content/explanations/ui-and-editor/hierarchy-panel.md`).

## Status convention

Each phase file carries a `**Status:**` line (`NOT STARTED` / `IN PROGRESS` / `COMPLETED`).
Mark a phase `COMPLETED` when its work is done and validation-clean; delete a phase file only
*after* it is `COMPLETED` and merged.

## Target shape

End state: scene (a hidden synthetic root) -> top-level entities -> child entities, with
components shown in the Inspector (the Unity model) rather than as tree children, and a pinned
non-deletable `Environment` sentinel node at the tree root that opens the existing Environment
editor.

IN SCOPE:

- `RelationshipComponent` (parent uuid + runtime `children` / `parentHandle` caches).
- A cached `WorldTransformComponent` via a once-per-frame `updateWorldTransforms` pass.
- World-preserving, cycle-guarded `set-parent`; recursive `destroy`.
- `SceneVersion` 2 -> 3 with v1/v2 migration.
- `parentId` on `list-entities` + a `set-parent` command + generated DTO/schema + contract test +
  `se` CLI.
- The React tree with drag-reparent.
- Optional selected-entity component subrows.

OUT OF SCOPE / deferred:

- `SceneEnvironment` stays **global** `Scene` state — it is **not** promoted to an entity, only
  surfaced as a client-side sentinel node, honoring the completed skybox plan's decision (recorded
  in the scene.cppm:242-244 comment and `docs/content/explanations/image-based-lighting/`).
- Sibling reorder (children are unordered in v1).
- Undo/redo.
- Full per-entity component subrows.
- A dirty-flag incremental world-transform scheme.
- Skinning, which is represented as bones-as-entities but whose implementing phase is
  forward-looking and research-gated because skinning is unbuilt.

## Phase map

| # | Phase | File | Depends on |
|---|-------|------|------------|
| 0 | Research and architecture | `phase-0-research-and-architecture.md` | - |
| 1 | Relationship component + cached world-transform propagation | `phase-1-engine-relationship-and-world-transform.md` | - |
| 2 | Serialize parent by uuid, two-pass resolve, SceneVersion 3 migration | `phase-2-engine-serialize-by-uuid-and-version-3.md` | 1 |
| 3 | Adopt world transform across renderer, picking, camera, gizmo, billboards | `phase-3-renderer-gizmo-pick-adopt-world-transform.md` | 1 |
| 4 | Control-plane: parentId on list-entities, set-parent command, schema + contract test + se CLI | `phase-4-control-hierarchy-commands-and-schema.md` | 1, 2 |
| 5 | Editor: tree-view outliner with drag-reparent and the pinned Environment node | `phase-5-editor-tree-view.md` | 4 |
| 6 | Optional: selected-entity component subrows in the tree | `phase-6-editor-component-subrows-optional.md` | 5 |
| 7 | Forward-looking: glTF skin import + bones-as-entities + skinning pass (research-gated) | `phase-7-skeleton-bones-forward-looking.md` | 1, 2, 3 |

## Current anchors

- The scene is a **flat** entt registry; entities carry `IdComponent` / `NameComponent` /
  `TransformComponent` only, with **no** parent/child anything
  (`scene.cppm:270-313`). `createEntity` (`scene.cppm:306-313`) emplaces `Id` + `Name` +
  `Transform`.
- `transformMatrix(TransformComponent)` (`scene.cppm:119`) builds a **local** `T*R*S` matrix;
  consumers — `draw` (`assets.cppm:822`), `pick` (`assets.cppm:1061`), point/spot lights
  (`assets.cppm:727-766`), reflection probes (`assets.cppm:916/:927`), `primaryCamera`
  (`scene.cppm:345`), the gizmo (`scene_edit_gizmo.cpp`), and the host billboards
  (`host.cppm:209/:227-228`) — all read the **local** transform as if it were world. There is no
  world-transform derivation anywhere.
- `sceneFromJson` (`scene.cppm:569`) runs a create+deserialize loop over the entity array and an
  **unused** uuid -> handle resolve hook (`scene.cppm:619-621`: "the hook is ready for them") —
  the exact insertion point for parent-uuid resolution. `SceneVersion = 2` (`scene.cppm:422`),
  upper-bound-checked at `scene.cppm:576`.
- `ComponentTraits` / `registerComponent` (`scene.cppm:424-487`) reflect every component to
  JSON; the per-component serde bodies are generated (`scene_component_serde.generated.cpp`, from
  the catalog in `tools/gen-control-dto/gen.ts`). copy-entity round-trips each component through
  serialize→deserialize (`control_commands_scene.cpp:686-692`) and `copyTo`
  (`scene.cppm:462-468`, currently uncalled) is a naive value copy — so a children-of-handles
  field would alias/corrupt; parent must serialize **by uuid** and children must be a
  non-serialized derived cache. Registrations live in `registerBuiltinComponents`
  (`scene_edit_components.cpp:17-72`).
- Environment is **global** `SceneEnvironment` `Scene` state, **not** an entity
  (`scene.cppm:245-263`), resolved into the sky / IBL / DDGI each frame
  (`assets.cppm:903-1020`). The completed skybox plan deliberately kept it global and explicitly
  rejected a sky-mesh entity — this plan honors that and adds only a client-side sentinel tree
  node.
- The editor `HierarchyPanel.tsx` is a **flat** `entities.map` (`L95`); `editor/src/state/store.ts`
  `entities` is a flat array filled by the `sceneVersion`-gated heavy refresh behind a 50 ms fast
  lane (`store.ts:201`), skipped while `dragActive` (`store.ts:288/:304`); the completed
  `typescript-ui-migration` plan made flat-list/no-parenting an explicit non-goal which **this**
  plan supersedes.
- The control `list-entities` returns a flat `EntityList` of `EntityRef{id (decimal string), name}`
  (`control_commands_scene.cpp:115-124`); the DTOs live in `control_dto.cppm` (`EntityRef` :36-40,
  returned by ~12 commands) and `tools/gen-control-dto/gen.ts` generates the serde, the TS
  protocol, and closed (`additionalProperties: false`) schemas into
  `schemas/control/openrpc.generated.json` — ids are u64-as-decimal-string via `WireUuid`. The
  contract test (`tools/check-control-schema/check.ts`) asserts u64 precision (its id-key alphabet
  at `check.ts:134` must learn `parentId`). `resolveEntity` hardcodes the `entity` key
  (`control_server.cpp:72`).
- The engine has **no** skinning / skeletal animation (Status: skinned mesh not built), no
  physics, and no undo/redo.
