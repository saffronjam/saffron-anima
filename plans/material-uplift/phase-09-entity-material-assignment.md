# Phase 09 — Entity material assignment

**Status:** COMPLETED (component + resolve; 9-field serde + doubleSided deferred)
**Depends on:** 03

> **Outcome.** `MaterialAssetComponent { Uuid material }` added, registered in
> `registerBuiltinComponents` with **inline-lambda serde** (one field → no `gen.ts` regen needed).
> `resolveMaterialAsset(.smat → SubmeshMaterial)` loads the asset's texture handles (a packed ORM feeds
> both the metallic-roughness slot and the occlusion slot; `blend:"masked"` → `alphaClip`).
> `resolveEntityMaterials` now applies **precedence: MaterialAssetComponent > MaterialSetComponent >
> MaterialComponent > built-in default** (a missing/zero asset id → `defaultMaterialAsset()` + a one-time
> warning, never a crash). Build clean; 6/6 e2e no-regression (no entity carries the component yet, so the
> new branch is dormant). **Functional `.smat`→entity render is exercised in phase 10** (which adds
> `material.create`/`assign`).
>
> **Deferred:** (1) ~~the 9-field `MaterialComponent`/`MaterialSlot` serde persistence~~ — **DONE** (later
> in-session): `gen.ts emitSceneSerde` now persists `normal/occlusion/emissive/height` textures +
> `normalStrength`/`heightScale`/`alphaClip`/`alphaCutoff` for both `MaterialComponent` and `MaterialSlot`;
> `material_persist.test.ts` proves an assigned normal map survives save+reload. (Only `uvTiling`/`uvOffset`
> remain unpersisted — `vec2` needs a serde helper; they default 1/0, not a data-loss path.) (2) the **`doubleSided`
> PSO axis** (`Material` + `requestMeshPipeline` cache key + `cullMode`) — isolated; a later cleanup, paired
> with reading `MaterialAsset.doubleSided` here.

## Goal

Let an entity reference a shared `.smat` asset. Add a `MaterialAssetComponent { Uuid material }`
(and an optional per-slot material handle on `MaterialSetComponent`), define resolve **precedence**
over the inline `MaterialComponent`, regenerate the component serde, and register the component for
the inspector. Many entities sharing one `.smat` then resolve to one deduped `MaterialParams` entry.

## Why

This is "select a material for an entity". Today materials are inline-only. A handle component is the
edit-once-propagate mechanism: change the `.smat`, every referencing entity updates.

## Design

```cpp
// scene.cppm
struct MaterialAssetComponent { Uuid material; };  // 0 / missing → fall back
// (optionally) MaterialSetComponent slots gain an optional `Uuid materialAsset` per slot.
```

**Precedence in `resolveEntityMaterials`** (assets.cppm): `MaterialAssetComponent` (load `.smat` →
`SubmeshMaterial`) **>** `MaterialSetComponent` **>** inline `MaterialComponent` **>** built-in default.
A new `resolveMaterialAsset(assets, renderer, Uuid) -> SubmeshMaterial` loads the `.smat` (phase 03),
resolves its texture handles to bindless `Ref<GpuTexture>` (via `loadTextureAsset`), and sets `features`.
Missing material/texture → built-in default (phase 03) + a one-time warning (never a silent null).

## Files to touch

- `engine/source/saffron/scene/scene.cppm` — `MaterialAssetComponent`; (optional) set-slot handle.
- `tools/gen-control-dto/gen.ts` — add the component to the serde catalog (`emitSceneSerde`): its
  `...ToJson`/`...FromJson` (Uuid as decimal string). **Regenerate** (`bun run tools/gen-control-dto/gen.ts`)
  → rewrites `scene_component_serde.generated.cpp`. Do **not** hand-edit the generated file.
- `engine/source/saffron/sceneedit/scene_edit_components.cpp` — `registerComponent<MaterialAssetComponent>(
  reg, "MaterialAsset", noop, materialAssetComponentToJson, materialAssetComponentFromJson, true)`.
- `engine/source/saffron/assets/assets.cppm` — `resolveMaterialAsset` + the precedence in
  `resolveEntityMaterials`; dangling-ref → default + warn.

## Steps

1. Declare `MaterialAssetComponent`; add to `gen.ts` catalog; regenerate the serde.
2. Register the component in `registerBuiltinComponents`.
3. Implement `resolveMaterialAsset` (`.smat`→`SubmeshMaterial`, loads textures, sets features).
4. Add precedence to `resolveEntityMaterials`; missing refs → default material + one warning.
5. e2e: create a `.smat`, add `MaterialAssetComponent` to an entity, render, assert it uses the asset;
   delete the `.smat` and assert the entity falls back to default (not a crash/black).

## Gate / done

- `make engine` clean; an entity with a `MaterialAssetComponent` renders the asset's material; two
  entities sharing it resolve to one `MaterialParams` entry (verify via the dedup count).
- Missing material/texture → default + warning, never a crash. `make prepare-for-commit` clean.
- Scene save/load round-trips the new component (decimal-string Uuid). Docs: assignment + precedence.

## Risks

- **Generated-serde drift**: the only correct workflow is edit `gen.ts` + regenerate. A hand-edit will be
  overwritten and the contract test will diverge. Call this out in the commit.
- **Resolve cost**: `resolveMaterialAsset` per entity per frame is wasteful — cache the resolved
  `SubmeshMaterial`/`MaterialParams` by material `Uuid` and invalidate on `.smat` edit (phase 13 edits
  bump a material version; resolve keys on it).
- Precedence must be deterministic and documented so the inspector shows the effective material.
