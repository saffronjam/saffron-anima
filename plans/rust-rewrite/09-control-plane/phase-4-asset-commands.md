# Phase 4 — Asset command domain

**Status:** COMPLETED

**Depends on:** 09-control-plane:phase-1-socket-server-and-dispatch, 07-assets-and-materials (the asset server, catalog, importers, material system, thumbnails), 03-ecs-and-scene + 08-host-and-viewport (sceneEdit for project/scene state and instantiation), 06-rendering (preview render, screenshot, active-view)

## Goal

Register the 52 asset-domain commands (`register_asset_commands`): project lifecycle
(get/new/open/save/load/reload), model + texture import + instantiation, the catalog
(list/scan/clean/delete-unused), sub-asset extraction, model info + references + asset model, the
asset-preview mode + active-view switch, asset/folder management (rename/move/create-folder/delete),
asset usages + metadata + assignment, the full material system (create/assign/import/list/get/update/
preview/set-graph/create-instance/set-override/compile/cook), scene save+load, screenshot, and
thumbnails (get/view/cache). This is the highest-coupling domain — `assets` (88), `sceneEdit` (136),
`renderer` (29), and `window` (1).

## Why this shape (NO LEGACY)

- **Four-subsystem reach is real, not incidental.** The asset domain genuinely orchestrates across
  `assets` (the catalog/importers/material store), `sceneEdit` (project + active-scene + instantiation
  + usage scans), `renderer` (preview/thumbnail/screenshot/material-compile), and `window`
  (screenshot target). The `EngineContext` disjoint-field borrow lets a handler hold `&mut` to
  `assets` and `sceneEdit` and `renderer` at once (e.g. `assign-asset`, `enter-asset-preview`,
  `material-update`). This is why control ports last — every one of these subsystems must already be
  Rust. The handlers stay thin orchestration; the heavy lifting (`renderScene`, the importers, the
  node-graph→slang codegen) lives in `07-assets-and-materials`.
- **`AssetSelector` is the id-or-path selector**, the asset analogue of `EntitySelector` — resolved by
  the asset-side selector helper, kept as a single translation place, not duplicated.
- **Project/scene save+load return the canonical info DTOs.** `*-project` commands return
  `ProjectInfoDto`; `save-scene`/`load-scene` return `PathResult`. The serde byte-compatibility of the
  *project/scene file format* is `03-ecs-and-scene`'s concern; this phase is the command wrapper that
  drives it and reports the result. NO second save path — one project format, the one the scene crate
  writes.
- **The material commands form one coherent group** (12 commands) but stay in this phase because they
  are registered in the asset file and share the asset server + renderer reach. The node-graph blobs
  (`MaterialSetGraphParams.graph`, `MaterialGetResult` graph field) are opaque `Json`/`Value`
  passthroughs — the graph schema is the editor's React-Flow model, not a typed protocol DTO.
- **`set-active-view`/`enter-asset-preview`/`exit-asset-preview` use the `ViewId` mapping**
  (`"scene"`/`"assetPreview"` ↔ `ViewId`), the single `view_id_from_wire`/`view_id_wire` translation
  (`command.cppm:85`) — kept as the one place, ported as `enum ViewId` + `FromStr`/`Display`.
- **`screenshot` reaches `window`** (the only command that does) for the `ScreenshotTargetDto`
  (`Viewport` vs `Window`) — kept; the window dimensions/handle feed the capture path.
- **Destructive/import commands carry no fixture** (they need external files or mutate the project fs)
  — they are exercised by targeted e2e cases with a scratch project, not the manifest contract test.
  The catalog marks each `*(needs ...)*`/`*(destructive)*`; this is faithful to the C++ `commandSkips`
  list in `gen.ts` (e.g. `import-model`/`import-texture` skip with "requires an external fixture").

## Grounding (real files/symbols)

- `engine-old/source/saffron/control/control_commands_asset.cpp`
  - `registerAssetCommands` (the 52-command block); the highest `ctx.assets`/`ctx.sceneEdit` density.
  - Sub-asset uuids read via `readWireUuid(*value, "subAsset")` (the generated serde, `:2982`/`:3014`)
    — the wire-uuid contract inside extraction params.
  - `viewIdFromWire`/`viewIdWire` use for `set-active-view`/asset-preview (`command.cppm:65`/`:78`).
- DTOs: `ProjectInfoDto`, `NewProjectParams`, `PathParams`, `OptionalPathParams`, `ImportModelResult`,
  `InstantiateModelParams`, `ImportTextureResult`, `ExtractSubAssetParams`/`ClearExtractionParams`,
  `AssetEntryDto`/`AssetList`, `ScanAssetsResult`, `ReimportModelParams`/`ReimportModelResult`,
  `ModelInfoParams`/`ModelSubAssetDto`/`ModelInfoResult`, `AssetReferencesParams`/
  `AssetReferencesResult`, `GetAssetModelParams`/`AssetModelResult`/`AssetCapabilitiesDto`/`BoneDto`/
  `AnimationClipDto`, `EnterAssetPreviewParams`/`AssetPreviewResult`/`BoneEntityDto`,
  `SetAssetPreviewOptionsParams`/`AssetPreviewOptionsResult`, `SetActiveViewParams`/
  `SetActiveViewResult`, `CleanAssetsParams`/`CleanCandidateDto`/`CleanReport`, `DeleteUnusedParams`/
  `DeleteUnusedResult`, `RenameAssetParams`/`AssetRef`, `CreateAssetFolderParams`/
  `RenameAssetFolderParams`/`DeleteAssetFolderParams`/`MoveAssetParams`, `AssetUsagesParams`/
  `AssetUsageDto`/`AssetUsagesResult`, `AssetMetadataParams`/`AssetMetadataDto`, `DeleteAssetParams`/
  `DeleteAssetResult`, `AssignAssetParams`/`AssignAssetResult`, the `Material*` family
  (`MaterialCreateParams`/`MaterialCreateResult`, `MaterialAssignParams`/`MaterialAssignResult`,
  `MaterialImportParams`/`MaterialImportResultDto`, `MaterialRefDto`/`MaterialListResult`,
  `MaterialGetParams`/`MaterialGetResult`, `MaterialUpdateParams`/`MaterialUpdateResult`,
  `PreviewRenderParams`/`PreviewRenderResult`, `MaterialSetGraphParams`/`MaterialSetGraphResult`,
  `MaterialCreateInstanceParams`, `MaterialSetOverrideParams`/`MaterialSetOverrideResult`,
  `MaterialCompileParams`/`MaterialCompileResult`, `MaterialCookResult`), `PathResult`,
  `ScreenshotParams`/`ScreenshotResult`, `ThumbnailParams`/`ThumbnailResult`, `ThumbnailCacheParams`/
  `ThumbnailCacheResult` — all in `control_dto.cppm`.
