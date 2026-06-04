# Phase 3 — Render + asset/project commands

**Status:** COMPLETED

**Depends on:** phase 1

## Goal

Migrate the render command group (`control_commands_render.cpp`) and the asset/project group
(`control_commands_asset.cpp`) to typed DTOs end to end — every command, including the
platform side-effect viewport commands and the binary-blob thumbnail commands. Parallel to
phase 2 (both depend only on the phase-1 foundation); together they cover all non-component
commands so phase 4 can delete `callRaw`.

## The render command group

Registered first by `registerRenderCommands` (`command.cppm:66`):

- **`ping`** (migrated as a phase-1 pilot) → result `{std::string pong, engine, version;
  i32 pid}`.
- **`help`** → **carve-out**: captures `&reg`, enumerates the registry; stays raw with a
  manifest skip-with-reason "reflective". Its result `{commands:[{name,help}]}` is the
  manifest's own shape — phase 5's completeness gate reads it.
- **`render-stats`** → no params; result `RenderStats` (**22 fields**, matching
  `render-stats.schema.json`: 8 numeric counters (`drawCalls`/`batches`/`instances`/
  `blasCount`/`pipelines` as `i32`, `frameMs`/`fps`/`gpuMs` as `f32`), 12 `bool` feature
  flags (`clustered`, `depthPrepass`, `shadows`, `ibl`, `ssao`, `contactShadows`, `ssgi`,
  `ddgi`, `rtSupported`, `rtShadows`, `restir`, `hdr`), `f32 exposureEv`, and `AaMode aa`
  (string enum); mirror `renderStatsJson`, `command.cppm:62`).
- **`set-aa`** → positional `AaMode` enum (`off|fxaa|taa|msaa2|msaa4|msaa8`); result `{AaMode
  aa}`.
