# Phase 2 — Scene commands

**Status:** IN PROGRESS

**Depends on:** phase 1

## Goal

Migrate the scene command group (`control_commands_scene.cpp`) to typed DTOs end to end —
every command in the group, including the irregular ones from the phase-0 catalog. Component
bodies still pass through as opaque json (component DTOs are phase 6); this phase types the
**params and results** of scene commands, not the component blobs they carry.

## The scene command group

The full set registered by `registerSceneCommands` (`command.cppm:67`, second in
`registerBuiltinCommands`): `list-entities`, `list-components`, `create-entity`,
`destroy-entity`, `add-component`, `remove-component`, `set-component`, `set-transform`,
`set-material`, `set-light`, `select`, `pick`, `inspect`, `focus`, `get-environment`,
`set-environment`, `get-selection`, `deselect`, `add-entity`, `copy-entity`, `rename-entity`,
`set-component-field`, `get-camera`, `set-camera`, `get-gizmo`, `set-gizmo`, `gizmo-pointer`,
`dump-schema`.

The current worktree also has reflection-probe scene commands (`set-probes`, `recapture-probes`,
`list-probes`); phase 2 migrates those with the scene group so the live command surface stays typed.

> **Conditional: `set-parent` (scene-hierarchy).** If `plans/scene-hierarchy/` has landed
> its phase 4 first, the scene group also contains a `set-parent {entity, parent?}` command
> (returns `EntityRef`, bumps `sceneVersion`; `plans/scene-hierarchy/phase-4`). Add it to
> this migration with the DTO `EntitySelector entity` + `std::optional<EntitySelector> parent`
> (absent/`"0"` → root) and result `EntityRef`, and bump the group's command count in the
> completeness note below. If this plan lands first, `set-parent` does not yet exist and is
> not migrated here; scene-hierarchy then adds it as a DTO, not a schema. See the README's
> Cross-plan coordination section.

## DTO designs (concrete, per command)

### Regular result-returning

- **`create-entity` / `copy-entity` / `rename-entity` / `add-entity` / `focus` / `select`**
  → result is `EntityRef` (`{WireUuid id, std::string name}`). `create-entity` param: one
  positional `std::string name`. `rename-entity`: `EntitySelector entity` + positional
  `std::string name`. `add-entity`: positional `Preset preset` (enum: `empty|cube|model|
  point-light|spot-light|directional-light|camera`). `copy-entity` / `focus` / `select`:
  `EntitySelector entity`.
- **`destroy-entity`** → `EntitySelector entity`; result `{WireUuid destroyed}`.
- **`list-entities`** → no params; result `EntityList` (`std::vector<EntityRef>`). (Coordinate
  with scene-hierarchy phase 4, which adds `parentId` here — see Cross-plan.)
- **`list-components`** → no params; result `{std::vector<std::string> components}`.
- **`add-component` / `remove-component`** → `EntitySelector entity` + positional
  `std::string component`; result `{bool added}` / `{bool removed}`.

Implementation note: the live wire currently returns the component name string for
`add-component`, `remove-component`, `set-component`, and `set-component-field` (`{"added":"Name"}`,
`{"removed":"Name"}`, `{"set":"Name"}`). The DTOs preserve that shape so this phase does not change
existing clients while moving parsing/result construction to typed code.

## Implementation checkpoint

- Added scene DTO declarations for entity/component, transform/material/light, environment, selection,
  camera, gizmo, pick, and reflection-probe scene commands.
- Extended `tools/gen-control-dto` beyond the phase-1 pilot: nested DTOs, vectors, optionals, enum
  wire strings, opaque json pass-throughs, UUID coercion, and nullable selection output.
- Converted all non-reflective scene handlers to `registerCommand<Params, Result>`; `dump-schema`
  remains raw and is recorded as a manifest skip.
