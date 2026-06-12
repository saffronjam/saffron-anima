# Phase 2 — catalog links + rig commands

**Status:** NOT STARTED

## Goal

Close the **linking gap** at the catalog level and surface the rig over the control plane. After
this phase the engine can answer, from assets alone: "which clips belong to this mesh?", "which rig
does this clip target?", and "what is this rig's bone tree?" — the three queries the editor view is
built on. Additive, optional JSON keys only: old projects load unchanged, no `ProjectVersion` bump.

## What exists to build on

- `AssetEntry` (`scene.cppm:340-350`) has no cross-asset references; the catalog round-trips through
  the hand-written `catalogToJson`/`catalogFromJson` (`assets.cppm:286-307` / `:335-363`), which
  default missing keys silently — the additive-compat precedent is `duration`, written only on
  animation rows so other rows stay byte-identical (`assets.cppm:298-303`).
- `ProjectVersion = 1` is hard-gated (`assets.cppm:437`, `:681-685`) — additive keys avoid the bump.
- `importModel` registers the mesh entry (`assets.cppm:1882-1883`) and one entry per clip
  (`:1904-1907`) with **no linkage**; the only association today is the first-clip
  `AnimationPlayerComponent` on the spawned rig (`assets.cppm:2159-2165`).
- `list-clips` ignores its `entity` param and returns the whole catalog
  (`control_commands_animation.cpp:171-184`).
- The codegen recipe (`control/AGENTS.md`, `tools/gen-control-dto/AGENTS.md`): DTO structs in
  `control_dto.cppm` (no defaults/methods — the parser is a restrictive regex; field order = CLI
  positional order; `std::optional` ⇒ TS `?` + omitted-when-null), a `CommandDef` + fixture in
  `gen.ts` (`commands` array ~`:97`, `commandFixtures` ~`:749`), regenerate the five outputs,
  `assertRawU64` in `check.ts:132` for new uuid-valued result keys.
- Phase 1's `loadRigAsset`.

## Work

### 1. Catalog link fields

On `AssetEntry`: `std::vector<Uuid> clips` (meaningful on mesh rows), `Uuid mesh` (meaningful on
animation rows; 0 = unlinked), and `bool rigged` (mesh rows: a `.srig` exists — persisted so the
flag survives project reload; a rig with zero clips has no link row otherwise, and stat-ing
sidecars per `list-assets` is off the table). Persist all as additive optional keys in
`catalogToJson`/`FromJson` — uuids as decimal strings via `uuidToJson` (`json.cppm:72-77`),
matching `id`. Write them only on rows where they are non-empty/true so unrelated rows stay
byte-identical. Populate at import: the clip bake loop (`assets.cppm:1893-1908`) appends each clip
uuid to the mesh entry's `clips`, stamps `entry.mesh = meshId` on the clip entry, and sets
`rigged` after a successful `.srig` write (phase 3's migration sets it too).

### 2. `get-rig {asset}` command

In `control_commands_asset.cpp` (it is an asset query, not a player command):
`GetRigParams { AssetSelector asset }` → `RigResult { WireUuid mesh; std::string name;
std::vector<RigBoneDto> bones; std::vector<AnimationClipDto> clips; }` with
`RigBoneDto { i32 index; std::string name; i32 parent; bool joint; }` — a flat parent-indexed tree
(the wire shape the skeleton-tree panel renders directly). Accept either a mesh asset or an
animation asset (resolve through its `mesh` link). Sourced from `loadRigAsset` + the catalog links;
`Err` with a clear message when no `.srig` exists (the editor shows it and offers migration,
phase 3).

### 3. Make `list-clips` honor its selector

Filter by the catalog links when the param is present: a mesh asset → its `clips`; keep the
no-param behavior (full catalog) for the CLI. The existing `ListClipsParams.entity` stays for
wire-compat; add the optional `asset` selector rather than repurposing it.

### 4. Codegen + gates

DTOs + declarations (`parseDto`/`dtoToJson` declarations are hand-authored in `control_dto.cppm` —
the build fails to link without them), `gen.ts` command entries, regenerate the five outputs,
extend the `assertRawU64` key list with `mesh`/`clips` if they ride results. **Contract fixtures:
`get-rig`/`list-clips {asset}` cannot be fixtured** — the contract harness seeds only one cube and
`import-model` is skip-listed (`gen.ts:840`, "requires an external model fixture path"), so there
is no `.srig` to query; `paramsForFixture` (`check.ts:227`) only builds params, not prerequisite
state. **Skip-list both** in `commandSkips` (like `delete-asset`/`material-*` are) with the reason
"needs an imported rig — covered in make e2e", and exercise them live in `make e2e` (which imports
`leg.gltf`). This matches phase 3's honest posture.

## Validation (done criteria)

- `make engine` + `make prepare-for-commit` clean; contract test green with the new commands
  skip-listed (and the help↔manifest completeness check passing).
- `make e2e`: after importing `leg.gltf` — `get-rig` on the mesh returns 3 bones with correct
  parent indices and 1 linked clip; `get-rig` on the clip asset resolves to the same rig;
  `list-clips {asset}` returns exactly the linked clip; an old-format `project.json` (no link keys)
  still loads.
- `se get-rig <name>` prints something readable (add a `printResult` branch in `tools/se` if the
  default JSON is unwieldy).
- `docs/`: the asset-model explanation gains the link fields + `get-rig`.

## Notes / gotchas

- **The wire DTO and the persisted catalog JSON are independent serializations** — adding the
  persisted keys does not require exposing them on `AssetEntryDto`, and v1 should not (the editor
  reads links through `get-rig`/`list-clips`, keeping the DTO stable). Precedent: `hdr`/`linear`/
  `duration` are persisted but deliberately absent from the DTO.
- An older build opening a new project loads fine but **drops the link keys on its next save**
  (its `catalogToJson` writes only known fields). Acceptable; say so in the docs page.
- `AssetTypeDto` has no `Rig` variant and this phase does not add one — the rig is mesh-keyed
  (derivable sidecar), not a catalog row. Adding an asset type later means five hand-touched
  places (engine enum, names, DTO enum, `enumWireNames`, the dto mapping) — avoid until needed.
