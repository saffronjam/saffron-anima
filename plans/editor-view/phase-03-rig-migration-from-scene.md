# Phase 3 — rig migration from spawned scenes

**Status:** NOT STARTED

## Goal

Projects imported before phase 1 have no `.srig` and no catalog links — but everything needed to
synthesize them is already persisted **in the scene**: the spawned bone entities and the
`SkinnedMeshComponent`. Add the inverse of `spawnSkinnedModel`: walk a spawned rig back into an
`ImportedRig` + links, write the sidecar, and stamp the catalog — so old projects open the rig
editor without re-importing source files (which is impossible anyway: the catalog stores no source
glTF path).

## What exists to build on

- The scene persists the full rig: `skinnedMeshComponentToJson`/`FromJson` round-trip `mesh` (the
  catalog uuid — the link back), `rootBone`, `bones[]` **in glTF joint order**, and `inverseBind`
  (`scene_component_serde.generated.cpp:403-461`); bone entities persist `NameComponent`,
  `TransformComponent` (local TRS), and the durable `RelationshipComponent.parent` uuid.
- `findEntityByUuid` (`scene.cppm:576`) and the relink caches (`relinkHierarchy`,
  `scene.cppm:665-681`) resolve uuids → entities.
- The forward direction it inverts: `spawnSkinnedModel` (`assets.cppm:2095-2169`) — one entity per
  `ImportedNode` (name + local TRS + parent uuid), `BoneComponent` tags, the component on the mesh
  node.
- Clip linkage source: `AnimationPlayerComponent.clip` on the spawned rig covers the first clip
  (`assets.cppm:2159-2165`); the rest match by joint names — `AnimTrack.jointName`
  (`geometry.cppm:67-89`) against the rig's bone names (the same rebinding `sampleClipResolved`
  does at runtime, `animation.cpp:302-338`).
- Phase 1's `saveRig`; phase 2's catalog link fields.

## Work

### 1. The extraction (Saffron.Assets)

`auto extractRig(Scene& scene, const SkinnedMeshComponent& skin) -> Result<ImportedRig>`:

- Find the subtree root precisely: the **lowest common ancestor of all `bones[]` entities and the
  mesh entity** ("lowest" binds the walk — every ancestor of an all-covering ancestor also covers,
  so "walk up while still covering" never terminates; the LCA is the faithful inverse because
  `spawnSkinnedModel` instantiated the full node forest under one root). Covers rigs whose mesh
  node and skeleton are siblings, like SimpleSkin.
- Walk the **subtree** from that root, not just the bones: non-joint intermediate nodes affect
  joint world transforms and must be preserved as `ImportedNode`s (name, parent index in walk
  order, TRS from `TransformComponent`).
- `skin.joints` = each `bones[i]` uuid's index in the walk; copy `inverseBind`; `skeletonRoot` =
  `rootBone`'s walk index; `meshNode` = the entity carrying the component (or −1 if outside the
  walk).
- Materials: extract the mesh entity's `MaterialComponent`/`MaterialSetComponent` into the
  `RigMaterial` table the same way the rig is extracted — the `.srig` (phase 1) carries the look,
  and a migrated rig must not preview flat white.

### 2. The `migrate-rigs` command

In `control_commands_asset.cpp`: scan `activeScene` for `SkinnedMeshComponent`s whose `skin.mesh`
catalog entry lacks a `.srig`; for each, `extractRig` → `saveRig` (nodes + skin + materials) →
stamp catalog links:
- set the mesh entry's `rigged = true` (phase 2's persisted key).
- clips: the rig's `AnimationPlayerComponent.clip` first, then every `AssetType::Animation` entry
  whose tracks' `jointName`s all resolve against this rig's bone names (load via `loadAnimation`;
  link only on a full match to avoid cross-rig false positives — a partial match logs a warning).
- Result DTO reports `{ migrated, skipped, failures }` counts. Idempotent: a second run is a no-op.

### 3. Editor surfacing

When `get-rig` errors with "no rig sidecar", the rig-editor open flow (phase 7) shows the error
state with a "Migrate from scene" action calling `migrate-rigs` — but the command itself is this
phase, CLI-reachable (`se migrate-rigs`) before any UI exists.

## Validation (done criteria)

- `make engine` + `make prepare-for-commit` clean; contract fixture for `migrate-rigs` (skip-listed
  or fixtured with an imported rig — it mutates the asset dir).
- `make e2e`: import `leg.gltf`, delete the `.srig` file, run `migrate-rigs` → `get-rig` succeeds
  and matches the original (bone names, parents, joint order, inverse binds within epsilon); run
  again → `{ migrated: 0 }`.
- `docs/`: migration note on the asset-model page (what it recovers, what it cannot).

## Notes / gotchas

- **Migration degrades with user edits**: deleted bone entities are unrecoverable (skip with a
  clear failure message); renamed bones survive for playback (name-rebinding) but change the
  extracted rig's names — document that the extraction reflects the scene as-is.
- The walk order will differ from the original glTF node order; that is fine — `bones[]`/joint
  order is what the skin stream indexes, and it is preserved exactly. Only `joints[]` (node
  indices) are remapped to the new walk.
- Do not auto-migrate on project load — a mutating side effect on open violates least surprise and
  the read-only expectations of `load-project`; the explicit command (+ the editor affordance) is
  the path.
- Match-by-jointName linking can mislink between two rigs with identical bone names; full-match +
  warning is the v1 posture, called out in the docs.
