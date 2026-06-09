/// Typed control client. Every engine call goes through the ONE generic Rust
/// passthrough `invoke('control', { cmd, params })`; the Rust layer already turns
/// an engine `ok:false` into a rejected promise, so `call` only narrows the
/// resolve type. Window-handle lifecycle commands (start/attach/resize/quit/alive)
/// have dedicated Rust commands and get thin wrappers.
///
/// Ids are `string` end-to-end (engine Uuids are u64 and can exceed 2^53). NEVER
/// `Number()` an id.
import { invoke } from "@tauri-apps/api/core";
import type {
  ActiveAlarmsDto,
  AssetList,
  AssetUsagesResult,
  CaptureStartParams,
  CaptureStartResult,
  CaptureStatusResult,
  CaptureStopResult,
  ComponentBody,
  CommandParamsMap,
  CommandResultMap,
  CreateScriptResult,
  DrainAlarmsResult,
  DrainScriptErrorsResult,
  EditorCamera,
  GetScriptSchemaResult,
  SetScriptOverrideResult,
  EntityList,
  EntityRef,
  Environment,
  FrameHistoryDto,
  GizmoState,
  InspectResult,
  Material,
  PerfConfigDto,
  PlayStateResult,
  ProfilerModeResult,
  RenderPassTimingsDto,
  RenderStats,
  Selection,
  SetPerfConfigParams,
  Thumbnail,
  Transform,
  Vec3,
  ProjectInfo,
} from "../protocol";

/// The GPU profiler depth (off keeps the present-only host at baseline cost).
export type ProfilerMode = ProfilerModeResult["mode"];

/// The viewport panel's logical CSS rect plus the window scale factor. Rust positions
/// the wayland subsurface in logical coordinates and tells the engine to render at
/// `width*scale × height*scale` device pixels.
export interface ViewportBounds {
  x: number;
  y: number;
  width: number;
  height: number;
  scale: number;
}

/// Result of a viewport `pick`: the engine tests billboards then mesh AABBs and
/// reports what (if anything) the ray hit. `id`/`name` are present only on a hit.
export interface PickResult {
  hit: boolean;
  kind?: "mesh" | "billboard";
  id?: string;
  name?: string;
}

export interface RecentProject {
  path: string;
  name: string;
  displayName: string;
  lastOpenedAt: string;
}

export interface RecentProjects {
  projects: RecentProject[];
}

export interface AppDataInfo {
  appDataDir: string;
  userdataDir: string;
  envProject: boolean;
  autoEmptyProject: boolean;
}

/// Editor-wide settings persisted in appdata/settings.json. `keyBindings` holds only
/// the user's overrides (command id → key-string); defaults live in lib/keybindings.
export interface EditorSettings {
  keyBindings: Record<string, string>;
}

/// One pointer phase forwarded to the native gizmo. `hover` tracks the handle
/// under the cursor; `begin`/`drag`/`end` bracket a manipulation.
export type GizmoPointerPhase = "hover" | "begin" | "drag" | "end";

/// A spawn preset for `add-entity` (matches the engine's Create menu items).
export type EntityPreset = NonNullable<CommandParamsMap["add-entity"]["preset"]>;

type CommandName = keyof CommandParamsMap & keyof CommandResultMap;
type EmptyCommandName = {
  [C in CommandName]: keyof CommandParamsMap[C] extends never ? C : never;
}[CommandName];

function call<C extends EmptyCommandName>(cmd: C): Promise<CommandResultMap[C]>;
function call<C extends CommandName>(
  cmd: C,
  params: CommandParamsMap[C],
): Promise<CommandResultMap[C]>;
/// Typed passthrough: resolves with the command's declared result type.
async function call<C extends CommandName>(
  cmd: C,
  params?: CommandParamsMap[C],
): Promise<CommandResultMap[C]> {
  return invoke<CommandResultMap[C]>("control", { cmd, params: params ?? {} });
}

