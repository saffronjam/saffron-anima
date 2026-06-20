# Phase 3 — Scene command domain

**Status:** COMPLETED

> **Implementation note.** `register_scene_commands` (`crates/control/src/commands_scene.rs`) registers
> the **42** scene-domain handlers in the C++ registration order, inserted between the render group and
> the animation group in `register_builtin_commands` (render → scene → animation → physics → asset).
> The C++ `control_commands_scene.cpp` also registers five commands the Rust port groups by their
> registration file instead: `set-probes` / `recapture-probes` / `list-probes` live in the render file
> (`commands_render.rs`, renderer-side), and `get-debug-overlays` / `set-debug-overlays` are the
> animation domain (`commands_animation.rs`). The catalog's scene-table also lists `quit`,
> `create-script`, and `get-script-schema` as cross-domain rows: `quit` and `create-script` register in
> the asset file (matching the C++ `control_commands_asset.cpp`), and `get-script-schema` is
> host-registered (it needs the Lua schema reader). Substrate added to land this phase:
> `SceneEditContext::registry_and_active_scene` (the disjoint registry + active-scene borrow — see
> `03-ecs-and-scene/phase-8`). The `get/set-debug-overlays` pair (manifest-interleaved with the
> skeleton-overlay group) is registered between `set-skeleton-overlay` and `set-skeleton-highlight` in
> `commands_animation.rs`, reusing `SceneEditContext::debug_overlays`. A manifest-completeness contract
> test (`registry::tests::registry_covers_the_protocol_manifest`) now asserts every
> `saffron_protocol::COMMANDS` row has a handler (minus the host-registered `get-script-schema` and the
> reflective `help`), so a manifest command without a registration trips at build/test time.

**Depends on:** 09-control-plane:phase-1-socket-server-and-dispatch, 03-ecs-and-scene (scene world + the component registry), 08-host-and-viewport (SceneEditContext: selection/gizmo/play), 12-scripting (the script-status/drain/schema/override surface)

## Goal

Register the 47 scene-domain commands (`register_scene_commands`): entity lifecycle
(create/add/destroy/copy/rename/parent), the registry-driven component commands (add/remove/set/
set-field/order), selection (select/deselect/get-selection), picking + inspect + focus +
world-transform, the editor camera + gizmo + fly/script input, the play-state machine
(play/pause/step/stop/get-play-state), environment + atmosphere, the scripting surface
(status/drain-errors/drain-logs/get-schema/set-override/create-script), and `quit`. This is the
largest and most `sceneEdit`-coupled domain (195 `ctx.sceneEdit` hits).

## Why this shape (NO LEGACY)

- **Reach: `sceneEdit` heavily, plus `renderer` and `assets`.** The handlers drive the
  `SceneEditContext` (selection, gizmo state, play machine, the active-scene resolution) and read the
  scene world. `set-material`/`add-entity`/`pick` also touch `renderer`/`assets`. So this phase depends
  on scene, sceneedit, and (for the script commands) scripting — and lands green once those exist.
