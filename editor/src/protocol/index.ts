/**
 * GENERATED — do not edit.
 *
 * Produced by editor/scripts/gen-protocol.ts from schemas/control/*.schema.json.
 * Regenerate with `bun run gen:protocol`. Edit the schemas, not this file.
 */

/**
 * Stable u64 id. Emitted as a JSON number on the wire (may exceed 2^53); typed as string in TS (tsType) and string-preserve-parsed to avoid precision loss.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "Uuid".
 */
export type Uuid = string;

export interface Protocol {
  AssetEntry?: AssetEntry;
  AssetList?: AssetList;
  Camera?: Camera;
  Components?: Components;
  DirectionalLight?: DirectionalLight;
  EditorCamera?: EditorCamera;
  EntityList?: EntityList;
  EntityRef?: EntityRef;
  Envelope?: Envelope;
  Environment?: Environment;
  GizmoState?: GizmoState;
  InspectResult?: InspectResult;
  Material?: Material;
  Mesh?: Mesh;
  Name?: Name;
  PointLight?: PointLight;
  ProjectInfo?: ProjectInfo;
  RenderStats?: RenderStats;
  Selection?: Selection;
  SpotLight?: SpotLight;
  Thumbnail?: Thumbnail;
  Transform?: Transform;
  Uuid?: Uuid;
  Vec3?: Vec3;
  Vec4?: Vec4;
}
/**
 * A catalog asset entry: id, display name, kind, and on-disk path.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "AssetEntry".
 */
export interface AssetEntry {
  id: Uuid;
  name: string;
  type: "mesh" | "texture" | "other";
  path: string;
}
/**
 * A list of catalog asset entries.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "AssetList".
 */
export interface AssetList {
  assets: AssetEntry[];
}
/**
 * Camera component. fov is the vertical field of view in DEGREES; near/far are the clip planes; primary marks the active scene camera.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "Camera".
 */
export interface Camera {
  fov: number;
  near: number;
  far: number;
  primary: boolean;
}
/**
 * The component family keyed by component name. An entity carries a subset, so no key is required; each present key maps to its component DTO.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "Components".
 */
export interface Components {
  Name?: Name;
  Transform?: Transform;
  Mesh?: Mesh;
  Camera?: Camera;
  Material?: Material;
  DirectionalLight?: DirectionalLight;
  PointLight?: PointLight;
  SpotLight?: SpotLight;
}
/**
 * Name component (the entity's display name).
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "Name".
 */
export interface Name {
  name: string;
}
/**
 * Transform component. rotation is Euler XYZ in RADIANS on the wire.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "Transform".
 */
export interface Transform {
  translation: Vec3;
  scale: Vec3;
  rotation: Vec3;
}
/**
 * Three-component float vector.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "Vec3".
 */
export interface Vec3 {
  x: number;
  y: number;
  z: number;
}
/**
 * Mesh component (references a mesh asset by id).
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "Mesh".
 */
export interface Mesh {
  mesh: Uuid;
}
/**
 * Material component (PBR base parameters + albedo texture reference).
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "Material".
 */
export interface Material {
  baseColor: Vec4;
  albedoTexture: Uuid;
  metallic: number;
  roughness: number;
  emissive: Vec3;
  emissiveStrength: number;
  unlit: boolean;
}
/**
 * Four-component float vector.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "Vec4".
 */
export interface Vec4 {
  x: number;
  y: number;
  z: number;
  w: number;
}
/**
 * Directional light component (sun-style light with a direction, color, intensity, and ambient term).
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "DirectionalLight".
 */
export interface DirectionalLight {
  direction: Vec3;
  color: Vec3;
  intensity: number;
  ambient: number;
}
/**
 * Point light component (omnidirectional light with color, intensity, and range).
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "PointLight".
 */
export interface PointLight {
  color: Vec3;
  intensity: number;
  range: number;
}
/**
 * Spot light component. innerAngle/outerAngle are the cone angles in DEGREES.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "SpotLight".
 */
export interface SpotLight {
  direction: Vec3;
  color: Vec3;
  intensity: number;
  range: number;
  innerAngle: number;
  outerAngle: number;
}
/**
 * Editor fly-camera state: world position, yaw/pitch orientation, clip planes, field of view, and movement/look speeds.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "EditorCamera".
 */
