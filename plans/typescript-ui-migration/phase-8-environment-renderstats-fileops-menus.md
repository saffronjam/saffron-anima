# Phase 8: Environment panel + Render Stats + save/load project/scene + menus

**Status:** NOT STARTED

<!-- Flip to COMPLETED when the "Done when" checklist passes, validation-clean. Delete this file only after COMPLETED + merged. -->

## Goal

Complete the data-panel surface of the TypeScript/Tauri editor: an **Environment** panel (sky/ambient) bound to `get-environment`/`set-environment`, a **Render Stats** readout with render toggles (gating RT-only toggles on `rtSupported`), and a **menu bar** with File (Save/Load Project, Save/Load Scene, Import) + Create (reuse the phase-2 `add-entity` presets), wired through Tauri file dialogs. All of these reuse control commands that already exist on `main` (no new control commands in this phase); the only engine-side dependency is the `add-entity` preset command introduced in phase-2. After a project/scene load the Zustand store + selection must be fully reset so no stale entities or selection survive.

This phase introduces NO new control commands. It is pure frontend work plus a Tauri capability/plugin addition (`tauri-plugin-dialog`).

**Depends on:** phase-7 (asset browser + `AssetPicker`/`AssetTile` components + the typed client + Zustand store + the reconcile poll). Transitively depends on phase-2 (`add-entity`, schema-first `@saffron/protocol`, `dump-schema`/contract test) and phase-3 (typed client, store, `LoadingOverlay`, file-dialog capability scaffolding).

## Current state (verified)

All engine-side commands this phase consumes already exist on `main` and return the shapes below. They are reached via the phase-3 generic `control(cmd, params)` passthrough + the phase-3 typed `client.ts`. No engine changes are required here.

### Environment (`get-environment` / `set-environment`)

