# Phase 07 — Decoupled instantiate from container

**Status:** NOT STARTED
**Depends on:** 06

## Goal

Add `instantiateModel(scene, modelId, name)`: expand the `MetadataChunk` node hierarchy into entt
entities (reusing `spawnModel` / `spawnSkinnedModel` / `relinkHierarchy`), attach materials by
`MaterialAssetComponent` referencing the embedded `.smat` chunk sub-ids, and set up bones +
`AnimationPlayerComponent` for skinned models. **Import no longer spawns** — one `.smodel` asset
instantiates into many entities on demand (or never). Defers: the editor drag UX (16), broken-reference
reporting (14/15).

## Why

This is the Unity Model-Prefab / Godot `PackedScene.instantiate()` behavior and the fix for the user's
original pain ("hard to create that entity on my own"): import produces a durable asset; you create the
entity the same way every time, from the asset. The hierarchy needed to spawn now lives in the container
(phase 02's `nodes`/`skin`), so spawning is "expand the saved tree" — and it supersedes the
`editor-view` `.srig` sidecar entirely.

## `instantiateModel`

```cpp
// Expand the container's stored hierarchy into the scene. Returns the root entity.
// Holds SOFT references: components store (modelId, subId), resolved at render time via phase 06.
std::expected<Entity, std::string>
instantiateModel(Scene& scene, AssetServer& assets, Renderer& renderer, Uuid modelId, std::string_view name);
```

Implementation, reusing today's spawn machinery:
1. `loadModelAsset(assets, modelId)` (06) → `meta` (`nodes`, `skin`, `materials`, `subAssets`).
2. Parse `meta.nodes` into the same shape `spawnModel`/`spawnSkinnedModel` already consume (name, parent
   index, TRS). For each mesh node, attach a `MeshComponent` whose mesh ref is the **mesh sub-id** (not a
   standalone file uuid) and a `MaterialAssetComponent` per slot pointing at the **material chunk sub-id**.
3. If `meta.skin` is present: build the bone entities, tag joints (`BoneComponent`), attach
   `SkinnedMeshComponent`, and an `AnimationPlayerComponent` seeded with the container's animation sub-ids.
4. `relinkHierarchy` resolves uuid-keyed parents → entity handles, exactly as today.

The spawned entity stores `(modelId, subId)` pairs (soft refs). Nothing about the container is copied into
the scene; the scene references it by id, so reimport/extract changes flow through (phases 12/13).

## Component touch points

- `MeshComponent` / `SkinnedMeshComponent` / `BoneComponent` keep their shape; the mesh ref becomes a
  sub-id resolved through `resolveMesh` (06).
- `MaterialAssetComponent` (from material-uplift phase 09) references a material **sub-id**; resolution
  precedence (asset > set > inline > default) is unchanged, but "asset" now resolves through the container.
- A new tiny `ModelInstanceComponent { Uuid modelId }` on the root marks the entity as an instance of a
  model asset (so the editor can show it and reimport can find live instances in phase 13).

## Files to touch

- `engine/source/saffron/assets/assets.cppm` — `instantiateModel`; adapt `spawnModel`/`spawnSkinnedModel`
  to take node data from `ContainerMetadata` (parse `meta.nodes`/`meta.skin`) instead of the transient
  `ImportResult`; remove the spawn from `importModel`.
- `engine/source/saffron/scene/scene.cppm` — add `ModelInstanceComponent`; regen scene serde via `gen.ts`.

## Steps

1. Write a `meta.nodes`/`meta.skin` → spawn-input adapter (the inverse of phase 05's hierarchy encode).
2. Refactor `spawnModel`/`spawnSkinnedModel` to consume that adapter's output; route mesh/material refs to
   sub-ids resolved via phase 06.
3. Add `ModelInstanceComponent`; regen serde; confirm the contract test stays green.
4. Implement `instantiateModel`; confirm `importModel` no longer spawns.
5. Self-test/e2e: bake a skinned + an unskinned container, `instantiateModel` each twice, assert two
   independent entity trees per model with correct hierarchy depth, material assignment, and (skinned) a
   working `AnimationPlayerComponent`.

## Gate / done

- `make engine` clean; instantiate self-test/e2e passes (one asset → two instances, hierarchy + materials
  + skinning intact); serde regenerated and contract test green; `make prepare-for-commit` clean.

## Risks

- **Hierarchy fidelity:** the `nodes`/`skin` JSON (phase 02) must capture everything `spawnSkinnedModel`
  needs (joint order, inverse-bind matrices, skeleton root, mesh node). A missing field silently produces
  a broken skeleton; the skinned self-test must assert joint count + a sample bind pose.
- **Soft-reference dangling:** an instance referencing a since-deleted container must degrade to a
  diagnosable missing-reference, not a silent null (the current negative-cache is silent). Broken-reference
  reporting lands in 14/15; until then, log on resolve miss.
- **Spawn refactor blast radius:** `spawnModel` is on the renderer's data path; changing its input source
  risks regressions in existing scenes. Keep the adapter output shape identical to today's `ImportResult`-
  derived input to minimize churn.
- **Quaternion order:** the `(w,x,y,z)` convention from phase 02 must match what `spawnModel` expects, or
  every instance is mis-rotated.
