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
  AssetList,
  AssetUsagesResult,
  ComponentBody,
  CommandParamsMap,
  CommandResultMap,
  EditorCamera,
  EntityList,
  EntityRef,
  Environment,
  GizmoState,
  InspectResult,
  Material,
  RenderStats,
  Selection,
  Thumbnail,
  Transform,
  Vec3,
  ProjectInfo,
} from "../protocol";

/// Device-pixel bounds the native viewport window is reparented over.
export interface ViewportBounds {
  x: number;
  y: number;
  width: number;
  height: number;
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
  setTransform(id: string, partial: Partial<Transform>): Promise<unknown> {
    return call("set-transform", { entity: id, ...partial });
  },
  /// Material merge helper (server-side merge over the current material, like
  /// set-transform). `albedoTexture` is a string uuid the engine coerces to u64.
  setMaterial(id: string, partial: Partial<Material>): Promise<unknown> {
    return call("set-material", { entity: id, ...partial });
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
  assignAsset(entity: string, slot: "mesh" | "albedo", asset: string): Promise<unknown> {
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

  // --- stats / environment ---
  renderStats(): Promise<RenderStats> {
    return call("render-stats");
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

  // --- window-handle lifecycle (dedicated Rust commands, NOT the passthrough) ---
  startEngine(): Promise<void> {
    return invoke<void>("start_engine");
  },
  attachViewport(bounds: ViewportBounds): Promise<void> {
    return invoke<void>("attach_native_viewport", { bounds });
  },
  resizeViewport(bounds: ViewportBounds): Promise<void> {
    return invoke<void>("resize_native_viewport", { bounds });
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