- **Flag toggles** `set-clustered` / `set-ibl` / `set-ssao` / `set-contact-shadows` /
  `set-ssgi` / `set-shadows` / `set-depth-prepass` → param `bool enabled` (positional;
  accepts number|bool|string today — the parser must keep coercing the CLI's string form).
  Each returns a **single bool with a per-command result field name** (the field is not a
  uniform `flag` — verified `control_commands_render.cpp:121,143,165,181,197,274,308`): a
  one-field result DTO per command keyed `{bool clustered}` / `{bool ibl}` / `{bool ssao}` /
  `{bool contactShadows}` / `{bool ssgi}` / `{bool shadows}` / `{bool depthPrepass}`
  respectively. The editor wrappers (`client.ts:259-292`) and the `se` printers read these
  exact keys, so the byte-identical gate requires preserving each name.
- **`set-rt-shadows` / `set-restir`** → same `bool enabled`, guarded by `rtSupported` else
  `Err` (the guard stays in the handler); per-command result DTOs `{bool rtShadows}` /
  `{bool restir}` (`control_commands_render.cpp:217,237`), consumed by `client.ts:296-302`.
- **`set-gi`** → positional `GiMode` enum (`off|ddgi`); result `{bool ddgi}`
  (`control_commands_render.cpp:252`).
- **`set-exposure`** (migrated as a phase-1 pilot) → positional `f32 ev`; result
  `{f32 exposureEv}`.
- **`viewport-native-info`** → no params; result `{std::string platform, transport, status,
  controlSocket, message; i32 width, height}`.
- **`attach-native-viewport`** → **irregular params**: `parentXid` via `readU64` (string or
  number) positional, plus `x/y/width/height` **named-only** (`readI32`, they bypass
  `positionalOr`). DTO: `WireUuid parentXid` + `std::optional<i32> x/y/width/height`; the
  parser must treat the geometry fields as named-only (no positional fallback). Result
  `{bool attached; std::string transport; i32 x,y,width,height}`.
- **`resize-native-viewport`** → named-only `x/y/width/height`; DTO of `std::optional<i32>`;
  result `{bool resized; i32 x,y,width,height}`.

> The `*-native-viewport` commands may be removed/repurposed by
> `plans/viewport-presentation/` phase 3. If that lands first, drop them here; if this lands
> first, that plan updates the DTO. Note the conflict in both plans.

## The asset/project command group

Registered third by `registerAssetCommands` (`command.cppm:68`):

- **`get-project` / `new-project` / `open-project` / `save-project` / `load-project`** →
  result `ProjectInfo` (mirror `projectInfoJson`). `new-project`: positional
  `std::optional<std::string> name/displayName/root` (root optional — the editor builds it
  conditionally). `open-project` / `load-project`: positional `std::string path` (load
  defaults to `project.json`). `save-project`: `std::optional<std::string> path`.
- **`import-model`** → positional `std::string path` (requires project); result `EntityRef`
  plus `{WireUuid mesh, albedoTexture}`. A composite result DTO: `EntityRef` fields inlined +
  the two `WireUuid`s.
- **`import-texture`** → positional `std::string path`; result `{WireUuid texture}`.
- **`list-assets`** → no params; result `AssetList` (`std::vector<AssetEntry{WireUuid id,
  std::string name, type, path}>`).
- **`rename-asset`** → positional `AssetSelector asset`, `std::string name`; result
  `{WireUuid id, std::string name}`. `AssetSelector` mirrors `EntitySelector` for the asset
  catalog's name|numeric-string|number resolution (`control_commands_asset.cpp` rename/assign).
- **`assign-asset`** → positional `EntitySelector entity`, `Slot slot` (enum `mesh|albedo`),
  `AssetSelector asset`; result `{WireUuid id, std::string name; Slot slot}`.
- **`save-scene` / `load-scene`** → positional `std::string path`; result `{std::string
  path}`.
- **`screenshot`** → positional `Target target` (enum `viewport|window`), `std::string path`;
  result `{Target target; std::string path; bool pending}`.
- **`get-thumbnail` / `view-asset`** → positional `AssetSelector asset` + `std::optional<i32>
  size` (default 128 / 512); result `Thumbnail {WireUuid id; std::string format, base64; i32
  width, height}`. The `base64` blob is carried as a plain `std::string` — it is not
  field-validated like a value DTO, just transported.
- **`quit`** → no params; result `{bool quitting}`.

## Implementation checkpoint

- Added render DTOs for stats, AA/GI modes, feature toggles, native viewport bridge commands, and
  exposure.
- Added asset/project DTOs for project info, project/scene path commands, imports, asset catalog
  entries, asset selectors, assignment slots, screenshots, thumbnails, and quit.
- Extended `tools/gen-control-dto` with the phase-3 enum vocabulary, asset selectors, transitive
  TypeScript DTO emission, and named-only parser handling for native viewport geometry.
- Converted all render handlers except reflective `help` and all asset/project handlers to
  `registerCommand<Params, Result>`. The generated manifest now has 65 typed commands and skips only
  `help` and `dump-schema`.
- `bun run tools/gen-control-dto/gen.ts`, `cd editor && bun run check`, and `git diff --check` pass.
- `tools/ci/check.sh` passed end to end in the `saffron-build` toolbox: engine build, headless
  present-only smoke, live control-schema contract test, project startup/asset-layout smoke, and
  frontend build.

## Steps

1. Add the render + asset param/result DTOs to `control_dto.cppm`, declaration order =
   positional order; `std::optional` for the named-only geometry and the optional paths.
2. Regenerate; the serde TU + `se-types.ts` update.
3. Convert each handler to `registerCommand<Params, Result>`, preserving: the `rtSupported`
   guard (`set-rt-shadows`/`set-restir`), the named-only parse for viewport geometry, the
   `requires project` check (`import-model`), and the `AssetSelector` linear-scan resolution.
4. Leave `help` raw; record its carve-out in the manifest source.

## Validation

- Build `-j1` + `check.sh` green; regenerate-and-diff gate passes.
- Every migrated render/asset command's `se <cmd> ... -o json` is byte-identical to the
  pre-migration build. Particularly: the toggle commands still accept `1`/`true`/`on` string
  forms from the CLI; `resize-native-viewport` still reads `x/y/w/h` as named-only and
  ignores positionals; `get-thumbnail` still returns the base64 blob intact.
- The contract test still validates `project-info` / `asset-list` / `thumbnail` /
  `render-stats` output against the existing schemas (unchanged this phase) and the
  `assertRawU64` invariant still holds (ids quoted decimal strings via `WireUuid`).
- Malformed render/asset params return `ok:false`, not an abort — e2e case for one toggle and
  one path command.

## Risks

- **Toggle coercion.** The toggles accept number|bool|string today; the editor wrappers send a
  boolean while the `se` CLI sends strings. The generated `bool` parser must accept all three
  accepted forms (or the DTO field stays a coercing type) so neither caller breaks.
- **Named-only vs. positional.** `attach-/resize-native-viewport` geometry deliberately
  bypasses `positionalOr`. The generated parser must support a per-field "named-only" marker
  so it does not pull `x` from `args[0]`. Decide the marker syntax in the DTO subset (e.g. a
  trailing comment annotation the parser reads) in phase 1's grammar if not already covered.
- **Viewport-presentation collision.** `*-native-viewport` and `viewport-native-info` are
  rewritten by that plan. Coordinate ordering; do not migrate them twice. If that plan is
  active, scope them out of this phase and note it.
- **Binary blob in a value DTO.** `Thumbnail.base64` is large and not field-checkable; keep it
  a plain `std::string` and do not let the generator try to schema-validate its contents.