- `bun run tools/gen-control-dto/gen.ts` and `cd editor && bun run check` pass.
- Engine validation is still blocked before control compilation by the existing Clang 21 module crash
  in rendering (`renderer_drawlist.cpp` / `renderer.cppm`). Keep this phase `IN PROGRESS` until a
  validation-clean C++ build or equivalent control compile is available.

### Merge-over-current (README OQ #3 — `std::optional` per overlayable field)

- **`set-transform`** → `EntitySelector entity` + `std::optional<Vec3> translation`,
  `std::optional<Vec3> rotation`, `std::optional<Vec3> scale`; result `EntityRef`. The
  handler keeps its read-modify-write (`control_commands_scene.cpp` `serialize` → patch →
  `deserialize`); the parser leaves `nullopt` for absent keys so omitted fields are not reset.
- **`set-material`** → `EntitySelector entity` + `std::optional<...>` per material field
  (incl. `std::optional<WireUuid> albedoTexture` replacing the string→u64 coercion at the
  boundary, and `std::optional<bool> unlit` replacing the multi-type coercion). Result
  `EntityRef`.
- **`set-light`** → `std::optional<EntitySelector> entity` (positional; absent → first
  `DirectionalLight`) + `std::optional<Vec3> direction/color`, `std::optional<f32>
  intensity/ambient`. Result `EntityRef`. (This command has **no** editor wrapper today —
  phase 4/5 either add one or mark it `se`-CLI-only in the manifest.)
- **`set-environment`** → `std::optional<std::string> json` (positional blob, merged) + named
  overlays as `std::optional<...>`. Result `Environment` (kept opaque in phase 2; phase 6 may
  type it). **`get-environment`** → no params; result `Environment`.
- **`set-camera`** → all `std::optional`: `position(Vec3)`, `yaw/pitch/fov/near/far/
  moveSpeed/lookSpeed (f32)`; result `EditorCamera` (flat). **`get-camera`** → no params.
  **Name the DTO members with the wire keys directly** — `near` / `far`, not `nearPlane` /
  `farPlane`. The engine's `SceneEditCamera` struct uses `nearPlane` / `farPlane` and the
  `get-camera` / `set-camera` handlers already emit/read the wire keys `near` / `far`
  (`control_commands_scene.cpp:618,636-637,641` map `c.nearPlane`→`"near"`,
  `c.farPlane`→`"far"`). The DTO is a fresh struct, not the engine struct, so its members are
  free to be `near` / `far` (both are valid C++ identifiers, not reserved); the handler keeps
  mapping `EditorCamera::near ↔ SceneEditCamera::nearPlane` at the boundary exactly as it does
  today. This keeps phase 2 entirely within the phase-1 grammar (naive member-name emission)
  and needs **no** per-field rename annotation — that generator feature stays a phase-6 concern
  for the *component* `Camera`, whose serde lives in `scene_edit_components.cpp` and cannot move
  its own field names.
- **`set-gizmo`** → `std::optional<GizmoOp> op`, `std::optional<GizmoSpace> space` (enums);
  result `GizmoState {op, space}`. **`get-gizmo`** → no params.

### Per-field set (string→u64 coercion at the boundary)

- **`set-component`** → `EntitySelector entity` + positional `std::string component` + a
  `json` field (the opaque component body, fed straight to `deserialize` —
  `control_commands_scene.cpp`). Result `{bool set}`. The `json` field is the one
  deliberately-opaque escape in the subset until phase 6.
- **`set-component-field`** → `EntitySelector entity` + positional `std::string component`,
  `std::string field`, and a `value` that keeps the string→u64 coercion; result
  `{bool set, std::string field}`.

### Irregular (from the phase-0 catalog)

- **`pick`** → params `std::optional<f32> u`, `std::optional<f32> v` (positional, default
  0.5); result a flat union DTO `{bool hit, std::optional<WireUuid> id, std::optional<std::string>
  name, std::optional<PickKind> kind}` (`PickKind` enum `billboard|mesh`). The EntityRef fields
  are **inlined** (id/name as top-level siblings of `hit`/`kind`), not nested under an `entity`
  key — the handler builds `entityRef(...)` then sets `hit`/`kind` on the same object
  (`control_commands_scene.cpp:364-380`), so the wire is `{hit, kind, id, name}` and the editor's
  `PickResult` is correspondingly flat (`client.ts:38-43`). When `hit` is false the optionals are
  absent — matches the `{hit:false}` shape. (This is the same inlining phase 3 applies to
  `import-model`.)