- Enums: `AssetSlotDto`, `AssetTypeDto`, `ScreenshotTargetDto` (`control_dto.cppm:124`+).
- `09-control-plane/catalog.md` — the asset-domain table (52 rows) + fixtures/skips.

## Acceptance gate

- `cargo build -p saffron-control` green with the asset handlers registered; clippy/fmt clean.
- `cargo test -p saffron-control` passes asset-domain unit tests over a stub asset server + scratch
  project:
  - `list-assets`/`scan-assets` round-trip on an empty project (the `empty` fixture shape).
  - `assign-asset` resolves an `AssetSelector` (id and path), assigns to an entity, and the result
    carries decimal-string ids.
  - `set-active-view` maps `"scene"`/`"assetPreview"` ↔ `ViewId` and errors on an unknown view
    (mirrors `viewIdFromWire`'s `Err`).
  - `rename-asset` round-trips (`mesh-asset-rename` fixture).
  - material `set-graph`/`get` keep the graph as an opaque `Value` (no typed-DTO drift).
- The wire-contract test validates every *fixtured* asset command's live `result` against OpenRPC and
  `help` against the manifest (`empty`, `new-project`, `project-name`, `active-view-scene`,
  `mesh-asset`, `mesh-asset-rename`, `mesh-asset-view`, `cube-mesh-asset`, `thumbnail-cache-stats`).
- Each external-input/destructive command (`import-*`, `material-*`, folder ops, `delete-*`,
  `save/load-scene`, `screenshot`) carries its manifest **skip reason** matching the C++ `gen.ts`
  skip list — verified by the manifest-parity assertion, not exercised live.
- All asset/entity/material ids stay decimal strings.

## Implementation notes (as built)

Lives in `crates/control/src/commands_asset.rs` (`register_asset_commands`), registered last in
`register_builtin_commands` (the asset domain is the frozen manifest tail, `get-project` … `quit`).
`set-asset-preview-options` is in the **animation** block per the manifest order, so it is registered
in `commands_animation.rs` between `pick-skeleton-joint` and `get-foot-ik` (it reuses
`commands_asset::compute_preview_bounds` / `spawn_preview_floor`). An order-lock unit test
(`asset_commands_register_in_manifest_order`) asserts the contiguous frozen order.

**Substrate added to the single `ControlRenderer` seam** (`crates/control/src/registry.rs`) — the
asset domain needs more renderer reach than render/scene/animation/physics, so the one trait grew the
following methods (implemented in the host `HostControlRenderer` and the test stub):

- `with_thumbnail_gpu(&mut dyn FnMut(&dyn ThumbnailGpu))` — `get-thumbnail` / `view-asset` drive
  `request_thumbnail`, and `preview-render` drives `render_material_preview` +
  `encode_texture_thumbnail_png`, through it.
- `render_settings_to_json` / `apply_render_settings` — wrapped by the in-crate `RendererProjectHost`
  adapter so the project-lifecycle commands (`new`/`open`/`save`/`load`/`reload`) can call
  `AssetServer::{create,load,save}_project` (which take a `&mut dyn ProjectHost`).
- `sa_lua_defs` — the LuaLS defs text written to `library/sa.lua` on project create/load.
- `request_window_capture` — the `screenshot {target:window}` capture path (arms the swapchain capture
  for the next present).

The `instantiate_model` Rust API references mesh ids by uuid (GPU upload is lazy at render), so
`instantiate-model` / `enter-asset-preview` need no upload seam — simpler than the C++ shape.

**Downstream render seams now LIVE** (06-rendering's offscreen-render-to-PNG + window-capture +
render-settings primitives landed — see `06-rendering/phase-16-capture-shm-profiler.md` "Deferred seams
closed"):

- The GPU thumbnail / material-preview render-to-PNG primitives — `HostThumbnailGpu`'s `render_*` /
  `encode_*` route to the renderer's offscreen-render-and-read-back, validated non-trivial on lavapipe.
  `get-thumbnail` / `view-asset` / `preview-render` are functional.
- The renderer `renderSettings` serde — `render_settings_to_json` / `apply_render_settings` are live and
  round-trip through project save/load.
- The window-surface capture — `request_window_capture` arms the next present's swapchain copy;
  `screenshot {target:window}` is functional (validated live under the toolbox weston).

**Still unbuilt downstream** (the host impl returns an honest default until it lands; tracked so the
plan stays honest):

- The host LuaLS defs generator — `sa_lua_defs` returns an empty string; the project still loads/saves.
