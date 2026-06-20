# Control command + DTO catalog

The complete, frozen wire surface: **153 typed commands** plus the one untyped reflective builtin
(`help`), grouped by the domain that registers them. This catalog is the single authoritative list
that the protocol codegen (`10-protocol-codegen`/PP-7), the `sa` CLI (`11-sa-cli`/PP-9), and the e2e
fixtures (`13-testing-and-verification`/PP-13) all consume. It is transcribed verbatim from
`schemas/control/command-manifest.generated.json` (153 typed + 1 skip) and the `commands: CommandDef[]`
array in `tools/gen-control-dto/gen.ts` (the generator's own source list), cross-checked against the
five `registerCommand` call sites in `engine-old/source/saffron/control/control_commands_*.cpp`
(13 animation + 52 asset + 12 physics + 29 render + 47 scene = 153, plus `help` registered untyped in
`control_commands_render.cpp`).

The columns are: **command name** (the wire `cmd` string — kebab-case, dotted for the `profiler.*`
group), **params DTO**, **result DTO**, **contract fixture** (the manifest's e2e fixture name, or a
blank cell = no fixture / requires external input), and **EngineContext reach** (which live-state
references the handler touches, from the `ctx.*` grep). The DTO field shapes are catalogued separately
in the DTO inventory at the bottom; field declaration order in `control_dto.cppm` IS the positional
CLI argument order (`AGENTS.md` rule), and codegen must preserve it (`preserve_order`).

`EngineContext` is `{ window, renderer, sceneEdit, assets, physics? }` — references built fresh each
frame, never stored past it; `physics` is the live play world or null in Edit
(`command.cppm:31` `struct EngineContext`).

---

## Render domain — `register_render_commands` (29 typed + `help`)

Reach: almost entirely `ctx.renderer` (53 hits); the only outliers touch nothing else. Imports:
`Saffron.Core`, `Saffron.Rendering`.

| Command | Params | Result | Fixture | Reach |
|---|---|---|---|---|
| `ping` | `PingParams` | `PingResult` | `empty` | (none — pid/version only) |
| `render-stats` | `EmptyParams` | `RenderStatsDto` | `empty` | renderer |
| `profiler.set-mode` | `ProfilerSetModeParams` | `ProfilerModeResult` | `profiler-timestamps` | renderer |
| `pass-timings` | `EmptyParams` | `RenderPassTimingsDto` | `empty` | renderer |
| `profiler.capture-start` | `CaptureStartParams` | `CaptureStartResult` | `capture-single` | renderer |
| `profiler.capture-stop` | `EmptyParams` | `CaptureStopResult` | `empty` | renderer |
| `profiler.capture-status` | `EmptyParams` | `CaptureStatusResult` | `empty` | renderer |
| `frame-history` | `FrameHistoryParams` | `FrameHistoryDto` | `frame-history-samples` | renderer |
| `get-perf-config` | `EmptyParams` | `PerfConfigDto` | `empty` | renderer |
| `set-perf-config` | `SetPerfConfigParams` | `PerfConfigDto` | `perf-config-30` | renderer |
| `drain-alarms` | `DrainAlarmsParams` | `DrainAlarmsResult` | `alarms-since-0` | renderer |
| `list-active-alarms` | `EmptyParams` | `ActiveAlarmsDto` | `empty` | renderer |
| `set-aa` | `SetAaParams` | `SetAaResult` | `aa` | renderer |
| `set-view-mode` | `SetViewModeParams` | `SetViewModeResult` | `view-mode-wireframe` | renderer |
| `set-clustered` | `ToggleParams` | `SetClusteredResult` | `toggle-on` | renderer |
| `set-ibl` | `ToggleParams` | `SetIblResult` | `toggle-on` | renderer |
| `set-ssao` | `ToggleParams` | `SetSsaoResult` | `toggle-on` | renderer |
| `set-contact-shadows` | `ToggleParams` | `SetContactShadowsResult` | `toggle-on` | renderer |
| `set-ssgi` | `ToggleParams` | `SetSsgiResult` | `toggle-on` | renderer |
| `set-rt-shadows` | `ToggleParams` | `SetRtShadowsResult` | `toggle-off` | renderer |
| `set-restir` | `ToggleParams` | `SetRestirResult` | `toggle-off` | renderer |
| `set-gi` | `SetGiParams` | `SetGiResult` | `gi-off` | renderer |
| `set-shadows` | `ToggleParams` | `SetShadowsResult` | `toggle-on` | renderer |
| `set-skinning` | `ToggleParams` | `SetSkinningResult` | `toggle-on` | renderer |
| `set-depth-prepass` | `ToggleParams` | `SetDepthPrepassResult` | `toggle-on` | renderer |
| `viewport-native-info` | `EmptyParams` | `ViewportNativeInfoResult` | `empty` | renderer |
| `set-viewport-size` | `SetViewportSizeParams` | `SetViewportSizeResult` | `viewport-size` | renderer |
| `set-exposure` | `SetExposureParams` | `SetExposureResult` | `exposure-zero` | renderer |
| `set-probes` | `SetProbesParams` | `SetProbesResult` | `toggle-on` | renderer |
| `help` | *(untyped)* | *(raw `{commands:[{name,help}]}`)* | *(skip: reflective registry)* | registry only |

> `set-exposure`, `set-probes`, `recapture-probes`, `list-probes` are registered in the render file in
> the C++ tree (probes/exposure are renderer-side); the manifest groups them by name. The catalog rows
> follow the manifest order but the *registration domain* is what the phase split below uses.

## Scene domain — `register_scene_commands` (47 typed)

Reach: dominated by `ctx.sceneEdit` (195 hits — selection/play/gizmo/camera/component registry),
with `ctx.renderer` (9) and `ctx.assets` (4). Imports: Core, Json, Geometry, Rendering, Scene,
SceneEdit, Assets.

| Command | Params | Result | Fixture | Reach |
|---|---|---|---|---|
| `list-entities` | `EmptyParams` | `EntityList` | `empty` | sceneEdit |
| `list-components` | `EmptyParams` | `ComponentList` | `empty` | sceneEdit (registry) |
| `create-entity` | `CreateEntityParams` | `EntityRef` | `new-entity` | sceneEdit |
| `add-entity` | `AddEntityParams` | `EntityRef` | `cube-preset` | sceneEdit, renderer, assets |
| `destroy-entity` | `EntityParams` | `DestroyEntityResult` | `temp-entity` | sceneEdit |
| `copy-entity` | `EntityParams` | `EntityRef` | `cube-entity` | sceneEdit |
| `rename-entity` | `RenameEntityParams` | `EntityRef` | `cube-rename` | sceneEdit |
| `set-parent` | `SetParentParams` | `EntityRef` | `temp-child-under-cube` | sceneEdit |
| `add-component` | `ComponentParams` | `AddComponentResult` | `temp-camera-entity` | sceneEdit (registry), + collider auto-fit |
| `remove-component` | `ComponentParams` | `RemoveComponentResult` | `temp-camera-component` | sceneEdit (registry) |
| `set-component` | `SetComponentParams` | `SetComponentResult` | `cube-name-component` | sceneEdit (registry) |
| `set-component-field` | `SetComponentFieldParams` | `SetComponentFieldResult` | `cube-name-field` | sceneEdit (registry) |
| `set-component-order` | `SetComponentOrderParams` | `SetComponentOrderResult` | `cube-component-order` | sceneEdit |
| `set-transform` | `SetTransformParams` | `EntityRef` | `cube-transform` | sceneEdit |
| `set-material` | `SetMaterialParams` | `EntityRef` | `cube-material` | sceneEdit, assets |
| `set-light` | `SetLightParams` | `EntityRef` | `temp-directional-light` | sceneEdit |
| `select` | `EntityParams` | `EntityRef` | `cube-entity` | sceneEdit |
| `deselect` | `EmptyParams` | `DeselectResult` | `empty` | sceneEdit |
| `get-selection` | `EmptyParams` | `SelectionResult` | `empty` | sceneEdit |
| `pick` | `PickParams` | `PickResult` | `viewport-center` | sceneEdit, renderer |
| `inspect` | `EntityParams` | `InspectResult` | `cube-entity` | sceneEdit (registry) |
| `focus` | `EntityParams` | `EntityRef` | `cube-entity` | sceneEdit |
| `get-world-transform` | `EntityParams` | `WorldTransformResult` | `cube-entity` | sceneEdit |
| `get-environment` | `EmptyParams` | `EnvironmentDto` | `empty` | sceneEdit |
| `set-environment` | `SetEnvironmentParams` | `EnvironmentDto` | `environment-intensity` | sceneEdit |
| `set-atmosphere` | `SetAtmosphereParams` | `EnvironmentDto` | `atmosphere-disabled` | sceneEdit |
| `get-camera` | `EmptyParams` | `EditorCamera` | `empty` | sceneEdit |
| `set-camera` | `SetCameraParams` | `EditorCamera` | `camera-yaw` | sceneEdit |
| `get-gizmo` | `EmptyParams` | `GizmoState` | `empty` | sceneEdit |
| `set-gizmo` | `SetGizmoParams` | `GizmoState` | `gizmo-rotate-local` | sceneEdit |
| `gizmo-pointer` | `GizmoPointerParams` | `GizmoPointerResult` | `gizmo-hover` | sceneEdit |
| `fly-input` | `FlyInputParams` | `FlyInputResult` | `fly-idle` | sceneEdit |
| `script-input` | `ScriptInputParams` | `ScriptInputResult` | `script-input-w` | sceneEdit |
| `play` | `EmptyParams` | `PlayStateResult` | `empty` | sceneEdit |
| `pause` | `EmptyParams` | `PlayStateResult` | `empty` | sceneEdit |
| `step` | `StepParams` | `PlayStateResult` | `step-one` | sceneEdit |
| `stop` | `EmptyParams` | `PlayStateResult` | `empty` | sceneEdit |
| `get-play-state` | `EmptyParams` | `PlayStateResult` | `empty` | sceneEdit |
| `get-script-status` | `EmptyParams` | `ScriptStatusResult` | `empty` | sceneEdit |
| `drain-script-errors` | `DrainScriptErrorsParams` | `DrainScriptErrorsResult` | `alarms-since-0` | sceneEdit |
| `drain-script-logs` | `DrainScriptLogsParams` | `DrainScriptLogsResult` | `alarms-since-0` | sceneEdit |
| `get-script-schema` | `GetScriptSchemaParams` | `GetScriptSchemaResult` | `script-schema-file` | sceneEdit |
| `set-script-override` | `SetScriptOverrideParams` | `SetScriptOverrideResult` | `script-override-slot` | sceneEdit |
| `create-script` | `CreateScriptParams` | `CreateScriptResult` | *(needs project)* | sceneEdit, assets |
| `quit` | `EmptyParams` | `QuitResult` | *(side-effecting)* | (host quit flag) |

> The exact render/scene/asset split per command follows the registration file each handler lives in;
> the few cross-domain rows (`quit`, `set-exposure`, the probe commands, the script commands) are
> resolved to a phase in §3 of the README by their registration file, not by the manifest's flat order.
> `quit` and the script-domain commands are registered in `control_commands_scene.cpp`.

## Asset domain — `register_asset_commands` (52 typed)

Reach: `ctx.assets` (88), `ctx.sceneEdit` (136), `ctx.renderer` (29), `ctx.window` (1). The single
highest-coupling domain. Imports: Core, Json, Window, Rendering, Geometry, Scene, SceneEdit, Assets.

| Command | Params | Result | Fixture | Reach |
|---|---|---|---|---|
| `get-project` | `EmptyParams` | `ProjectInfoDto` | `empty` | assets |
| `new-project` | `NewProjectParams` | `ProjectInfoDto` | `new-project` | assets, sceneEdit |
| `open-project` | `PathParams` | `ProjectInfoDto` | `project-name` | assets, sceneEdit |
| `save-project` | `OptionalPathParams` | `ProjectInfoDto` | `empty` | assets, sceneEdit |
| `load-project` | `OptionalPathParams` | `ProjectInfoDto` | `project-name` | assets, sceneEdit |
| `reload-project` | `EmptyParams` | `ProjectInfoDto` | *(stateful)* | assets, sceneEdit |
| `import-model` | `PathParams` | `ImportModelResult` | *(needs model file)* | assets |
| `instantiate-model` | `InstantiateModelParams` | `EntityRef` | *(needs model)* | assets, sceneEdit, renderer |
| `import-texture` | `PathParams` | `ImportTextureResult` | *(needs texture file)* | assets |
| `list-assets` | `EmptyParams` | `AssetList` | `empty` | assets |
| `scan-assets` | `EmptyParams` | `ScanAssetsResult` | `empty` | assets |
| `extract-subasset` | `ExtractSubAssetParams` | `AssetRef` | *(needs model)* | assets |
| `clear-extraction` | `ClearExtractionParams` | `AssetRef` | *(needs model)* | assets |
| `reimport-model` | `ReimportModelParams` | `ReimportModelResult` | *(needs model)* | assets |
| `model-info` | `ModelInfoParams` | `ModelInfoResult` | *(needs model)* | assets |
| `asset-references` | `AssetReferencesParams` | `AssetReferencesResult` | *(needs model)* | assets, sceneEdit |
| `get-asset-model` | `GetAssetModelParams` | `AssetModelResult` | *(needs model)* | assets |
| `enter-asset-preview` | `EnterAssetPreviewParams` | `AssetPreviewResult` | *(needs model)* | assets, sceneEdit, renderer |
| `exit-asset-preview` | `EmptyParams` | `PlayStateResult` | *(stateful)* | sceneEdit, renderer |
| `set-asset-preview-options` | `SetAssetPreviewOptionsParams` | `AssetPreviewOptionsResult` | *(stateful)* | sceneEdit, renderer |
| `set-active-view` | `SetActiveViewParams` | `SetActiveViewResult` | `active-view-scene` | renderer |
| `clean-assets` | `CleanAssetsParams` | `CleanReport` | `empty` | assets |
| `delete-unused` | `DeleteUnusedParams` | `DeleteUnusedResult` | *(destructive)* | assets, sceneEdit |
| `rename-asset` | `RenameAssetParams` | `AssetRef` | `mesh-asset-rename` | assets |
| `create-asset-folder` | `CreateAssetFolderParams` | `AssetList` | *(needs project fs)* | assets |
| `rename-asset-folder` | `RenameAssetFolderParams` | `AssetList` | *(needs project fs)* | assets |
| `delete-asset-folder` | `DeleteAssetFolderParams` | `AssetList` | *(destructive)* | assets |
| `move-asset` | `MoveAssetParams` | `AssetRef` | *(needs project fs)* | assets |
| `asset-usages` | `AssetUsagesParams` | `AssetUsagesResult` | `mesh-asset` | assets, sceneEdit |
| `probe-asset` | `AssetMetadataParams` | `AssetMetadataDto` | `mesh-asset` | assets |
| `delete-asset` | `DeleteAssetParams` | `DeleteAssetResult` | *(destructive)* | assets, sceneEdit |
| `assign-asset` | `AssignAssetParams` | `AssignAssetResult` | `cube-mesh-asset` | assets, sceneEdit, renderer |
| `material-create` | `MaterialCreateParams` | `MaterialCreateResult` | *(needs project)* | assets |
| `material-assign` | `MaterialAssignParams` | `MaterialAssignResult` | *(needs material)* | assets, sceneEdit |
| `material-import` | `MaterialImportParams` | `MaterialImportResultDto` | *(needs file)* | assets |
| `material-list` | `EmptyParams` | `MaterialListResult` | *(needs project)* | assets |
| `material-get` | `MaterialGetParams` | `MaterialGetResult` | *(needs material)* | assets |
| `material-update` | `MaterialUpdateParams` | `MaterialUpdateResult` | *(needs material)* | assets, renderer |
| `preview-render` | `PreviewRenderParams` | `PreviewRenderResult` | *(needs material)* | assets, renderer |
| `material-set-graph` | `MaterialSetGraphParams` | `MaterialSetGraphResult` | *(needs material)* | assets, renderer |
| `material-create-instance` | `MaterialCreateInstanceParams` | `MaterialCreateResult` | *(needs material)* | assets |
| `material-set-override` | `MaterialSetOverrideParams` | `MaterialSetOverrideResult` | *(needs material)* | assets, renderer |
| `material-compile-graph` | `MaterialCompileParams` | `MaterialCompileResult` | *(needs material)* | assets, renderer |
| `material-cook` | `EmptyParams` | `MaterialCookResult` | *(needs project)* | assets, renderer |
| `save-scene` | `PathParams` | `PathResult` | *(stateful)* | assets, sceneEdit |
| `load-scene` | `PathParams` | `PathResult` | *(stateful)* | assets, sceneEdit, renderer |
| `screenshot` | `ScreenshotParams` | `ScreenshotResult` | *(side-effecting)* | renderer, window |
| `get-thumbnail` | `ThumbnailParams` | `ThumbnailResult` | `mesh-asset` | assets, renderer |
| `view-asset` | `ThumbnailParams` | `ThumbnailResult` | `mesh-asset-view` | assets, renderer |
| `thumbnail-cache` | `ThumbnailCacheParams` | `ThumbnailCacheResult` | `thumbnail-cache-stats` | assets |

## Animation domain — `register_animation_commands` (13 typed)

Reach: `ctx.sceneEdit` (15), `ctx.assets` (4), `ctx.renderer` (2). Imports: Core, Json, Rendering,
Scene, SceneEdit, Assets.

| Command | Params | Result | Fixture | Reach |
|---|---|---|---|---|
| `get-animation-state` | `AnimationStateParams` | `AnimationStateResult` | *(needs rig)* | sceneEdit |
| `list-clips` | `ListClipsParams` | `ListClipsResult` | *(needs rig)* | sceneEdit, assets |
| `play-animation` | `PlayAnimationParams` | `AnimationStateResult` | *(needs rig)* | sceneEdit |
| `set-animation-playing` | `SetAnimationPlayingParams` | `AnimationStateResult` | *(needs rig)* | sceneEdit |
| `seek-animation` | `SeekAnimationParams` | `AnimationStateResult` | *(needs rig)* | sceneEdit |
| `set-animation-loop` | `SetAnimationLoopParams` | `AnimationStateResult` | *(needs rig)* | sceneEdit |
| `stop-preview` | `AnimationStateParams` | `AnimationStateResult` | *(needs rig)* | sceneEdit, renderer |
| `get-skeleton-overlay` | `EmptyParams` | `SkeletonOverlayResult` | `empty` | sceneEdit |
| `set-skeleton-overlay` | `SetSkeletonOverlayParams` | `SkeletonOverlayResult` | `skeleton-overlay-on` | sceneEdit |
| `get-debug-overlays` | `EmptyParams` | `DebugOverlaysResult` | `empty` | sceneEdit, renderer |
| `set-debug-overlays` | `DebugOverlaysParams` | `DebugOverlaysResult` | `debug-overlays-bounds` | sceneEdit, renderer |
| `set-skeleton-highlight` | `SetSkeletonHighlightParams` | `SkeletonOverlayResult` | *(needs rig)* | sceneEdit |
| `pick-skeleton-joint` | `PickSkeletonJointParams` | `PickSkeletonJointResult` | *(needs rig)* | sceneEdit, renderer |
| `get-foot-ik` | `GetFootIkParams` | `FootIkResult` | `cube-entity` | sceneEdit |
| `set-foot-ik` | `SetFootIkParams` | `FootIkResult` | `foot-ik-on` | sceneEdit |

> The animation file registers 13 `registerCommand` invocations; `get-foot-ik`/`set-foot-ik` and the
> skeleton-overlay pair are counted here as the animation domain even though the manifest interleaves
> them — the phase split uses the registration file.

## Physics domain — `register_physics_commands` (12 typed)

Reach: `ctx.physics` (24 — null in Edit, handlers report inactive not error), `ctx.sceneEdit` (10).
Imports: Core, Scene, SceneEdit, Physics. **This domain is the one that touches the nullable
`physics` field**; every handler guards `physics == null` and returns an inactive/empty result
(never an error) so the editor polls unconditionally.

| Command | Params | Result | Fixture | Reach |
|---|---|---|---|---|
| `physics-state` | `EmptyParams` | `PhysicsStateResult` | `empty` | physics (nullable) |
| `physics-bodies` | `EmptyParams` | `PhysicsBodiesResult` | `empty` | physics (nullable) |
| `fit-collider` | `FitColliderParams` | `FitColliderResult` | *(needs collider)* | sceneEdit |
| `apply-impulse` | `ApplyImpulseParams` | `ApplyImpulseResult` | *(needs play)* | physics |
| `drain-contacts` | `DrainContactsParams` | `DrainContactsResult` | `alarms-since-0` | physics |
| `set-kinematic-bones` | `SetKinematicBonesParams` | `KinematicBonesResult` | *(needs rig)* | sceneEdit |
| `move-character` | `MoveCharacterParams` | `MoveCharacterResult` | *(needs play)* | physics |
| `raycast` | `RaycastParams` | `RaycastResult` | *(needs play)* | physics |
| `shapecast` | `ShapecastParams` | `RaycastResult` | *(needs play)* | physics |
| `enable-ragdoll` | `EnableRagdollParams` | `RagdollResult` | *(needs rig)* | sceneEdit |
| `set-ragdoll` | `SetRagdollParams` | `RagdollResult` | *(needs rig)* | physics, sceneEdit |
| `get-ragdoll` | `GetRagdollParams` | `RagdollResult` | *(needs rig)* | sceneEdit |

---

## Wire-helper types (the byte-frozen seam)

These four types in `control_dto.cppm` carry the load-bearing encoding behaviors that fail silently
if they drift. They map to `saffron-protocol` newtypes whose `serde` derive must reproduce the
generated-serde behavior byte-for-byte (PP-7 owns the derive; this catalog pins the contract).

| Type | C++ | Wire emit | Wire accept | Rust target |
|---|---|---|---|---|
| `WireUuid` | `struct { u64 value; }` | decimal **string** (`uuidToJson`, `:645`) | string **or** number, whole-string parse (`readWireUuid`, `:157`) | `Uuid` newtype, `serde_with::PickFirst<(DisplayFromStr, _)>` |
| `EntitySelector` | `struct { Json value; }` | passthrough | any json (uuid-or-name; resolved by `resolve_entity`) | opaque `serde_json::Value` |
| `AssetSelector` | `struct { Json value; }` | passthrough | any json (uuid-or-path) | opaque `serde_json::Value` |
| `Json` (field type) | `nlohmann::json` | passthrough | any json (component blobs, material graphs, override values) | `serde_json::Value` |

Enum wire spelling: every `*Dto`/preset enum serializes as a **kebab-case string** (e.g.
`AddEntityPreset::PointLight` ↔ `"point-light"`, `GizmoOpDto::Translate` ↔ `"translate"`), with an
unknown value → a typed error (`readAddEntityPreset` etc., `:199`+). The 17 enums:
`AddEntityPreset`, `PickKind`, `GizmoOpDto`, `GizmoSpaceDto`, `GizmoPointerPhase`, `AaModeDto`,
`GiModeDto`, `ViewModeDto`, `AssetSlotDto`, `ScreenshotTargetDto`, `AssetTypeDto`, `ProfilerModeDto`,
`ProfileLaneDto`, `CaptureModeDto`, `CaptureStateDto`, `AlarmSeverityDto`, `AlarmStateDto`.

## DTO inventory

`control_dto.cppm` declares **236 structs** (including the 4 wire-helpers + `DtoTag` + `Vec3`/`Vec4`)
and **17 enums**. The full struct list (the codegen and the `saffron-protocol` crate enumerate it):

```
DtoTag WireUuid EntitySelector AssetSelector EntityRef Vec3 Vec4
PingParams EmptyParams PingResult
RenderStatsDto RenderPassTimingDto RenderPassTimingsDto ProfilerSetModeParams ProfilerModeResult
PipelineStatsDto ProfileSpanDto ProfileCaptureMetadataDto ProfileCaptureDto CaptureStartParams
CaptureStartResult CaptureStopResult CaptureStatusResult FrameSampleDto FrameHistoryParams
FrameHistoryDto PerfConfigDto SetPerfConfigParams AlarmEventDto DrainAlarmsParams DrainAlarmsResult
ScriptStatusResult PhysicsStateResult FitColliderParams FitColliderResult ContactEventDto
DrainContactsParams DrainContactsResult PhysicsBodyDto PhysicsBodiesResult ApplyImpulseParams
ApplyImpulseResult SetKinematicBonesParams KinematicBonesResult MoveCharacterParams
MoveCharacterResult RaycastParams ShapecastParams RaycastResult EnableRagdollParams RagdollResult
SetRagdollParams GetRagdollParams ScriptErrorDto DrainScriptErrorsParams DrainScriptErrorsResult
ScriptLogDto DrainScriptLogsParams DrainScriptLogsResult GetScriptSchemaParams ScriptFieldDto
GetScriptSchemaResult SetScriptOverrideParams SetScriptOverrideResult CreateScriptParams
CreateScriptResult ActiveAlarmDto ActiveAlarmsDto SetAaParams SetAaResult SetViewModeParams
SetViewModeResult ToggleParams SetClusteredResult SetIblResult SetSsaoResult SetContactShadowsResult
SetSsgiResult SetRtShadowsResult SetRestirResult SetGiParams SetGiResult SetShadowsResult
SetSkinningResult SetDepthPrepassResult ViewportNativeInfoResult SetViewportSizeParams
SetViewportSizeResult SetActiveViewParams SetActiveViewResult ProjectInfoDto NewProjectParams
PathParams OptionalPathParams ImportModelResult InstantiateModelParams ExtractSubAssetParams
ClearExtractionParams ImportTextureResult AssetEntryDto AssetList ScanAssetsResult
ReimportModelResult ReimportModelParams ModelInfoParams ModelSubAssetDto ModelInfoResult
AssetReferencesParams AssetReferencesResult CleanCandidateDto CleanReport CleanAssetsParams
DeleteUnusedParams DeleteUnusedResult RenameAssetParams AssetRef CreateAssetFolderParams
RenameAssetFolderParams DeleteAssetFolderParams MoveAssetParams AssetUsagesParams AssetUsageDto
AssetUsagesResult AssetMetadataParams AssetMetadataDto DeleteAssetParams DeleteAssetResult
AssignAssetParams MaterialCreateParams MaterialCreateResult MaterialAssignParams MaterialAssignResult
MaterialImportParams MaterialImportResultDto MaterialRefDto MaterialListResult MaterialGetParams
MaterialGetResult MaterialUpdateParams MaterialUpdateResult PreviewRenderParams PreviewRenderResult
MaterialSetGraphParams MaterialSetGraphResult MaterialCreateInstanceParams MaterialSetOverrideParams
MaterialSetOverrideResult MaterialCompileParams MaterialCompileResult MaterialCookResult
AssignAssetResult PathResult ScreenshotParams ScreenshotResult ThumbnailParams ThumbnailResult
ThumbnailCacheParams ThumbnailCacheResult QuitResult CreateEntityParams EntityParams SetParentParams
DestroyEntityResult EntityListEntry EntityList ComponentList ComponentParams AddComponentResult
RemoveComponentResult SetComponentParams SetComponentResult SetComponentOrderParams
SetComponentOrderResult SetTransformParams SetMaterialParams SetLightParams PickParams PickResult
InspectResult EnvironmentDto SetEnvironmentParams SetAtmosphereParams SelectionResult PlayStateResult
AnimationClipDto BoneDto AssetCapabilitiesDto GetAssetModelParams AssetModelResult
EnterAssetPreviewParams BoneEntityDto AssetPreviewResult ListClipsParams ListClipsResult
PlayAnimationParams SeekAnimationParams SetAnimationLoopParams SetAnimationPlayingParams
AnimationStateParams AnimationStateResult SetSkeletonOverlayParams SkeletonOverlayResult
DebugOverlaysParams DebugOverlaysResult SetSkeletonHighlightParams PickSkeletonJointParams
PickSkeletonJointResult SetAssetPreviewOptionsParams AssetPreviewOptionsResult SetFootIkParams
GetFootIkParams FootIkResult WorldTransformResult StepParams DeselectResult AddEntityParams
RenameEntityParams SetComponentFieldParams SetComponentFieldResult EditorCamera SetCameraParams
GizmoState SetGizmoParams GizmoPointerParams GizmoPointerResult FlyInputParams FlyInputResult
ScriptInputParams ScriptInputResult SetProbesParams SetProbesResult RecaptureProbesResult ProbeRef
ListProbesResult SetExposureParams SetExposureResult
```

Field-shape facts the codegen must honor (the silent-failure surface):

- **`std::optional<T>` → `Option<T>`**, written as a missing key (not `null`) on the wire and read
  leniently (`optionalField` returns absent on missing). e.g. `RaycastParams.maxDist`,
  `PickResult.{id,name,kind}`, `EntityListEntry.{parentId,bone}`, `SetComponentFieldParams.index`.
- **`std::vector<T>` → `Vec<T>`**, a JSON array. e.g. `EntityList.entities`, `ComponentList.components`,
  `InspectResult.componentOrder`, `PhysicsBodiesResult.bodies`.
- **`Json`-typed fields are opaque passthrough** — `InspectResult.components`,
  `SetComponentFieldParams.value`, `MaterialSetGraphParams.graph`, `SetScriptOverrideParams.value` —
  map to `serde_json::Value` and are NOT modeled as typed sub-DTOs (they carry component/graph/override
  blobs whose shape the scene component registry, not the protocol crate, defines).
- **`WireUuid` fields emit decimal strings**; `f32`/`f64` are JSON numbers; `f32` reads narrow an f64
  wire value (`readF32`, `:87`).
- **Field declaration order = positional arg order** — preserved by codegen so `sa cmd <positional>`
  matches `{key: value}`.
