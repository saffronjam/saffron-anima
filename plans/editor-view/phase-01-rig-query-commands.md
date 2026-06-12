# Phase 1 — rig query commands

**Status:** COMPLETED (kept as a build record; superseded by the general Asset editor — see this dir's README)

**Depends on:** `plans/saffron-models` (build that first, in full) — phases 02 (the `.smodel`
MetadataChunk with `nodes`/`skin`), 06 (the chunk-slice loaders), and 14 (`model-info` /
`asset-references`).

## Goal

Surface the rig over the control plane. After this phase the engine can answer, from assets alone:
"which clips belong to this mesh?", "which rig does this clip target?", and "what is this rig's bone
tree?" — the three queries the editor view is built on. Both commands **read** from the mesh's
`.smodel` container metadata (its `nodes`/`skin` plus its animation sub-assets); nothing new is
persisted. A model's clips and materials are sub-assets of the same container, so the mesh↔clip
association is intrinsic (same file) — there is no catalog-link field to add and no `ProjectVersion`
bump.

## What exists to build on

- The `.smodel` container's MetadataChunk carries the rig (the node forest + the optional `skin`:
  joints, inverse binds, skeleton root, mesh node) and a flat `subAssets` array tagging each
  embedded mesh/material/animation (`plans/saffron-models` phase 02). `readContainerMetadata(path)`
  does a cheap prefix read into `ContainerMetadata` (`geometry.cppm`/`assets.cppm`, saffron-models
  phase 02); `loadModelAsset(assets, modelId)` resolves a model id to that metadata (saffron-models
  phase 06). The bone tree and the clip list are both read directly from that container metadata.
- Saffron-models phase 14 already walks the same data: `model-info {asset}` returns the sub-asset
  list (type, name, bytes) + node/skin presence, and `asset-references {asset}` returns the
  container's internal edges (node → mesh → material → texture). The rig queries here reuse that
  metadata-reading path rather than re-deriving it.
- `list-clips` ignores its `entity` param and returns the whole catalog
  (`control_commands_animation.cpp:171-184`).
- The codegen recipe (`control/AGENTS.md`, `tools/gen-control-dto/AGENTS.md`): DTO structs in
  `control_dto.cppm` (no defaults/methods — the parser is a restrictive regex; field order = CLI
  positional order; `std::optional` ⇒ TS `?` + omitted-when-null), a `CommandDef` + fixture in
  `gen.ts` (`commands` array ~`:97`, `commandFixtures` ~`:749`), regenerate the five outputs,
  `assertRawU64` in `check.ts:132` for new uuid-valued result keys.

## Work

### 1. `get-rig {asset}` command

In `control_commands_asset.cpp` (it is an asset query, not a player command):
`GetRigParams { AssetSelector asset }` → `RigResult { WireUuid mesh; std::string name;
std::vector<RigBoneDto> bones; std::vector<AnimationClipDto> clips; }` with
`RigBoneDto { i32 index; std::string name; i32 parent; bool joint; }` — a flat parent-indexed tree
(the wire shape the skeleton-tree panel renders directly). Accept either a mesh sub-asset or an
animation sub-asset of a model: resolve the asset to its owning `.smodel` container (the sub-asset
shares the container's id), `loadModelAsset` → `ContainerMetadata`, then:
- `bones` from `meta.nodes` + `meta.skin`: each node becomes a `RigBoneDto` (its walk index, name,
  parent index), with `joint = true` for nodes whose index appears in `skin.joints`.
- `clips` from the container's `AssetType::Animation` sub-assets (`meta.subAssets`), shaped as the
  existing `AnimationClipDto`.
- `mesh` = the container's model id; `name` = the model name.

`Err` with a clear message when the asset has no `skin` in its metadata (an unskinned model has no
rig — the editor shows the message). The rig is read straight from the container metadata.

### 2. Make `list-clips` honor its selector

Filter by the container when the param is present: a model (or one of its sub-assets) → the
container's animation sub-assets; keep the no-param behavior (full catalog) for the CLI. The existing
`ListClipsParams.entity` stays for wire-compat; add the optional `asset` selector rather than
repurposing it. The association is intrinsic — the clips are the container's own animation
sub-assets — so the filter is "same container as the selected asset," not a stored link to chase.

### 3. Codegen + gates

DTOs + declarations (`parseDto`/`dtoToJson` declarations are hand-authored in `control_dto.cppm` —
the build fails to link without them), `gen.ts` command entries, regenerate the five outputs,
extend the `assertRawU64` key list with `mesh` if it rides results. **Contract fixtures:
`get-rig`/`list-clips {asset}` cannot be fixtured** — the contract harness seeds only one cube and
`import-model` is skip-listed (`gen.ts:840`, "requires an external model fixture path"), so there
is no imported `.smodel` rig to query; `paramsForFixture` (`check.ts:227`) only builds params, not
prerequisite state. **Skip-list both** in `commandSkips` (like `delete-asset`/`material-*` are) with
the reason "needs an imported rig — covered in make e2e", and exercise them live in `make e2e`
(which imports `leg.gltf`).

## Validation (done criteria)

- `make engine` + `make prepare-for-commit` clean; contract test green with the new commands
  skip-listed (and the help↔manifest completeness check passing).
- `make e2e`: after importing `leg.gltf` — `get-rig` on the mesh sub-asset returns 3 bones with
  correct parent indices and 1 clip read from the container; `get-rig` on the clip sub-asset
  resolves to the same rig (same container); `list-clips {asset}` returns exactly that clip; an
  unskinned model returns the clear "no rig" error.
- `se get-rig <name>` prints something readable (add a `printResult` branch in `tools/se` if the
  default JSON is unwieldy).
- `docs/`: the asset-model explanation gains `get-rig`/`list-clips {asset}` and notes that the rig
  and the clip↔mesh association are read from the `.smodel` container.

## Notes / gotchas

- **The wire DTO and the container metadata are independent serializations** — the rig queries shape
  the wire from `ContainerMetadata`; they do not expose the rig on `AssetEntryDto`, and v1 should not
  (the editor reads the rig through `get-rig`/`list-clips`, keeping the DTO stable). Precedent:
  `hdr`/`linear`/`duration` are persisted but deliberately absent from the DTO.
- **Nothing is persisted by this phase** — both commands are pure reads of the container. There is no
  additive catalog key, so old projects load unchanged and no `ProjectVersion` bump is needed
  (`assets.cppm:681-685`).
- `AssetTypeDto` has no `Rig` variant and this phase does not add one — the rig is the model
  container's embedded `skin`, not a catalog row. Adding an asset type later means five hand-touched
  places (engine enum, names, DTO enum, `enumWireNames`, the dto mapping) — avoid until needed.