- **`get-selection`** → no params; result `{i32 selectionVersion, i32 sceneVersion,
  std::optional<EntityRef> entity}` (nullable entity).
- **`deselect`** → no params; result `{i32 selectionVersion}`.
- **`inspect`** → `EntitySelector entity`; result `{WireUuid id, std::string name, <opaque
  components map>}`. The components map stays opaque json in phase 2 (it is registry-keyed,
  not a fixed schema); phase 6 decides whether to type it.
- **`gizmo-pointer`** → positional `Phase phase` (enum `hover|begin|drag|end`), `f32 x`,
  `f32 y` (NDC); result `{std::string hovered, bool dragging}`.
- **`dump-schema`** → **carve-out**: stays a raw handler (it reflects the live registry via
  scratch entities — `control_commands_scene.cpp`). It gets a manifest skip-with-reason
  "reflective; superseded by generated DTOs in phase 6", not a DTO.

## Steps

1. Add the scene param/result DTOs to `control_dto.cppm` in declaration order = positional
   order, using `std::optional` for every merge/absent-sensitive field.
2. Regenerate (`tools/gen-control-dto`); the C++ serde TU + `se-types.ts` update.
3. Convert each scene handler in `control_commands_scene.cpp` to the
   `registerCommand<Params, Result>` overload, replacing `positionalOr` / `asString` /
   manual `is_*()` reads with the typed `Params`. Keep `resolveEntity` as the resolver:
   the handler calls it with the original selector. Keep every read-modify-write
   (`serialize`→patch→`deserialize`) intact.
4. Leave `dump-schema` raw; record its carve-out in the manifest source.

## Validation

- Build `-j1` + `check.sh` green; the regenerate-and-diff gate passes with the committed
  generated TU.
- For every migrated scene command, `se <cmd> ... -o json` is byte-identical to the
  pre-migration build (script the full group as a diff harness). Particularly: `set-transform`
  with only `translation` set leaves rotation/scale untouched (the `std::optional` merge);
  `pick` over empty space returns `{hit:false}` and over a mesh returns the entity+kind union.
- Malformed scene params (e.g. `set-transform` with a non-array `translation`) return
  `ok:false`, not an abort — assert in an e2e case.
- The contract test still validates `inspect` / `get-environment` / `get-selection` /
  `gizmo-state` / `editor-camera` output against the existing schemas (unchanged this phase —
  schemas retire in phase 5).

## Risks

- **Merge semantics regression.** A flat DTO with defaults would silently reset omitted
  fields on every `set-*`. The `std::optional`-per-field design plus the handler's existing
  read-modify-write is the guard; the `set-transform`-partial validation case above pins it.
- **Coercion moves to the boundary.** `set-material`'s string→u64 albedoTexture and
  multi-type `unlit`, and `set-component-field`'s string→u64 `value`, were per-handler
  coercions. Folding them into `WireUuid` / `std::optional<bool>` field types must reproduce
  the exact accepted inputs (numeric string, bare number) or the `se` CLI's string args break.
- **`set-light` has no editor wrapper.** It is engine-only today; do not assume a client
  call site exists. Phase 4/5 classify it (manifest `se`-only) so the completeness gate does
  not demand an editor binding.
- **Scene-hierarchy collision.** That plan's phase 4 edits `entity-ref`/`entity-list` and
  adds `parentId` to `list-entities`. If it lands first, `EntityRef`/`EntityList` DTOs here
  must include `parentId`; if this lands first, scene-hierarchy adds the field to the DTO, not
  a schema. Coordinate before converting `list-entities`.
