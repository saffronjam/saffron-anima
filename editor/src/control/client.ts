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

/// One pointer phase forwarded to the native gizmo. `hover` tracks the handle
/// under the cursor; `begin`/`drag`/`end` bracket a manipulation.
export type GizmoPointerPhase = "hover" | "begin" | "drag" | "end";

/// A spawn preset for `add-entity` (matches the engine's Create menu items).
export type EntityPreset =
  | "empty"
  | "cube"
  | "point-light"
  | "spot-light"
  | "directional-light"
  | "camera";

/// Typed passthrough: resolves with the command's declared result type.
async function call<C extends keyof CommandResultMap>(
  cmd: C,
  params?: object,
): Promise<CommandResultMap[C]> {
  return invoke<CommandResultMap[C]>("control", { cmd, params: params ?? {} });
}

/// Untyped passthrough for commands not in `CommandResultMap` (mutations that
/// return an empty/echo payload). Rejects on engine `ok:false`.
async function callRaw(cmd: string, params?: object): Promise<unknown> {
  return invoke<unknown>("control", { cmd, params: params ?? {} });
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
    return callRaw("select", { entity: id });
  },
  destroyEntity(id: string): Promise<unknown> {
    return callRaw("destroy-entity", { entity: id });
  },
  deselect(): Promise<unknown> {
    return callRaw("deselect");
  },
  addEntity(preset: EntityPreset): Promise<EntityRef> {
    return call("add-entity", { preset });
  },
  copyEntity(id: string): Promise<EntityRef> {
    return call("copy-entity", { entity: id });
  },

  // --- transform / components ---
  setTransform(id: string, partial: Partial<Transform>): Promise<unknown> {
    return callRaw("set-transform", { entity: id, ...partial });
  },
  /// Material merge helper (server-side merge over the current material, like
  /// set-transform). `albedoTexture` is a string uuid the engine coerces to u64.
  setMaterial(id: string, partial: Partial<Material>): Promise<unknown> {
    return callRaw("set-material", { entity: id, ...partial });
  },
  addComponent(id: string, component: string): Promise<unknown> {
    return callRaw("add-component", { entity: id, component });
  },
  removeComponent(id: string, component: string): Promise<unknown> {
    return callRaw("remove-component", { entity: id, component });
  },
  setComponent(id: string, component: string, body: object): Promise<unknown> {
    return callRaw("set-component", { entity: id, component, json: body });
  },
  setComponentField(
    id: string,
    component: string,
    field: string,
    value: unknown,
  ): Promise<unknown> {
    return callRaw("set-component-field", { entity: id, component, field, value });
  },

  // --- picking ---
  pick(u: number, v: number): Promise<PickResult> {
    return callRaw("pick", { u, v }) as Promise<PickResult>;
  },

  // --- gizmo ---
  getGizmo(): Promise<GizmoState> {
    return call("get-gizmo");
  },
  setGizmo(state: Partial<GizmoState>): Promise<GizmoState> {
    return call("set-gizmo", state);
  },
  /// Forward one pointer phase to the engine's native gizmo. `x`/`y` are NDC in
  /// [-1, 1] (same `u*2-1` mapping `pick` uses). No schema — untyped passthrough.
  gizmoPointer(phase: GizmoPointerPhase, x: number, y: number): Promise<unknown> {
    return callRaw("gizmo-pointer", { phase, x, y });
  },

  // --- editor camera ---
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
  /// Rename a catalog entry. The engine accepts {id} (preferred, string uuid) or
  /// {name}; we always send the id. Returns the new {id, name}.
  renameAsset(id: string, newName: string): Promise<{ id: string; name: string }> {
    return callRaw("rename-asset", { id, newName }) as Promise<{ id: string; name: string }>;
  },
  /// Assign a mesh or albedo texture to an entity slot (adds the component if
  /// missing). The dedicated, minimal write for Mesh.mesh / Material.albedoTexture.
  assignAsset(entity: string, slot: "mesh" | "albedo", asset: string): Promise<unknown> {
    return callRaw("assign-asset", { entity, slot, asset });
  },
  /// Import a model from a filesystem path; the engine spawns + selects an entity
  /// and returns its ref plus the created mesh/albedo asset ids.
  importModel(path: string): Promise<EntityRef & { mesh: string; albedoTexture: string }> {
    return callRaw("import-model", { path }) as Promise<
      EntityRef & { mesh: string; albedoTexture: string }
    >;
  },
  /// Import a texture from a filesystem path into the catalog (no spawn).
  importTexture(path: string): Promise<{ texture: string }> {
    return callRaw("import-texture", { path }) as Promise<{ texture: string }>;
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

  // --- render toggles (untyped echo payloads; each returns its own flag) ---
  /// Anti-aliasing mode. Echoes `{ aa }`.
  setAa(mode: RenderStats["aa"]): Promise<{ aa: RenderStats["aa"] }> {
    return callRaw("set-aa", { mode }) as Promise<{ aa: RenderStats["aa"] }>;
  },
  /// Clustered (Forward+) light culling. Echoes `{ clustered }`.
  setClustered(on: boolean): Promise<{ clustered: boolean }> {
    return callRaw("set-clustered", { enabled: on ? 1 : 0 }) as Promise<{ clustered: boolean }>;
  },
  /// Image-based lighting. Echoes `{ ibl }`.
  setIbl(on: boolean): Promise<{ ibl: boolean }> {
    return callRaw("set-ibl", { enabled: on ? 1 : 0 }) as Promise<{ ibl: boolean }>;
  },
  /// Screen-space ambient occlusion. Echoes `{ ssao }`.
  setSsao(on: boolean): Promise<{ ssao: boolean }> {
    return callRaw("set-ssao", { enabled: on ? 1 : 0 }) as Promise<{ ssao: boolean }>;
  },
  /// Contact shadows. Echoes `{ contactShadows }`.
  setContactShadows(on: boolean): Promise<{ contactShadows: boolean }> {
    return callRaw("set-contact-shadows", { enabled: on ? 1 : 0 }) as Promise<{
      contactShadows: boolean;
    }>;
  },
  /// Screen-space global illumination. Echoes `{ ssgi }`.
  setSsgi(on: boolean): Promise<{ ssgi: boolean }> {
    return callRaw("set-ssgi", { enabled: on ? 1 : 0 }) as Promise<{ ssgi: boolean }>;
  },
  /// Shadow pass. Echoes `{ shadows }`.
  setShadows(on: boolean): Promise<{ shadows: boolean }> {
    return callRaw("set-shadows", { enabled: on ? 1 : 0 }) as Promise<{ shadows: boolean }>;
  },
  /// Global-illumination mode (`off` | `ddgi`). Echoes `{ ddgi }`.
  setGi(mode: "off" | "ddgi"): Promise<{ ddgi: boolean }> {
    return callRaw("set-gi", { mode }) as Promise<{ ddgi: boolean }>;
  },
  /// Depth pre-pass. Echoes `{ depthPrepass }`.
  setDepthPrepass(on: boolean): Promise<{ depthPrepass: boolean }> {
    return callRaw("set-depth-prepass", { enabled: on ? 1 : 0 }) as Promise<{
      depthPrepass: boolean;
    }>;
  },
  /// Ray-traced shadows. Rejects with the typed error when ray tracing is
  /// unsupported on the device; echoes `{ rtShadows }` otherwise.
  setRtShadows(on: boolean): Promise<{ rtShadows: boolean }> {
    return callRaw("set-rt-shadows", { enabled: on ? 1 : 0 }) as Promise<{ rtShadows: boolean }>;
  },
  /// ReSTIR. Rejects with the typed error when ray tracing is unsupported;
  /// echoes `{ restir }` otherwise.
  setRestir(on: boolean): Promise<{ restir: boolean }> {
    return callRaw("set-restir", { enabled: on ? 1 : 0 }) as Promise<{ restir: boolean }>;
  },
  /// Tonemap exposure in stops (exp2). This is the EFFECTIVE exposure; the env's
  /// `exposure` field is reserved on the wire. Echoes `{ exposureEv }`.
  setExposure(ev: number): Promise<{ exposureEv: number }> {
    return callRaw("set-exposure", { ev }) as Promise<{ exposureEv: number }>;
  },

  // --- file ops (untyped; each returns { path }) ---
  /// Write catalog + scene to `path` (engine default `project.json` when omitted).
  saveProject(path?: string): Promise<{ path: string }> {
    return callRaw("save-project", path === undefined ? {} : { path }) as Promise<{
      path: string;
    }>;
  },
  /// Restore catalog + scene + GPU assets from `path` (engine default
  /// `project.json`). Clears the engine's selection; the caller resets the store.
  loadProject(path?: string): Promise<{ path: string }> {
    return callRaw("load-project", path === undefined ? {} : { path }) as Promise<{
      path: string;
    }>;
  },
  /// Write the scene only to `path` (required).
  saveScene(path: string): Promise<{ path: string }> {
    return callRaw("save-scene", { path }) as Promise<{ path: string }>;
  },
  /// Load the scene only from `path` (required). Clears the engine's selection.
  loadScene(path: string): Promise<{ path: string }> {
    return callRaw("load-scene", { path }) as Promise<{ path: string }>;
  },
  /// Capture a PNG. `viewport` is synchronous (`pending:false`); `window` is
  /// deferred and unavailable under embedded present-only mode.
  screenshot(
    target: "viewport" | "window",
    path: string,
  ): Promise<{ target: "viewport" | "window"; path: string; pending: boolean }> {
    return callRaw("screenshot", { target, path }) as Promise<{
      target: "viewport" | "window";
      path: string;
      pending: boolean;
    }>;
  },

  // --- escape hatch for not-yet-typed commands ---
  raw(cmd: string, params?: object): Promise<unknown> {
    return callRaw(cmd, params);
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
export type { Vec3 };
