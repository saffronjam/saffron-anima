# Phase 08 — Split import/instantiate control commands

**Status:** NOT STARTED
**Depends on:** 07

## Goal

Surface the decoupled flow over the control plane: add `import-model-to-asset` (bake a `.smodel` + catalog
rows, **no spawn**) and `instantiate-model` (spawn the stored hierarchy from an existing model asset).
Define the DTOs in `control_dto.cppm`, update `gen.ts`, regenerate all five generated outputs, make the
verbs reachable from the `se` CLI, and add contract fixtures. Retire or thin `import-model` (the old
spawn-on-import command) to a compose for back-compat. Defers: scan/refresh (09), extract (12).

## Why

Per the repo rule, engine state worth driving gets an `se` command so the editor stays scriptable and the
e2e suite can drive it. The split commands are what the editor (16) and the e2e round-trips (09, 18) call;
they make "import once, instance many" a first-class, testable operation.

## The commands + DTOs

```cpp
// import-model-to-asset: parse → bake → return the model asset ref (NO entity created)
struct ImportModelToAssetParams { std::string path; /* optional ImportOptions overrides */ };
struct AssetRefDto { std::string id; std::string name; std::string type; };   // id as decimal string

// instantiate-model: spawn the stored hierarchy; returns the new root entity
struct InstantiateModelParams { std::string asset; std::string name; /* optional parent entity */ };
struct EntityRefDto { std::string entity; std::string name; };
```

Registered via `registerCommand<Params, ResultDto>` (`command.cppm`) in `control_commands_asset.cpp`:
- `import-model-to-asset` → `importModel`-now-bakes (phase 05) → `AssetRefDto`.
- `instantiate-model` → `instantiateModel` (phase 07) → `EntityRefDto`.
- `import-model` (old, `control_commands_asset.cpp:522`) becomes a thin compose
  (`import-model-to-asset` then `instantiate-model`) for back-compat, or is removed if no caller needs it —
  decide based on existing e2e/editor callers.

## Files to touch

- `engine/source/saffron/control/control_dto.cppm` — add `ImportModelToAssetParams`, `AssetRefDto`,
  `InstantiateModelParams`, `EntityRefDto`.
- `engine/source/saffron/control/control_commands_asset.cpp` — register the two commands; rework
  `import-model`.
- `tools/gen-control-dto/gen.ts` — add the commands to `commands[]` + `commandFixtures`; regenerate
  `control_dto_serde.generated.cpp`, `editor/src/protocol/se-types.ts`, `openrpc.generated.json`,
  `command-manifest.generated.json` (and scene serde if touched). **Never hand-edit generated files.**
- `tools/se/source` — the `se` CLI auto-forwards registered commands; verify the two verbs work.

## Steps

1. Add the DTO structs (uuids/entities as decimal strings, matching the serde convention).
2. Register the two handlers; rework `import-model` to compose or remove it (and update its callers).
3. Edit `gen.ts` (commands + fixtures), `bun run` it, confirm five outputs regenerated and the
   control-schema contract test passes.
4. e2e: `import-model-to-asset` a fixture glTF → assert an `AssetType::Model` row, **no** new entity;
   then `instantiate-model` it twice → assert two entities, no extra catalog rows; log validation-clean.

## Gate / done

- `make engine` clean; `make e2e` + the control-schema contract test pass (new fixtures + the import/
  instantiate round-trip); `se import-model-to-asset` / `se instantiate-model` reachable;
  `make prepare-for-commit` clean.

## Risks

- **Generated-code discipline:** a missed regen or a hand-edit of a `.generated` file fails the contract
  test (it checks raw bytes; ids must be decimal strings, not numbers). Run `gen.ts` and re-check.
- **Back-compat callers:** the editor and existing e2e may call `import-model` expecting a spawn; updating
  them is part of this phase (or the compose preserves behavior). Don't leave a dangling caller.
- **Optional options plumbing:** `ImportOptions` overrides over the wire add surface; keep v1 minimal
  (path only) and add option fields later to avoid churning the DTO + fixtures repeatedly.
