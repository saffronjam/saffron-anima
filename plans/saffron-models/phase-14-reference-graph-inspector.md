# Phase 14 — Reference graph + dependency inspector

**Status:** COMPLETED
**Depends on:** 13

> Implementation note: `buildDependencyGraph` builds nodes from the catalog (bytes = `.smodel`/file
> size, or the embedded chunk length via `assetBytes`) + edges: ContainerChild (model→sub-asset),
> MaterialTexture (`.smat` → textures, via resolve/loadMaterialAsset), EntityAsset (scene
> mesh/skinned/materialAsset/modelInstance/materialSet → asset, `from` = entity uuid). `referencedBy`/
> `referencesOf`/`footprint` query it. `model-info` summarizes a container (sub-assets+bytes, source
> recipe, total footprint); `asset-references` returns the who/what + footprint. Footprint is the honest
> on-disk byte count (a `.smodel` already includes its embedded sub-assets — no double-count);
> exclusive-vs-shared accounting is a v1 simplification. Verified by `tests/e2e/model_inspect.test.ts` +
> the 133-check contract test.

## Goal

Build the dependency graph — scene entity → `(modelId, subId)` → material → texture, plus the container's
internal edges (node → mesh → material → texture) — and expose inspector commands: `model-info {asset}`
and `asset-references {asset}` returning **what-references-this** / **what-this-references** plus a
byte-footprint rollup per sub-asset. DTOs + `gen.ts` + e2e. Defers: the destructive cleanup that consumes
this graph (15), the editor UI for it (16).

## Why

This is UE's **Reference Viewer + Size Map** as a first-class debugging aid (not only a precursor to
delete). Cleanup (phase 15) needs reachability from roots, and the editor (16) needs "what uses this model"
and "what would deleting it break." It also makes the silent-null failure mode of soft references
(phases 06/07) diagnosable: a broken `(modelId, subId)` edge becomes visible.

## The graph + queries

```cpp
struct RefNode { Uuid id; AssetType type; Uuid container; u64 bytes; };   // bytes = chunk/file size
struct RefEdge { Uuid from; Uuid to; enum class Kind { NodeMesh, MeshMaterial, MaterialTexture,
                                                       EntityAsset, ContainerChild } kind; };

struct DependencyGraph {
    std::vector<RefNode> nodes; std::vector<RefEdge> edges;
    std::vector<Uuid> referencedBy(Uuid) const;   // who points AT this
    std::vector<Uuid> referencesOf(Uuid) const;    // what this points to
    u64 footprint(Uuid) const;                     // self + uniquely-owned descendants (Size Map)
};
DependencyGraph buildDependencyGraph(const Scene&, const AssetCatalog&, AssetServer&);
```

Edge sources:
- **EntityAsset:** scan the active scene's `MeshComponent` / `MaterialAssetComponent` / `ModelInstanceComponent`
  (phase 07) for `(modelId, subId)` refs.
- **ContainerChild / NodeMesh / MeshMaterial / MaterialTexture:** from each container's MetadataChunk
  (`nodes` → mesh index, `materials` → texture sub-ids) and each `.smat`'s texture refs.

## Commands

- `model-info {asset}` → the container's metadata summary: sub-asset list (type, name, bytes), material
  count, node/skin presence, source path + hash, total footprint.
- `asset-references {asset}` → `{ referencedBy: [...], references: [...], footprint }` — the Reference
  Viewer payload, for any asset id (model or sub-asset).

## Files to touch

- `engine/source/saffron/assets/assets.cppm` — `DependencyGraph` + `buildDependencyGraph` + the queries +
  `footprint`.
- `engine/source/saffron/control/control_dto.cppm` + `control_commands_asset.cpp` + `gen.ts` —
  `model-info` + `asset-references` commands + DTOs + regen.

## Steps

1. Implement `buildDependencyGraph` from the scene + catalog + container metadata.
2. Implement `referencedBy` / `referencesOf` / `footprint` (footprint = self + descendants reachable only
   through this node, to avoid double-counting a shared texture).
3. Add the two commands + DTOs; `gen.ts`; fixtures.
4. e2e: import a multi-material model, instantiate it, `asset-references` the model (asserts the scene
   entity references it) and a shared texture (asserts multiple materials reference it); `model-info`
   returns the sub-asset list + a sane footprint. Include a broken edge (delete an extracted file) and
   assert it's reported, not crashed.

## Gate / done

- `make engine` clean; the inspector e2e proves correct referenced-by/references + footprint + broken-edge
  reporting; `make e2e` + contract test pass; `make prepare-for-commit` clean.

## Risks

- **Static reachability misses dynamic refs:** a Lua `ScriptComponent` can reference an asset by name/path
  the static scan can't see. Surface those as an **indirect/review** category (phase 15 consumes it); never
  treat "no static referrer" as "definitely unused."
- **Footprint double-counting:** a texture shared by two materials belongs to neither's exclusive
  footprint; define footprint as exclusively-owned bytes + report shared bytes separately, or the rollup
  misleads.
- **Graph staleness:** the graph is a snapshot; rebuild it on scan/scene change rather than caching stale
  edges.
- **Scope:** keep this read-only/diagnostic; deletion belongs to phase 15. Mixing them risks an accidental
  destructive path here.
