# Phase 10 — Material CRUD commands

**Status:** COMPLETED (create + assign; list/get/update deferred to the editor phase)
**Depends on:** 03, 09

> **Outcome.** Added `material-create {name}` → `{ id, name }` (writes a default `.smat` via
> `saveMaterialAsset`) and `material-assign {entity, material}` → `{ material }` (sets/clears
> `MaterialAssetComponent`; `0`/empty clears). A new e2e `tests/e2e/material_asset.test.ts` proves the
> **whole native path**: create → assign → `resolveEntityMaterials` precedence → `resolveMaterialAsset`
> → the render changes from the entity's inline glTF material to the fresh default — confirming phases
> 03/09/10 together. Build clean; **114/114 contract checks**; `.smat` e2e green.
>
> **gen.ts gotchas learned:** (1) gen.ts *parses* `control_dto.cppm` for DTO fields, but the exported
> `parseDto`/`dtoToJson` **declarations** in that file are **hand-maintained** (add them next to the
> sibling command's, or `command.cppm`'s `registerCommand` won't find the overload). (2) gen.ts could
> **not parse `std::optional<EntitySelector>`** — so `material-create`'s "snapshot from current entity"
> (`from`) was dropped; revisit with a string selector or explicit factor fields.
>
> **Full CRUD added (later in-session):** `material-list` → `{ materials: [{id,name,folder}] }`,
> `material-get {id|name}` → blend/unlit/factors/texture-ids, and `material-update {id} [baseColor metallic
> roughness emissive emissiveStrength]` (overwrites the `.smat` in place via `updateMaterialAsset`). All
> e2e-covered (`material_list`/`material_get`/`material_update.test.ts`). So the command surface the editor
> needs is complete except the `material-create` **snapshot-from-entity** (`from`) — still deferred (gen.ts
> can't parse `std::optional<EntitySelector>`; revisit with a string selector or explicit factor fields).
> `material-update` covers the scalar factors; texture-slot assignment is via `assign-asset`.

## Goal

The control-plane surface for materials: `material.create / list / get / update / assign`, with DTOs,
schema generation, `se` CLI verbs, and e2e coverage. After this, materials are fully scriptable and
testable from a shell — the AGENTS.md "feature that adds engine state gets a control command" rule.

## Why

The editor (phase 13) drives everything over the control plane, and e2e tests run on the wire. This
phase makes the native material system (phases 01–09) operable without any UI.

## Commands (match the `registerCommand<Params, ResultDto>` pattern)

- **`material.create {name, folder?, from?}` → `{ id }`** — new `.smat`; `from` (entity) snapshots its
  inline `MaterialComponent` into the asset. Registers an `AssetType::Material` catalog entry.
- **`material.list {}` → `{ materials: [{id, name, folder}] }`** — for the browser/picker.
- **`material.get {id}` → `{ material: SmatDto }`** — full resolved `.smat` (factors, texture handles,
  features, blend, doubleSided).
- **`material.update {id, patch, smooth?}` → `{ id }`** — JSON-merge patch of factors/textures/features.
  Mirror `set-material`'s coalesced-write shape (`SetMaterialParams`) but target the **asset**, not an
  entity. Apply atomically per call (full coalesced apply, clean transaction boundary for a future undo).
  Bump a per-material version so resolve caches (phase 09) invalidate and previews/thumbnails regenerate.
- **`material.assign {entity, material, slot?}` → `{ entity }`** — set `MaterialAssetComponent` (or a
  `MaterialSetComponent` slot handle). May fold into the existing `assign-asset` with a new `AssetSlotDto`
  value (`Material`) — prefer extending `assign-asset` for consistency, add `material.assign` as an alias.

## Files to touch

- `engine/source/saffron/control/control_dto.cppm` — `MaterialCreate/List/Get/Update/AssignParams` +
  `...Result` + a `SmatDto`. Extend `AssetSlotDto` with `Material` for `assign-asset`.
- `engine/source/saffron/control/control_commands_asset.cpp` (and/or `control_commands_scene.cpp`) —
  register the handlers; reuse `resolveMaterialAsset`/`saveMaterialAsset`.
- `tools/gen-control-dto/gen.ts` — add the commands to `commands[]`; add `commandFixtures` entries for the
  deterministic ones (`material.create/list/get/update`), and `commandSkips` for `material.import`
  (external files, like `import-model`/`import-texture`). **Regenerate** → `control_dto_serde.generated.cpp`,
  `editor/src/protocol/se-types.ts`, `openrpc.generated.json`, `command-manifest.generated.json`.
- `tools/se/source/main.cpp` — optional `printResult` branches for human-readable `material list`/`get`
  (auto-forwards regardless; only formatting is bespoke).
- `tests/e2e/` — a material suite (create → update → get round-trip → assign → list).

## Steps

1. Define the DTOs + `SmatDto`; extend `AssetSlotDto`.
2. Register the handlers (create/list/get/update/assign) over the phase-03/09 engine fns.
3. Add to `gen.ts` (`commands`, `commandFixtures`/`commandSkips`); regenerate; verify the contract test
   (`tools/check-control-schema`) passes.
4. Add `se` formatting + an e2e suite; assert a validation-clean engine log.

## Gate / done

- `make engine` + `make schema` (control-schema contract) clean; `make e2e` material suite green.
- `se material create/list/get/update/assign` all work against a running engine.
- `make prepare-for-commit` clean. Docs: the material control commands (reference page row).

## Risks

- **Fixture/skip omissions** break `check-control-schema` (the gate). Every new command needs a fixture or
  a skip reason — `material.import` must be a skip (external files).
- **Patch semantics**: define `material.update` merge precisely (absent field = unchanged; explicit null =
  clear). Match the optional-field style of `SetMaterialParams`.
- Keep `material.assign` and `assign-asset(Material)` from diverging — one implementation, one alias.