- **Component commands are registry-driven, not per-component.** `add-component`/`remove-component`/
  `set-component`/`set-component-field`/`inspect` resolve a component by name through the
  `SceneEditContext` component registry (`findByName(ctx.sceneEdit.registry, params.component)`,
  `control_commands_scene.cpp:516`) and operate generically. The registry itself is the scene-component
  serde codegen (owned by `03-ecs-and-scene` + PP-7's component-registration macro); this phase
  *consumes* it via `find_by_name` and the generic component setters. The component value travels as an
  opaque `Json`/`serde_json::Value` blob (`SetComponentFieldParams.value`, `InspectResult.components`)
  whose shape the registry — not `saffron-protocol` — defines, so these stay `Value` passthroughs, not
  typed sub-DTOs.
- **`resolve_entity` is the shared id-or-name selector.** Every entity-taking command resolves
  `EntitySelector` via `resolve_entity(ctx, params)` (`command.cppm:88` / `:87` impl): UUID first
  (a numeric string counts as a UUID, parsed whole-string), then a `NameComponent` scan, against the
  **active** scene (so it finds runtime entities during play). `entity_ref_dto` builds the
  `{id: decimal-string, name}` reply. Both port as shared helpers, not duplicated per handler.
- **The play-state machine returns `PlayStateResult` uniformly.** `play`/`pause`/`step`/`stop`/
  `get-play-state` all report the same `PlayStateResult` shape; `step` takes `StepParams` (frame
  count). The state transitions live in `SceneEditContext` (`08-host-and-viewport`); this phase is the
  thin command wrapper.
- **The script-domain commands are registered in the scene file** (per the C++ tree) so they belong to
  this phase, not a separate scripting-control phase: `get-script-status`, `drain-script-errors`,
  `drain-script-logs`, `get-script-schema`, `set-script-override`, `create-script`. They read the
  script runtime's error/log rings and the declared-field schema (`12-scripting`). The
  borrowed-pointer/session-guard invariant is the scripting crate's concern; the control handlers just
  drain typed rings.
- **`quit` is a scene-file command** (sets the host quit flag) — kept here, returning `QuitResult`.
- **`fly-input`/`script-input` feed the editor camera / play input** — the edge-vs-down semantics the
  recent C++ commits settled travel as the `FlyInputParams`/`ScriptInputParams` DTOs; ported as-is.

## Grounding (real files/symbols)

- `engine-old/source/saffron/control/control_commands_scene.cpp`
  - `registerSceneCommands` (the 47-command block).
  - Registry-driven component dispatch: `findByName(ctx.sceneEdit.registry, ...)` (`:516`, `:550`,
    `:596`, `:632`, `:787`, `:877`); `removeComponentOrder` (`:560`); the registry walk in `inspect`
    (`:995`); `addComponent<...>` for presets (`:1418`+).
  - `EntitySelector` resolution via `resolveEntity`/`entityRefDto` (`command.cppm:87`/`:145`).
- DTOs: `CreateEntityParams`, `AddEntityParams`, `EntityParams`, `SetParentParams`, `RenameEntityParams`,
  `ComponentParams`, `AddComponentResult`/`RemoveComponentResult`, `SetComponentParams`/
  `SetComponentResult`, `SetComponentFieldParams`/`SetComponentFieldResult`, `SetComponentOrderParams`/
  `SetComponentOrderResult`, `SetTransformParams`, `SetMaterialParams`, `SetLightParams`,
  `EntityList`/`EntityListEntry`, `ComponentList`, `DestroyEntityResult`, `PickParams`/`PickResult`,
  `InspectResult`, `WorldTransformResult`, `SelectionResult`/`DeselectResult`, `PlayStateResult`/
  `StepParams`, `EnvironmentDto`/`SetEnvironmentParams`/`SetAtmosphereParams`, `EditorCamera`/
  `SetCameraParams`, `GizmoState`/`SetGizmoParams`/`GizmoPointerParams`/`GizmoPointerResult`,
  `FlyInputParams`/`FlyInputResult`, `ScriptInputParams`/`ScriptInputResult`, `ScriptStatusResult`,
  `ScriptErrorDto`/`DrainScriptErrorsParams`/`DrainScriptErrorsResult`, `ScriptLogDto`/
  `DrainScriptLogsParams`/`DrainScriptLogsResult`, `GetScriptSchemaParams`/`ScriptFieldDto`/
  `GetScriptSchemaResult`, `SetScriptOverrideParams`/`SetScriptOverrideResult`, `CreateScriptParams`/
  `CreateScriptResult`, `QuitResult`, `EntityRef` — all in `control_dto.cppm`.
- Enums: `AddEntityPreset`, `PickKind`, `GizmoOpDto`, `GizmoSpaceDto`, `GizmoPointerPhase`
  (`control_dto.cppm:57`+).
- `09-control-plane/catalog.md` — the scene-domain table (47 rows, incl. cross-domain `quit`/script
  rows) + fixtures.

## Acceptance gate

- `cargo build -p saffron-control` green with the scene handlers registered; clippy/fmt clean.
- `cargo test -p saffron-control` passes scene-domain unit tests over a stub scene + registry:
  - `create-entity`/`destroy-entity` round-trip; the returned `EntityRef.id` is a decimal string.
  - `resolve_entity` finds by UUID (numeric string), by name, and errors with the dumped selector when
    absent (mirrors `resolveEntity`'s `Err`).
  - `add-component`/`set-component-field` dispatch through the registry by component name; an unknown
    component name is a typed error; `set-component-field` with an `index` merges into an array element.
  - `inspect` returns `{id, name, components:<Value>, componentOrder:[...]}` in registry order.
  - `play`/`pause`/`step`/`stop` produce the expected `PlayStateResult` transitions.
- The wire-contract test validates every scene-domain command's live `result` against OpenRPC + `help`
  against the manifest, using the catalog fixtures (`new-entity`, `cube-preset`, `temp-entity`,
  `cube-rename`, `temp-child-under-cube`, `temp-camera-entity`, `cube-name-component`,
  `cube-name-field`, `cube-component-order`, `cube-transform`, `cube-material`,
  `temp-directional-light`, `cube-entity`, `viewport-center`, `environment-intensity`,
  `atmosphere-disabled`, `camera-yaw`, `gizmo-rotate-local`, `gizmo-hover`, `fly-idle`,
  `script-input-w`, `step-one`, `alarms-since-0`, `script-schema-file`, `script-override-slot`,
  `empty`).
- Component blobs and override values remain opaque `Value` passthroughs (no typed-DTO drift); all
  entity ids stay decimal strings.