export const client = {
  // --- scene / entities ---
  listEntities(): Promise<EntityList> {
    return call("list-entities");
  },
  inspect(id: string): Promise<InspectResult> {
    return call("inspect", { entity: id });
  },
  getSelection(): Promise<Selection> {
    return call("get-selection");
  },
  selectEntity(id: string): Promise<unknown> {
    return call("select", { entity: id });
  },
  destroyEntity(id: string): Promise<unknown> {
    return call("destroy-entity", { entity: id });
  },
  /// Reparent (engine-authoritative, cycle-guarded); null detaches to root. Ids stay
  /// strings end-to-end (u64 precision).
  setParent(id: string, parentId: string | null): Promise<EntityRef> {
    return call("set-parent", { entity: id, parent: parentId ?? "0" });
  },
  deselect(): Promise<unknown> {
    return call("deselect");
  },
  addEntity(preset: EntityPreset): Promise<EntityRef> {
    return call("add-entity", { preset });
  },
  /// Create an empty named entity; the engine returns its ref (no auto-select).
  createEntity(name: string): Promise<EntityRef> {
    return call("create-entity", { name });
  },
  copyEntity(id: string): Promise<EntityRef> {
    return call("copy-entity", { entity: id });
  },
  /// Set the entity's Name component; echoes the updated ref.
  renameEntity(id: string, name: string): Promise<EntityRef> {
    return call("rename-entity", { entity: id, name });
  },

  // --- transform / components ---
  /// `smooth` makes the engine animate the fields toward the values (~25ms) instead
  /// of snapping — sent only mid-drag; the release send omits it.
  setTransform(id: string, partial: Partial<Transform>, smooth?: boolean): Promise<unknown> {
    return call("set-transform", { entity: id, ...partial, ...(smooth ? { smooth: true } : {}) });
  },
  /// Material merge helper (server-side merge over the current material, like
  /// set-transform). `albedoTexture` is a string uuid the engine coerces to u64.
  /// `smooth` makes the engine animate numeric fields toward the values (~25ms)
  /// instead of snapping — sent only mid-drag; the release send omits it.
  /// `slot` targets one entry of the entity's MaterialSetComponent instead of its
  /// MaterialComponent (direct write; numeric fields are not animated per slot).
  setMaterial(
    id: string,
    partial: Partial<Material>,
    smooth?: boolean,
    slot?: number,
  ): Promise<unknown> {
    return call("set-material", {
      entity: id,
      ...partial,
      ...(slot === undefined ? {} : { slot }),
      ...(smooth ? { smooth: true } : {}),
    });
  },
  addComponent(id: string, component: string): Promise<unknown> {
    return call("add-component", { entity: id, component });
  },
  removeComponent(id: string, component: string): Promise<unknown> {
    return call("remove-component", { entity: id, component });
  },
  setComponent(id: string, component: string, body: ComponentBody): Promise<unknown> {
    return call("set-component", { entity: id, component, json: body });
  },
  setComponentField(
    id: string,
    component: string,
    field: string,
    value: unknown,
  ): Promise<unknown> {
    return call("set-component-field", { entity: id, component, field, value });
  },

  // --- picking ---
  pick(u: number, v: number): Promise<PickResult> {
    return call("pick", { u, v });
  },

  // --- play mode ---
  /// Enter play mode (from edit) or resume (from paused): the engine duplicates the
  /// scene and cuts to its primary camera. `hasPrimaryCamera:false` → fly-cam fallback.
  play(): Promise<PlayStateResult> {
    return call("play");
  },
  pause(): Promise<PlayStateResult> {
    return call("pause");
  },
  /// Discard the play duplicate and restore the authored scene.
  stop(): Promise<PlayStateResult> {
    return call("stop");
  },
  /// Advance fixed ticks while paused (default 1).
  step(frames?: number): Promise<PlayStateResult> {
    return call("step", frames === undefined ? {} : { frames });
  },
  getPlayState(): Promise<PlayStateResult> {
    return call("get-play-state");
  },

  // --- scripting ---
  /// Drain contained script errors with seq > since (the cursor mirrors drain-alarms).
  drainScriptErrors(since: number): Promise<DrainScriptErrorsResult> {
    return call("drain-script-errors", { since });
  },
  /// A project script's declared fields (path relative to the project src/).
  getScriptSchema(path: string): Promise<GetScriptSchemaResult> {
    return call("get-script-schema", { path });
  },
  /// Create a boilerplate .lua under the project src/; returns the slot path.
  createScript(name: string): Promise<CreateScriptResult> {
    return call("create-script", { name });
  },
  /// Write one per-instance field override onto a Script slot; null clears it.
  setScriptOverride(
    id: string,
    slot: number,
    name: string,
    value: unknown,
  ): Promise<SetScriptOverrideResult> {
    return call("set-script-override", { entity: id, slot, name, value });
  },

  // --- gizmo ---
  getGizmo(): Promise<GizmoState> {
    return call("get-gizmo");
  },
  setGizmo(state: Partial<GizmoState>): Promise<GizmoState> {
    return call("set-gizmo", state);
  },
  /// Forward one pointer phase to the engine's native gizmo. `x`/`y` are NDC in
  /// [-1, 1] (same `u*2-1` mapping `pick` uses).
  gizmoPointer(phase: GizmoPointerPhase, x: number, y: number): Promise<unknown> {
    return call("gizmo-pointer", { phase, x, y });
  },

  // --- editor camera ---
  /// Aim the editor camera at the entity (the F shortcut / hierarchy Focus).
  focus(id: string): Promise<EntityRef> {
    return call("focus", { entity: id });
  },
  getCamera(): Promise<EditorCamera> {
    return call("get-camera");
  },
  setCamera(camera: Partial<EditorCamera>): Promise<EditorCamera> {
    return call("set-camera", camera);
  },

  // --- assets ---
  listAssets(): Promise<AssetList> {
    return call("list-assets");
  },
  getThumbnail(id: string, size?: number): Promise<Thumbnail> {
    return call("get-thumbnail", size === undefined ? { asset: id } : { asset: id, size });
  },
  /// 512-px preview for the View modal (same readback path as get-thumbnail).
  viewAsset(id: string, size?: number): Promise<Thumbnail> {
    return call("view-asset", size === undefined ? { asset: id } : { asset: id, size });
  },
  /// Rename a catalog entry. Returns the new {id, name}.
  renameAsset(id: string, newName: string): Promise<{ id: string; name: string }> {
    return call("rename-asset", { asset: id, name: newName });
  },
  createAssetFolder(folder: string): Promise<AssetList> {
    return call("create-asset-folder", { folder });
  },
  renameAssetFolder(folder: string, name: string): Promise<AssetList> {
    return call("rename-asset-folder", { folder, name });
  },
  deleteAssetFolder(folder: string): Promise<AssetList> {
    return call("delete-asset-folder", { folder });
  },
  moveAsset(
    id: string,
    folder: string | null,
  ): Promise<{ id: string; name: string; folder?: string }> {
    const params: CommandParamsMap["move-asset"] = { asset: id };
    if (folder) {
      params.folder = folder;
    }
    return call("move-asset", params);
  },
  assetUsages(id: string): Promise<AssetUsagesResult> {
    return call("asset-usages", { asset: id });
  },
  /// On-disk metadata for one asset: size, vertex/triangle counts (meshes), and the
  /// file's modified time. Backs the assets-panel detail view.
  probeAsset(id: string): Promise<CommandResultMap["probe-asset"]> {
    return call("probe-asset", { asset: id });
  },
  deleteAsset(id: string): Promise<CommandResultMap["delete-asset"]> {
    return call("delete-asset", { asset: id });
  },
  /// Assign a mesh or albedo texture to an entity slot (adds the component if
  /// missing). The dedicated, minimal write for Mesh.mesh / Material.albedoTexture.
  assignAsset(
    entity: string,
    slot: "mesh" | "albedo" | "metallic-roughness",
    asset: string,
  ): Promise<unknown> {
    return call("assign-asset", { entity, slot, asset });
  },
  /// Import a model from a filesystem path; the engine spawns + selects an entity
  /// and returns its ref plus the created mesh/albedo asset ids.
  importModel(path: string): Promise<EntityRef & { mesh: string; albedoTexture: string }> {
    return call("import-model", { path });
  },
  /// Import a texture from a filesystem path into the catalog (no spawn).
  importTexture(path: string): Promise<{ texture: string }> {
    return call("import-texture", { path });
  },

  // --- projects ---
  getProject(): Promise<ProjectInfo> {
    return call("get-project");
  },
  newProject(name: string, displayName: string, root?: string): Promise<ProjectInfo> {
    const params: CommandParamsMap["new-project"] = {
      name,
      displayName,
    };
    if (root !== undefined && root !== "") {
      params.root = root;
    }
    return call("new-project", params);
  },
  openProject(path: string): Promise<ProjectInfo> {
    return call("open-project", { path });
  },
  appDataInfo(): Promise<AppDataInfo> {
    return invoke<AppDataInfo>("app_data_info");
  },
  listRecentProjects(): Promise<RecentProjects> {
    return invoke<RecentProjects>("list_recent_projects");
  },
  rememberRecentProject(project: RecentProject): Promise<RecentProjects> {
    return invoke<RecentProjects>("remember_recent_project", { project });
  },
  loadEditorSettings(): Promise<EditorSettings> {
    return invoke<EditorSettings>("load_editor_settings");
  },
  saveEditorSettings(settings: EditorSettings): Promise<void> {
    return invoke<void>("save_editor_settings", { settings });
  },

  // --- stats / environment ---
  renderStats(): Promise<RenderStats> {
    return call("render-stats");
  },

  // --- performance telemetry (phases 1-3) ---
  /// Set the GPU profiler depth. `timestamps` enables per-pass GPU timing + the VMA
  /// budget read; `off` returns to baseline cost. Reports device capability.
  setProfilerMode(mode: ProfilerMode): Promise<ProfilerModeResult> {
    return call("profiler.set-mode", { mode });
  },
  /// Last frame's per-pass GPU times (needs the profiler in timestamps mode).
  passTimings(): Promise<RenderPassTimingsDto> {
    return call("pass-timings");
  },
  /// Arm a bounded profiler capture (single frame by default). Forces timestamps mode +
  /// sub-scopes for the duration; returns the capture id + ack.
  captureStart(params: CaptureStartParams): Promise<CaptureStartResult> {
    return call("profiler.capture-start", params);
  },
  /// Non-destructive capture progress — poll while recording to drive the live counter and
  /// detect readiness without draining the capture.
  captureStatus(): Promise<CaptureStatusResult> {
    return call("profiler.capture-status");
  },
  /// Finish + return the armed capture. A single capture comes back inline (`capture` +
  /// `chromeTrace`); a multi-frame one is written to `path`. `ready` is false when none armed.
  captureStop(): Promise<CaptureStopResult> {
    return call("profiler.capture-stop");
  },
  /// Frame-time percentiles + stutter count, optionally with the recent raw samples
  /// (the live-graph source). Always recorded, independent of the profiler.
  frameHistory(samples?: number): Promise<FrameHistoryDto> {
    return call("frame-history", samples === undefined ? {} : { samples });
  },
  /// The shared budget / green-amber-red threshold config.
  getPerfConfig(): Promise<PerfConfigDto> {
    return call("get-perf-config");
  },
  setPerfConfig(params: SetPerfConfigParams): Promise<PerfConfigDto> {
    return call("set-perf-config", params);
  },
  /// Drain perf-alarm events with seq > since (non-blocking) plus the cursor metadata.
  drainAlarms(since: number): Promise<DrainAlarmsResult> {
    return call("drain-alarms", { since });
  },
  /// The currently firing perf alarms (the badge + per-pass highlight source).
  listActiveAlarms(): Promise<ActiveAlarmsDto> {
    return call("list-active-alarms");
  },
  getEnvironment(): Promise<Environment> {
    return call("get-environment");
  },
  setEnvironment(env: Partial<Environment>): Promise<Environment> {
    return call("set-environment", env);
  },
  /// Merge atmosphere fields over the current environment's `atmosphere` block; the
  /// engine re-bakes the LUT chain next frame. Returns the full updated environment.
  setAtmosphere(atmosphere: Partial<Environment["atmosphere"]>): Promise<Environment> {
    return call("set-atmosphere", atmosphere);
  },

  // --- render toggles (untyped echo payloads; each returns its own flag) ---
  /// Anti-aliasing mode. Echoes `{ aa }`.
  setAa(mode: RenderStats["aa"]): Promise<{ aa: RenderStats["aa"] }> {
    return call("set-aa", { mode });
  },
  /// Clustered (Forward+) light culling. Echoes `{ clustered }`.
  setClustered(on: boolean): Promise<{ clustered: boolean }> {
    return call("set-clustered", { enabled: on });
  },
  /// Image-based lighting. Echoes `{ ibl }`.
  setIbl(on: boolean): Promise<{ ibl: boolean }> {
    return call("set-ibl", { enabled: on });
  },
  /// Screen-space ambient occlusion. Echoes `{ ssao }`.
  setSsao(on: boolean): Promise<{ ssao: boolean }> {
    return call("set-ssao", { enabled: on });
  },
  /// Contact shadows. Echoes `{ contactShadows }`.
  setContactShadows(on: boolean): Promise<{ contactShadows: boolean }> {
    return call("set-contact-shadows", { enabled: on });
  },
  /// Screen-space global illumination. Echoes `{ ssgi }`.
  setSsgi(on: boolean): Promise<{ ssgi: boolean }> {
    return call("set-ssgi", { enabled: on });
  },
  /// Shadow pass. Echoes `{ shadows }`.
  setShadows(on: boolean): Promise<{ shadows: boolean }> {
    return call("set-shadows", { enabled: on });
  },
  /// Global-illumination mode (`off` | `ddgi`). Echoes `{ ddgi }`.
  setGi(mode: "off" | "ddgi"): Promise<{ ddgi: boolean }> {
    return call("set-gi", { mode });
  },
  /// Depth pre-pass. Echoes `{ depthPrepass }`.
  setDepthPrepass(on: boolean): Promise<{ depthPrepass: boolean }> {
    return call("set-depth-prepass", { enabled: on });
  },
  /// Ray-traced shadows. Rejects with the typed error when ray tracing is
  /// unsupported on the device; echoes `{ rtShadows }` otherwise.
  setRtShadows(on: boolean): Promise<{ rtShadows: boolean }> {
    return call("set-rt-shadows", { enabled: on });
  },
  /// ReSTIR. Rejects with the typed error when ray tracing is unsupported;
  /// echoes `{ restir }` otherwise.
  setRestir(on: boolean): Promise<{ restir: boolean }> {
    return call("set-restir", { enabled: on });
  },
  /// Tonemap exposure in stops (exp2). This is the EFFECTIVE exposure; the env's
  /// `exposure` field is reserved on the wire. Echoes `{ exposureEv }`.
  setExposure(ev: number): Promise<{ exposureEv: number }> {
    return call("set-exposure", { ev });
  },

  // --- file ops ---
  /// Write catalog + scene to `path` (engine default `project.json` when omitted).
  saveProject(path?: string): Promise<ProjectInfo> {
    return call("save-project", path === undefined ? {} : { path });
  },
  /// Restore catalog + scene + GPU assets from `path` (engine default
  /// `project.json`). Clears the engine's selection; the caller resets the store.
  loadProject(path?: string): Promise<ProjectInfo> {
    return call("load-project", path === undefined ? {} : { path });
  },
  /// Close the active project and load it again from its own path (catalog + scene +
  /// GPU assets). Clears the engine's selection; the caller resets the store.
  reloadProject(): Promise<ProjectInfo> {
    return call("reload-project");
  },
  /// Write the scene only to `path` (required).
  saveScene(path: string): Promise<{ path: string }> {
    return call("save-scene", { path });
  },
  /// Load the scene only from `path` (required). Clears the engine's selection.
  loadScene(path: string): Promise<{ path: string }> {
    return call("load-scene", { path });
  },
  /// Capture a PNG. `viewport` is synchronous (`pending:false`); `window` is
  /// deferred and unavailable under embedded present-only mode.
  screenshot(
    target: "viewport" | "window",
    path: string,
  ): Promise<{ target: "viewport" | "window"; path: string; pending: boolean }> {
    return call("screenshot", { target, path });
  },

  viewportNativeInfo(): Promise<CommandResultMap["viewport-native-info"]> {
    return call("viewport-native-info");
  },

  /// Stream editor fly-cam input (pointer-lock look deltas in pixels + move keys).
  flyInput(input: {
    active: boolean;
    lookDx?: number;
    lookDy?: number;
    forward?: boolean;
    back?: boolean;
    left?: boolean;
    right?: boolean;
    up?: boolean;
    down?: boolean;
  }): Promise<{ active: boolean }> {
    return call("fly-input", input);
  },

  // --- engine lifecycle + presenter (dedicated Rust commands, NOT the passthrough) ---
  startEngine(): Promise<void> {
    return invoke<void>("start_engine");
  },
  /// Route the viewport panel's rect to the subsurface presenter. `resizeEngine` also
  /// commits the device-pixel render size to the engine (expensive target recreation) —
  /// send it on settled bounds, not on live drag ticks.
  setViewportBounds(bounds: ViewportBounds, resizeEngine: boolean): Promise<void> {
    return invoke<void>("set_viewport_bounds", { bounds, resizeEngine });
  },
  /// Park/unpark the subsurface (a modal or another tab owns the region).
  setViewportHidden(hidden: boolean): Promise<void> {
    return invoke<void>("set_viewport_hidden", { hidden });
  },
  quitEngine(): Promise<void> {
    return invoke<void>("quit_engine");
  },
  engineAlive(): Promise<boolean> {
    return invoke<boolean>("engine_alive");
  },
};

export type Client = typeof client;
export type { ProjectInfo, Vec3 };