export interface EditorCamera {
  position: Vec3;
  yaw: number;
  pitch: number;
  fov: number;
  near: number;
  far: number;
  moveSpeed: number;
  lookSpeed: number;
}
/**
 * A list of scene entities as lightweight references.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "EntityList".
 */
export interface EntityList {
  entities: EntityRef[];
}
/**
 * Lightweight reference to a scene entity: its stable id and display name.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "EntityRef".
 */
export interface EntityRef {
  id: Uuid;
  name: string;
}
/**
 * Outer response envelope for every control-protocol reply. ok signals success; error carries a message on failure; result is the open per-command payload.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "Envelope".
 */
export interface Envelope {
  ok: boolean;
  error?: string;
  result?: {};
}
/**
 * Scene environment: sky source mode plus clear/sky/ambient parameters.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "Environment".
 */
export interface Environment {
  skyMode: "color" | "texture" | "procedural";
  clearColor: Vec3;
  skyTexture: Uuid;
  skyIntensity: number;
  skyRotation: number;
  exposure: number;
  visible: boolean;
  useSkyForAmbient: boolean;
  ambientColor: Vec3;
  ambientIntensity: number;
}
/**
 * In-viewport gizmo state: the active operation and the reference space.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "GizmoState".
 */
export interface GizmoState {
  op: "translate" | "rotate" | "scale";
  space: "world" | "local";
}
/**
 * Result of inspecting an entity: its id, name, and the set of components it carries.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "InspectResult".
 */
export interface InspectResult {
  id: Uuid;
  name: string;
  components: Components;
}
/**
 * The active project state: whether a project is loaded plus its root, project file path, stable name, and display name.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "ProjectInfo".
 */
export interface ProjectInfo {
  loaded: boolean;
  root: string;
  path: string;
  name: string;
  displayName: string;
}
/**
 * Per-frame renderer statistics plus the active feature toggles.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "RenderStats".
 */
export interface RenderStats {
  drawCalls: number;
  batches: number;
  instances: number;
  blasCount: number;
  pipelines: number;
  clustered: boolean;
  depthPrepass: boolean;
  shadows: boolean;
  ibl: boolean;
  ssao: boolean;
  contactShadows: boolean;
  ssgi: boolean;
  ddgi: boolean;
  rtSupported: boolean;
  rtShadows: boolean;
  restir: boolean;
  hdr: boolean;
  exposureEv: number;
  aa: "off" | "fxaa" | "taa" | "msaa2" | "msaa4" | "msaa8";
}
/**
 * Current editor selection: the selected entity (or null) plus the selection and scene version counters.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "Selection".
 */
export interface Selection {
  entity: EntityRef | null;
  selectionVersion: number;
  sceneVersion: number;
}
/**
 * A rendered asset thumbnail: the asset id plus a base64-encoded PNG of the given pixel dimensions.
 *
 * This interface was referenced by `Protocol`'s JSON-Schema
 * via the `definition` "Thumbnail".
 */
export interface Thumbnail {
  id: Uuid;
  format: "png";
  width: number;
  height: number;
  base64: string;
}

/**
 * Maps each typed control command to the schema title of its result payload.
 * Hand-maintained alongside the engine's control surface (gen-protocol appends it).
 */
export interface CommandResultMap {
  "add-entity": EntityRef;
  "copy-entity": EntityRef;
  "create-entity": EntityRef;
  "get-selection": Selection;
  inspect: InspectResult;
  "list-entities": EntityList;
  "list-assets": AssetList;
  "get-gizmo": GizmoState;
  "set-gizmo": GizmoState;
  "get-camera": EditorCamera;
  "set-camera": EditorCamera;
  "render-stats": RenderStats;
  "get-environment": Environment;
  "set-environment": Environment;
  "get-thumbnail": Thumbnail;
  "view-asset": Thumbnail;
  "get-project": ProjectInfo;
  "new-project": ProjectInfo;
  "open-project": ProjectInfo;
  "save-project": ProjectInfo;
  "load-project": ProjectInfo;
}