- `get-environment` returns `environmentToJson(scene.environment)` — `control_commands_scene.cpp:364-368`.
- `set-environment` is a **merge over the current environment** (same wire shape as the scene file's `"environment"` block): it seeds `body` from `environmentToJson` then overlays a `--json` blob and/or individual named fields (`skyMode`, `clearColor`, `skyTexture`, `skyIntensity`, `skyRotation`, `exposure`, `visible`, `useSkyForAmbient`, `ambientColor`, `ambientIntensity`), reassigns `scene.environment = environmentFromJson(body)`, and returns the new `environmentToJson` — `control_commands_scene.cpp:373-397`. So the TS side may send a `Partial<Environment>` of named fields and unspecified fields are preserved.
- Wire DTO `environmentToJson` (`scene.cppm:381-395`):
  - `skyMode`: string enum `"color" | "texture" | "procedural"` (`skyModeName`, `scene.cppm:361-370`).
  - `clearColor`: `Vec3 {x,y,z}` (Color-mode + clear fallback tint).
  - `skyTexture`: **u64 emitted raw** (`env.skyTexture.value`) → **typed as `string` end-to-end** per the cross-cutting u64-as-string rule; `0` = none.
  - `skyIntensity`, `skyRotation` (yaw **radians**), `exposure` (reserved on the wire — see below): floats.
  - `visible`, `useSkyForAmbient`: bools.
  - `ambientColor`: `Vec3`; `ambientIntensity`: float (non-IBL fallback ambient).
- **Exposure caveat**: `SceneEnvironment.exposure` is serialized but reserved; the actual tonemap exposure is the **renderer's** `set-exposure {ev}` (stops, exp2) — `control_commands_render.cpp:263-273`, returning `{ exposureEv }`. The env panel's exposure slider must call `set-exposure` (NOT `set-environment.exposure`), exactly as the C++ Environment panel does NOT expose exposure (`editor_panels.cpp:214-221` has Intensity/Rotation/Visible/Use-Sky-For-Ambient/Ambient-Color/Ambient-Intensity, no exposure field) and exposure lives on the render side. The exposure control therefore belongs to the Render Stats / render toggles area as well; surface it once and route it to `set-exposure`.
- C++ Environment panel layout for parity (`editor_panels.cpp:191-224`): Sky Mode combo (`Color/Texture/Procedural`); when Color → Clear Color (`ColorEdit3`); when Texture → Sky Texture asset picker (`drawAssetPicker(..., AssetType::Texture, ...)`, `editor_panels.cpp:211`); then Intensity drag (`0..100`), Rotation drag (`-2π..2π`), Visible checkbox; a separator; Use Sky For Ambient checkbox, Ambient Color, Ambient Intensity drag (`0..10`).

### Render Stats (`render-stats`)

- `render-stats` (`control_commands_render.cpp:38-61`) returns a **flat bag** (camelCase):
  - counters (u32): `drawCalls`, `batches`, `instances`, `blasCount`, `pipelines`.
  - bools: `clustered`, `depthPrepass`, `shadows`, `ibl`, `ssao`, `contactShadows`, `ssgi`, `ddgi`, `rtSupported`, `rtShadows`, `restir`, `hdr` (always `true`).
  - `exposureEv`: f32; `aa`: string `off|fxaa|taa|msaa2|msaa4|msaa8`.
- **No `fps` on the wire.** The C++ editor's fps is `ImGui::GetIO().Framerate` measured **in the editor process** (`editor_app.cppm:362-368` — `io.Framerate`, `1000/io.Framerate` ms). Over the socket there is no frame timing. The TS panel must compute fps **client-side** (EMA over the reconcile-poll tick interval, or just label the engine-side fields and omit fps; preferred: a client-side EMA derived from the poll cadence and labelled as "UI poll fps", not engine frame rate — the embedded native window paints independently of the webview, so do not claim it as the renderer fps). State this distinction in the panel.
- Render toggles map 1:1 to existing setters (each returns its echoed state):
  - `set-aa {off|fxaa|taa|msaa2|msaa4|msaa8}` → `{ aa }` (`control_commands_render.cpp:63-86`).
  - `set-clustered {0|1}` → `{ clustered }` (`:88-108`).
  - `set-ibl {0|1}` → `{ ibl }` (`:110-130`).
  - `set-ssao {0|1}` → `{ ssao }` (`:132-152`).
  - `set-contact-shadows {0|1}` → `{ contactShadows }` (`:154-168`).
  - `set-ssgi {0|1}` → `{ ssgi }` (`:170-184`).
  - `set-shadows {0|1}` → `{ shadows }` (`:241-261`).
  - `set-gi {off|ddgi}` → `{ ddgi }` (`:226-239`).
  - `set-depth-prepass {0|1}` → `{ depthPrepass }` (`:275-295`).
  - `set-exposure {ev}` → `{ exposureEv }` (`:263-273`).
  - **RT-gated** (return a typed error when unsupported): `set-rt-shadows {0|1}` → returns `Err("ray tracing not supported on this device")` if `!rtSupported` then `{ rtShadows }` (`:186-204`); `set-restir {0|1}` → same guard then `{ restir }` (`:206-224`).

### File ops (`save-project` / `load-project` / `save-scene` / `load-scene` / `screenshot`)

- `save-project {path}` defaults `path` to `"project.json"`, writes catalog + scene, sets `editor.scenePath`, returns `{ path }` — `control_commands_asset.cpp:177-188`.
- `load-project {path}` (default `"project.json"`) restores catalog + scene + GPU assets via `loadProject(assets, renderer, registry, scene, path)`, sets `scenePath`, **clears selection** (`setSelection(editor, Entity{entt::null})`), returns `{ path }` — `:190-203`.
- `save-scene {path}` requires a non-empty `path`, returns `{ path }` — `:142-157`.
- `load-scene {path}` requires `path`, **clears selection**, returns `{ path }` — `:159-175`.
- `screenshot {target:viewport|window, path}` requires `path`; `target=viewport` is synchronous → `{ target, path, pending:false }`; `target=window` is written at end-of-frame → `{ target, path, pending:true }` — `:205-234`. **Window screenshots break under present-only mode** (phase-1 note); the UI should default to `viewport` and surface `pending` honestly (do not assume the file exists synchronously when `pending:true`).
- Import: `import-model {path}` → entityRef-like result incl. `{ mesh, albedoTexture }` (`:24-46`); `import-texture {path}` (`:47-62`); both add to the catalog. `list-assets` (`:63-73`).

### Create presets

- Phase-2 adds `add-entity {preset: empty|cube|model|point-light|spot-light|directional-light|camera}` → `entityRef`. This phase's Create menu calls it. The C++ Create menu it mirrors is `editor_panels.cpp:386-423` (Empty/Cube + Point/Spot/Directional Light + Camera; the "Model" preset maps to the bundled-cube import path the C++ `onCreateCube` used, `editor_app.cppm:186-206`).

### Frontend state today

- The MVP has **no** dialog plugin and **no** menu/stats/environment UI: capabilities are `core:default` only (`wt:editor/src-tauri/capabilities/default.json`), no `tauri-plugin-dialog` in `wt:editor/src-tauri/Cargo.toml`, and `wt:editor/src/main.tsx` has only a `<header className="topbar">` (`:207`) with no File/Create menus. The Zustand store, typed `client.ts`, `AssetPicker`, and reconcile poll arrive in phases 3/6/7. This phase consumes them; it must add `tauri-plugin-dialog` (capability + Cargo dep) since phase-3 only scaffolded the dialog permission scope.

### se-current / docs obligation

Per AGENTS.md, every new `se` command must appear in `tools/se` `printResult` + `docs/`. **This phase adds no new control commands**, so there is no `se`/docs delta for the engine — but verify the existing `render-stats` `printResult` branch (`tools/se/source/main.cpp:166-174`) still reflects the wire fields the TS panel relies on (it currently prints only `drawCalls/batches/instances/clustered`; the full bag is available via `-o json`). No change required; note it so a future session does not mistake the partial text formatter for the wire contract.

## Implementation

Ordered steps. All frontend paths are under `editor/`. Reuse the typed `client.ts` (phase-3), `@saffron/protocol` generated types (phase-2/3), the Zustand `store.ts` (phase-3), and the phase-6/7 field widgets (`VectorEditor`, `NumberDrag`, `ColorField`, `ComboField`, `AssetPicker`). Do NOT call `invoke` directly anywhere — always go through `client.ts` typed methods.

### 1. Protocol + typed client surface

1.1. Confirm `@saffron/protocol` already exposes (from the phase-2 schemas, regenerated by `editor/scripts/gen-protocol.ts`): `Environment` (with `skyTexture: string`, `skyMode: 'color'|'texture'|'procedural'`, `clearColor`/`ambientColor: Vec3`, the floats/bools), `RenderStats` (the 21-field bag, `aa` as the literal union, `rtSupported: boolean`, all ids/counters typed correctly), `SaveLoadResult { path: string }`, and `ScreenshotResult { target: 'viewport'|'window', path: string, pending: boolean }`. If any are missing from the schema, add `schemas/control/environment.schema.json`, `render-stats.schema.json`, `save-load.schema.json`, `screenshot.schema.json` result schemas (draft 2020-12, camelCase, u64-as-string) and re-run `bun run gen-protocol`; extend the phase-2 contract test (`tools/check-control-schema`) to validate live `get-environment`/`render-stats`/`save-project`/`screenshot` outputs.

1.2. Add typed methods to `editor/src/control/client.ts` mapped against `CommandResultMap` (named params only, ids as strings):
- `getEnvironment(): Promise<Environment>`
- `setEnvironment(patch: Partial<Environment>): Promise<Environment>` — sends only the named fields present in `patch` (the engine merges); `skyTexture` is a `string`.
- `setExposure(ev: number): Promise<{ exposureEv: number }>`
- `renderStats(): Promise<RenderStats>` (likely already added in phase-3 — reuse)
- The render toggles: `setAa(mode)`, `setClustered(b)`, `setIbl(b)`, `setSsao(b)`, `setContactShadows(b)`, `setSsgi(b)`, `setShadows(b)`, `setGi('off'|'ddgi')`, `setDepthPrepass(b)`, `setRtShadows(b)`, `setRestir(b)` — each returns the echoed `{field}`; the RT ones may reject with the typed error (surface as a rejection, do not silently swallow).
- `saveProject(path: string)`, `loadProject(path: string)`, `saveScene(path: string)`, `loadScene(path: string)` → `SaveLoadResult`.
- `screenshot(target: 'viewport'|'window', path: string): Promise<ScreenshotResult>`.
- `importModel(path)`, `importTexture(path)` (reuse from phase-7 if present).
- `addEntity(preset)` (reuse from phase-5/2 if present) for the Create menu.

### 2. Tauri dialog plugin + capabilities

2.1. Add `tauri-plugin-dialog` to `editor/src-tauri/Cargo.toml` and register it in `editor/src-tauri/src/lib.rs` (`.plugin(tauri_plugin_dialog::init())`). Add the matching JS dep `@tauri-apps/plugin-dialog` to `editor/package.json`.

2.2. Extend `editor/src-tauri/capabilities/default.json` permissions with `dialog:allow-open`, `dialog:allow-save`, and (if not already present) the `core:event` / `event:default` permissions the phase-3 lifecycle uses. Keep the window scope `["main"]`.

2.3. File dialog helper in the frontend (e.g. `editor/src/control/dialogs.ts` or inline in `MenuBar.tsx`): wrap `open({ filters: [...] })` / `save({ filters: [...], defaultPath })` from `@tauri-apps/plugin-dialog`. Filters: project/scene → `*.json`; import-model → `*.gltf,*.glb,*.obj`; import-texture → `*.png,*.jpg,*.jpeg`. Default the project path to the engine's `scenePath` if known (otherwise `project.json`); note paths are sent to the engine which resolves them relative to its cwd (repo root, per phase-3 spawn) — pass absolute paths returned by the dialog.

### 3. EnvironmentPanel (`editor/src/panels/EnvironmentPanel.tsx`)

3.1. On mount and on every project/scene load (subscribe to the store's `sceneVersion` or a `loadCounter`), fetch `client.getEnvironment()` into local panel state (or a slice `store.environment`). The reconcile poll (phase-3) may also refresh `environment` cheaply; gate writes OFF during an active drag to avoid clobbering optimistic local state (reuse the phase-3 coalesced-write helper).

3.2. Render parity with `editor_panels.cpp:191-224`:
- **Sky Mode** `ComboField` (`Color | Texture | Procedural`) → `setEnvironment({ skyMode })`.
- When `skyMode === 'color'`: **Clear Color** `ColorField` (Vec3) → `setEnvironment({ clearColor })`.
- When `skyMode === 'texture'`: **Sky Texture** `AssetPicker` filtered to texture assets (phase-7), value `skyTexture: string`. Assigning calls `setEnvironment({ skyTexture })` (NOT `assign-asset`, which only covers entity mesh/albedo). Drag-drop from the Assets panel targets this field via `setComponentField`-style assign → here it is `setEnvironment({ skyTexture })`.
- **Intensity** `NumberDrag` (0..100, step 0.01) → `setEnvironment({ skyIntensity })`.
- **Rotation** `NumberDrag` — **store/wire is radians**; display **degrees** in the UI (convert `rad↔deg`), range `-360..360`, step ~0.5° → `setEnvironment({ skyRotation })` (radians on the wire). Document the unit in the label (e.g. "Rotation (°)").
- **Visible** checkbox → `setEnvironment({ visible })`.
- separator.
- **Use Sky For Ambient** checkbox → `setEnvironment({ useSkyForAmbient })`.
- **Ambient Color** `ColorField` → `setEnvironment({ ambientColor })`.
- **Ambient Intensity** `NumberDrag` (0..10, step 0.005) → `setEnvironment({ ambientIntensity })`.

3.3. All hot fields (drags/sliders) route through the coalesced-write helper so dragging a slider does not flood the socket. Each `setEnvironment` response (the merged env) updates local state so the panel stays consistent if the engine clamps.

### 4. RenderStatsPanel (`editor/src/panels/RenderStatsPanel.tsx`)

4.1. Read `store.renderStats` (populated every reconcile tick by the phase-3 poll; the poll already calls `render-stats` per spec). If the store does not yet poll render-stats, add it to the focus-gated poll (cheap, version-independent).

4.2. Readout section (top): counters `drawCalls`, `batches`, `instances`, `pipelines`, `blasCount`; `aa` mode; `exposureEv`; `hdr`. Plus a **client-side fps/poll-rate** computed as an EMA of the actual poll interval (label it "UI poll" not "engine fps" — there is no engine frame timing on the wire). Mono font for numbers (phase-9 theme; until then a `<code>`/monospace class).

4.3. Toggles section: checkboxes / segmented controls bound to the live `renderStats` booleans, each calling its setter then optimistically updating the store from the echoed `{field}` (or refetching `render-stats`):
- `aa`: a segmented/`ComboField` `off|fxaa|taa|msaa2|msaa4|msaa8` → `setAa`.
- `clustered`, `depthPrepass`, `shadows`, `ibl`, `ssao`, `contactShadows`, `ssgi` → boolean toggles.
- `ddgi`: a 2-state `off|ddgi` → `setGi`.
- `exposureEv`: a `NumberDrag` (stops, e.g. -8..8) → `setExposure` (this is the single home for tonemap exposure, distinct from the env's reserved `exposure`).
- **RT-gated** `rtShadows`, `restir`: render the toggles **disabled** when `renderStats.rtSupported === false` (with a tooltip "Ray tracing not supported on this device"). When enabled and toggled, call `setRtShadows`/`setRestir`; wrap in try/catch — if the engine still rejects (typed error path, `control_commands_render.cpp:189-192/209-212`), surface a transient toast/inline error and revert the optimistic state.

4.4. After a `setAa`/toggle, refetch `render-stats` on the next poll tick so derived counters (e.g. `batches` under MSAA) stay accurate.

### 5. MenuBar (`editor/src/app/MenuBar.tsx`)

5.1. Replace the MVP placeholder `<header className="topbar">` content (`wt:main.tsx:207`) — actually built fresh under the phase-3 `app/` tree — with a menu bar mirroring `editor_panels.cpp:355-426`:
- **File** menu:
  - *Save Project* → `save()` dialog (`*.json`, default `project.json`/last path) → `client.saveProject(path)`; on success store `scenePath` and toast.
  - *Load Project* → `open()` dialog → `client.loadProject(path)` → **full store reset** (see step 6).
  - *Save Scene* / *Load Scene* → same pattern via `saveScene`/`loadScene` (scene-only; Load Scene also resets store like Load Project).
  - separator.
  - *Import Model…* → `open()` (`*.gltf,*.glb,*.obj`) → `client.importModel(path)` → refresh asset list (store) so the new catalog entry appears in the Assets panel + pickers.
  - *Import Texture…* → `open()` (`*.png,*.jpg,*.jpeg`) → `client.importTexture(path)` → refresh assets.
  - *Screenshot Viewport…* → `save()` (`*.png`) → `client.screenshot('viewport', path)` → toast with the saved path (synchronous, `pending:false`). Offer *Screenshot Window…* only as a secondary item with an explicit note it is unavailable in embedded present-only mode (or hide it entirely; default to viewport).
- **Create** menu: one item per `add-entity` preset (Empty / Cube / Model / Point Light / Spot Light / Directional Light / Camera) → `client.addEntity(preset)` → on success select the returned entity (`store.selectedId`) and let `sceneVersion` drive the hierarchy refresh (phase-5). This reuses the phase-5 `CreateMenu` logic; if `CreateMenu` already exists, embed it under this menu rather than duplicating.

5.2. Menus must render in a stacking layer that is NOT occluded by the reparented X11 child window (the loading-overlay design, phase-3 cross-cutting): position dropdowns OUTSIDE the viewport rect (the top menu bar is above the viewport, so its dropdowns drop into non-viewport chrome) or, if a dropdown would overlap the viewport, temporarily lower/unmap the native window per the phase-3 overlay strategy. The top menu bar itself sits above the viewport region, so File/Create dropdowns drop down over the left sidebar / non-viewport area — verify they are not clipped by the native window.

### 6. Store reset after load (`editor/src/state/store.ts`)

6.1. Add a `resetSceneState()` action that clears `entities`, `selectedId` (to null), `componentsBySelected`, bumps a local `loadCounter`, and forces a re-fetch of `listEntities`, `getEnvironment`, `listAssets`, and `renderStats` on the next tick. Call it after a successful `loadProject`/`loadScene` (the engine already cleared its own selection — `control_commands_asset.cpp:173,201` — so the TS side must mirror that, not keep the stale selected id).

6.2. Ensure the reconcile poll's version stamps (`sceneVersion`, `selectionVersion`) are re-read after a load so the hierarchy/inspector diff against the new scene rather than the old. A load is a hard scene-version bump on the engine side (entity create/destroy via phase-2 `sceneVersion`); confirm `loadProject`/`loadScene` bump it so the poll diff fires.

6.3. After import (model/texture), do NOT reset selection — only refresh the asset list slice so the Assets panel + `AssetPicker` combos pick up the new entry (matches C++ catalog-only import, `editor_app.cppm:186-206`).

### 7. Wire-up + layout

7.1. Mount `EnvironmentPanel`, `RenderStatsPanel`, and `MenuBar` into the phase-3 `App.tsx` / phase-9 layout slots (Environment beside the Inspector per C++ docking; Render Stats as a dockable panel; MenuBar at top). Until phase-9's dock layout lands, place them in the existing sidebar/topbar regions.

7.2. No `se` or `docs/` engine-side changes (no new control commands). Optionally add a frontend README note (deferred to phase-10 docs) that the env exposure field is reserved and tonemap exposure is the render-side `set-exposure`.

## Done when

- [ ] `toolbox` build unaffected (no engine changes); `bun run check` passes against the generated `@saffron/protocol` types; `tools/check-control-schema` still green (env/stats/save/screenshot schemas validate live `se` output).
- [ ] EnvironmentPanel shows Sky Mode (Color/Texture/Procedural) and conditionally Clear Color (Color) or a texture `AssetPicker` (Texture); editing any field is reflected **live in the embedded viewport** and round-trips via `get-environment` (re-fetch shows the persisted value).
- [ ] Rotation edits in **degrees** in the UI but the wire carries **radians** (no 57× drift); intensity/ambient/visible/use-sky-for-ambient all persist.
- [ ] Sky-texture `AssetPicker` (and a drag-drop from the Assets panel) assigns via `setEnvironment({ skyTexture })` with the id as a string; assigning a texture changes the sky in Texture mode.
- [ ] RenderStatsPanel shows live counters (`drawCalls/batches/instances/pipelines/blasCount`), `aa`, `exposureEv`, and a client-side poll-rate (clearly labelled, not claimed as engine fps).
- [ ] Toggling AA mode, `clustered`, `shadows`, `ibl`, `ssao`, `contactShadows`, `ssgi`, `ddgi`, `depthPrepass`, and `exposure` works and the readout reflects the echoed state.
- [ ] `rtShadows`/`restir` toggles are **disabled** when `rtSupported === false` (tooltip shown); when supported, toggling works; the typed `"ray tracing not supported"` error path is caught and surfaced (no silent failure, optimistic state reverted).
- [ ] File ▸ Save Project opens a save dialog and writes `project.json` at the chosen path (`{path}` returned); Save/Load Scene work the same.
- [ ] File ▸ Load Project restores scene + catalog; the store is **fully reset** (no stale entities/selection — hierarchy, inspector, assets, environment, stats all refresh from the loaded project); selection is cleared to match the engine.
- [ ] File ▸ Import Model/Texture opens a file dialog, imports into the catalog, and the new asset appears in the Assets panel + pickers (no selection reset, no stale list).
- [ ] File ▸ Screenshot Viewport opens a save dialog, writes the PNG (synchronous, `pending:false`), and the UI does NOT assume a synchronous file when `pending:true` (window target, which is unavailable in present-only, is hidden or clearly disabled).
- [ ] Create menu spawns every `add-entity` preset (Empty/Cube/Model/Point/Spot/Directional Light/Camera) via the typed client; the new entity appears in the hierarchy (driven by `sceneVersion`) and is selected.
- [ ] File/Create dropdowns are not clipped/occluded by the reparented X11 child window.

## Risks / seams

- **Render-stats is an ever-growing flat bag.** New render features keep appending fields (the bag already has 21). Keep `RenderStats` derived from the schema and rely on the phase-2 contract test to catch drift; do NOT hand-maintain the type. The TS panel should tolerate unknown extra fields (do not exhaustively destructure).
- **RT-gated toggles** have two failure surfaces: the `rtSupported` flag (pre-disable) AND the engine's runtime guard (`Err(...)`). Handle both; a device may report support but still reject. Revert optimistic UI on rejection.
- **Load must fully reset state.** The engine clears its own selection on load (`control_commands_asset.cpp:173,201`); the TS store must mirror this or it will show a stale selected entity that no longer exists. Tie the reset to the load call result, and re-stamp `sceneVersion`/`selectionVersion` so the poll diff fires.
- **Exposure double-home.** `SceneEnvironment.exposure` (wire, reserved) vs renderer `set-exposure` (effective). Route the exposure slider to `set-exposure` only; do not write `setEnvironment({ exposure })` expecting a visual change (it is reserved, `scene.cppm:218`, `phase-1` note).
- **fps is not on the wire.** The native viewport paints independently of the webview; any "fps" the panel shows is the UI poll cadence, not the renderer frame rate. Label it honestly to avoid implying a renderer metric the control protocol does not expose.
- **Window screenshots break under present-only** (phase-1). Default to viewport capture; treat `pending:true` (window) as unsupported in the embedded editor rather than waiting on a file that may never be written.
- **Dialog plugin scope.** `tauri-plugin-dialog` + the `dialog:allow-open`/`dialog:allow-save` permissions must be added to `capabilities/default.json` or the dialogs silently no-op; phase-3 only scaffolded the lifecycle/event permissions.
- **Menu dropdown occlusion** is the same X11-paints-on-top constraint as the loading overlay; menus that drop over the viewport must use the phase-3 lower/unmap-or-position-outside strategy. The top menu bar lives above the viewport, so this is usually a non-issue, but verify with a wide Create menu.
- No new control commands here, so no `se`/`docs` engine delta — but the existing `render-stats` text formatter (`tools/se/source/main.cpp:166-174`) prints only a subset; do not mistake it for the wire contract (the full bag is in `-o json`).
