/**
 * GENERATED - do not edit.
 *
 * Produced by tools/gen-control-dto/gen.ts from control_dto.cppm.
 */

export type WireUuid = string;

export interface Name {
  name: string;
}

export interface Transform {
  translation: Vec3;
  scale: Vec3;
  rotation: Vec3;
}

export interface Mesh {
  mesh: WireUuid;
}

export interface Camera {
  fov: number;
  near: number;
  far: number;
  primary: boolean;
}

export interface Material {
  baseColor: Vec4;
  albedoTexture: WireUuid;
  metallic: number;
  roughness: number;
  emissive: Vec3;
  emissiveStrength: number;
  unlit: boolean;
}

export interface DirectionalLight {
  direction: Vec3;
  color: Vec3;
  intensity: number;
  ambient: number;
}

export interface PointLight {
  color: Vec3;
  intensity: number;
  range: number;
}

export interface SpotLight {
  direction: Vec3;
  color: Vec3;
  intensity: number;
  range: number;
  innerAngle: number;
  outerAngle: number;
}

export interface ReflectionProbe {
  influenceRadius: number;
  intensity: number;
  boxProjection: boolean;
  boxExtent: Vec3;
}

export interface Relationship {
  parent: WireUuid;
}

export interface AtmosphereSettingsDto {
  enabled: boolean;
  planetRadius: number;
  atmosphereHeight: number;
  rayleighScattering: Vec3;
  rayleighScaleHeight: number;
  mieScattering: number;
  mieScaleHeight: number;
  mieAnisotropy: number;
  ozoneAbsorption: Vec3;
  sunDiskAngularRadius: number;
  sunDiskIntensity: number;
}

export interface Components {
  Name?: Name;
  Transform?: Transform;
  Mesh?: Mesh;
  Camera?: Camera;
  Material?: Material;
  DirectionalLight?: DirectionalLight;
  PointLight?: PointLight;
  SpotLight?: SpotLight;
  ReflectionProbe?: ReflectionProbe;
  Relationship?: Relationship;
}

export type ComponentBody =
  | Name
  | Transform
  | Mesh
  | Camera
  | Material
  | DirectionalLight
  | PointLight
  | SpotLight
  | ReflectionProbe
  | Relationship
  | Record<string, unknown>;

export interface EntityRef {
  id: WireUuid;
  name: string;
}

export interface PingParams {

}

export interface PingResult {
  pong: boolean;
  engine: string;
  version: string;
  pid: number;
}

export interface EmptyParams {

}

export interface RenderStatsDto {
  drawCalls: number;
  batches: number;
  instances: number;
  frameMs: number;
  fps: number;
  gpuMs: number;
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
  blasCount: number;
  pipelines: number;
  hdr: boolean;
  exposureEv: number;
  aa: "off" | "fxaa" | "taa" | "msaa2" | "msaa4" | "msaa8";
}

export interface SetAaParams {
  mode?: "off" | "fxaa" | "taa" | "msaa2" | "msaa4" | "msaa8";
}

export interface SetAaResult {
  aa: "off" | "fxaa" | "taa" | "msaa2" | "msaa4" | "msaa8";
}

export interface ToggleParams {
  enabled?: boolean;
}

export interface SetClusteredResult {
  clustered: boolean;
}

export interface SetIblResult {
  ibl: boolean;
}

export interface SetSsaoResult {
  ssao: boolean;
}

export interface SetContactShadowsResult {
  contactShadows: boolean;
}

export interface SetSsgiResult {
  ssgi: boolean;
}

export interface SetRtShadowsResult {
  rtShadows: boolean;
}

export interface SetRestirResult {
  restir: boolean;
}

export interface SetGiParams {
  mode: "off" | "ddgi";
}

export interface SetGiResult {
  ddgi: boolean;
}

export interface SetShadowsResult {
  shadows: boolean;
}

export interface SetDepthPrepassResult {
  depthPrepass: boolean;
}

export interface ViewportNativeInfoResult {
  platform: string;
  transport: string;
  status: string;
  controlSocket: string;
  width: number;
  height: number;
  message: string;
}

export interface AttachNativeViewportParams {
  parentXid: WireUuid;
  x?: number;
  y?: number;
  width?: number;
  height?: number;
}

export interface AttachNativeViewportResult {
  attached: boolean;
  transport: string;
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface ResizeNativeViewportParams {
  x?: number;
  y?: number;
  width?: number;
  height?: number;
}

export interface ResizeNativeViewportResult {
  resized: boolean;
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface EntityList {
  entities: EntityRef[];
}

export interface ComponentList {
  components: string[];
}

export interface CreateEntityParams {
  name: string;
}

export interface EntityParams {
  entity: WireUuid | string | number;
}

export interface DestroyEntityResult {
  destroyed: WireUuid;
}

export interface ComponentParams {
  entity: WireUuid | string | number;
  component: string;
}

export interface AddComponentResult {
  added: string;
}

export interface RemoveComponentResult {
  removed: string;
}

export interface SetComponentParams {
  entity: WireUuid | string | number;
  component: string;
  json: ComponentBody;
}

export interface SetComponentResult {
  set: string;
}

export interface SetTransformParams {
  entity: WireUuid | string | number;
  translation?: Vec3;
  rotation?: Vec3;
  scale?: Vec3;
}

export interface Vec3 {
  x: number;
  y: number;
  z: number;
}

export interface SetMaterialParams {
  entity: WireUuid | string | number;
  baseColor?: Vec4;
  albedoTexture?: WireUuid;
  metallic?: number;
  roughness?: number;
  emissive?: Vec3;
  emissiveStrength?: number;
  unlit?: boolean;
}

export interface Vec4 {
  x: number;
  y: number;
  z: number;
  w: number;
}

export interface SetLightParams {
  entity?: WireUuid | string | number;
  direction?: Vec3;
  color?: Vec3;
  intensity?: number;
  ambient?: number;
}

export interface PickParams {
  u?: number;
  v?: number;
}

export interface PickResult {
  hit: boolean;
  id?: WireUuid;
  name?: string;
  kind?: "billboard" | "mesh";
}

export interface InspectResult {
  id: WireUuid;
  name: string;
  components: Components;
}

export interface EnvironmentDto {
  skyMode: "color" | "texture" | "procedural";
  clearColor: Vec3;
  skyTexture: WireUuid;
  skyIntensity: number;
  skyRotation: number;
  exposure: number;
  visible: boolean;
  useSkyForAmbient: boolean;
  ambientColor: Vec3;
  ambientIntensity: number;
  atmosphere: AtmosphereSettingsDto;
}

export interface SetEnvironmentParams {
  json?: unknown;
  skyMode?: string;
  clearColor?: Vec3;
  skyTexture?: WireUuid;
  skyIntensity?: number;
  skyRotation?: number;
  exposure?: number;
  visible?: boolean;
  useSkyForAmbient?: boolean;
  ambientColor?: Vec3;
  ambientIntensity?: number;
}

export interface SetAtmosphereParams {
  json?: unknown;
  enabled?: boolean;
  planetRadius?: number;
  atmosphereHeight?: number;
  rayleighScattering?: Vec3;
  rayleighScaleHeight?: number;
  mieScattering?: number;
  mieScaleHeight?: number;
  mieAnisotropy?: number;
  ozoneAbsorption?: Vec3;
  sunDiskAngularRadius?: number;
  sunDiskIntensity?: number;
}

export interface SelectionResult {
  selectionVersion: number;
  sceneVersion: number;
  entity?: EntityRef;
}

export interface DeselectResult {
  selectionVersion: number;
}

export interface AddEntityParams {
  preset?: "empty" | "cube" | "model" | "point-light" | "spot-light" | "directional-light" | "camera" | "reflection-probe";
}

export interface RenameEntityParams {
  entity: WireUuid | string | number;
  name: string;
}

export interface SetComponentFieldParams {
  entity: WireUuid | string | number;
  component: string;
  field: string;
  value: unknown;
}

export interface SetComponentFieldResult {
  set: string;
  field: string;
}

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

export interface SetCameraParams {
  position?: Vec3;
  yaw?: number;
  pitch?: number;
  fov?: number;
  near?: number;
  far?: number;
  moveSpeed?: number;
  lookSpeed?: number;
}

export interface GizmoState {
  op: "translate" | "rotate" | "scale";
  space: "world" | "local";
}

export interface SetGizmoParams {
  op?: "translate" | "rotate" | "scale";
  space?: "world" | "local";
}

export interface GizmoPointerParams {
  phase?: "hover" | "begin" | "drag" | "end";
  x?: number;
  y?: number;
}

export interface GizmoPointerResult {
  hovered: string;
  dragging: boolean;
}

export interface SetProbesParams {
  enabled?: boolean;
}

export interface SetProbesResult {
  probes: boolean;
}

export interface RecaptureProbesResult {
  marked: number;
}

export interface ListProbesResult {
  enabled: boolean;
  count: number;
  probes: ProbeRef[];
}

export interface ProbeRef {
  slot: number;
  entity: WireUuid;
  origin: Vec3;
  influenceRadius: number;
  intensity: number;
  boxProjection: boolean;
  valid: boolean;
  dirty: boolean;
}

export interface SetExposureParams {
  ev: number;
}

export interface SetExposureResult {
  exposureEv: number;
}

export interface ProjectInfoDto {
  loaded: boolean;
  root: string;
  path: string;
  name: string;
  displayName: string;
}

export interface NewProjectParams {
  name?: string;
  displayName?: string;
  root?: string;
}

export interface PathParams {
  path: string;
}

export interface ImportModelResult {
  id: WireUuid;
  name: string;
  mesh: WireUuid;
  albedoTexture: WireUuid;
}

export interface ImportTextureResult {
  texture: WireUuid;
}

export interface AssetList {
  assets: AssetEntryDto[];
  folders: string[];
}

export interface AssetEntryDto {
  id: WireUuid;
  name: string;
  type: "mesh" | "texture" | "other";
  path: string;
  folder?: string;
}

export interface RenameAssetParams {
  asset: WireUuid | string | number;
  name: string;
}

export interface AssetRef {
  id: WireUuid;
  name: string;
  folder?: string;
}

export interface CreateAssetFolderParams {
  folder: string;
}

export interface RenameAssetFolderParams {
  folder: string;
  name: string;
}

export interface DeleteAssetFolderParams {
  folder: string;
}

export interface MoveAssetParams {
  asset: WireUuid | string | number;
  folder?: string;
}

export interface AssetUsagesParams {
  asset: WireUuid | string | number;
}

export interface AssetUsagesResult {
  usages: AssetUsageDto[];
}

export interface AssetUsageDto {
  entity?: WireUuid;
  entityName?: string;
  slot: string;
}

export interface DeleteAssetParams {
  asset: WireUuid | string | number;
}

export interface DeleteAssetResult {
  id: WireUuid;
  name: string;
  cleared: AssetUsageDto[];
  fileDeleted: boolean;
}

export interface AssignAssetParams {
  entity: WireUuid | string | number;
  slot: "mesh" | "albedo";
  asset: WireUuid | string | number;
}

export interface AssignAssetResult {
  id: WireUuid;
  name: string;
  slot: "mesh" | "albedo";
}

export interface PathResult {
  path: string;
}

export interface OptionalPathParams {
  path?: string;
}

export interface ScreenshotParams {
  target?: "viewport" | "window";
  path: string;
}

export interface ScreenshotResult {
  target: "viewport" | "window";
  path: string;
  pending: boolean;
}

export interface ThumbnailParams {
  asset: WireUuid | string | number;
  size?: number;
}

export interface ThumbnailResult {
  id: WireUuid;
  format: string;
  width: number;
  height: number;
  base64: string;
}

export interface QuitResult {
  quitting: boolean;
}

export interface CommandParamsMap {
  "ping": PingParams;
  "render-stats": EmptyParams;
  "set-aa": SetAaParams;
  "set-clustered": ToggleParams;
  "set-ibl": ToggleParams;
  "set-ssao": ToggleParams;
  "set-contact-shadows": ToggleParams;
  "set-ssgi": ToggleParams;
  "set-rt-shadows": ToggleParams;
  "set-restir": ToggleParams;
  "set-gi": SetGiParams;
  "set-shadows": ToggleParams;
  "set-depth-prepass": ToggleParams;
  "viewport-native-info": EmptyParams;
  "attach-native-viewport": AttachNativeViewportParams;
  "resize-native-viewport": ResizeNativeViewportParams;
  "list-entities": EmptyParams;
  "list-components": EmptyParams;
  "create-entity": CreateEntityParams;
  "destroy-entity": EntityParams;
  "add-component": ComponentParams;
  "remove-component": ComponentParams;
  "set-component": SetComponentParams;
  "set-transform": SetTransformParams;
  "set-material": SetMaterialParams;
  "set-light": SetLightParams;
  "select": EntityParams;
  "pick": PickParams;
  "inspect": EntityParams;
  "focus": EntityParams;
  "get-environment": EmptyParams;
  "set-environment": SetEnvironmentParams;
  "set-atmosphere": SetAtmosphereParams;
  "get-selection": EmptyParams;
  "deselect": EmptyParams;
  "add-entity": AddEntityParams;
  "copy-entity": EntityParams;
  "rename-entity": RenameEntityParams;
  "set-component-field": SetComponentFieldParams;
  "get-camera": EmptyParams;
  "set-camera": SetCameraParams;
  "get-gizmo": EmptyParams;
  "set-gizmo": SetGizmoParams;
  "gizmo-pointer": GizmoPointerParams;
  "set-probes": SetProbesParams;
  "recapture-probes": EmptyParams;
  "list-probes": EmptyParams;
  "set-exposure": SetExposureParams;
  "get-project": EmptyParams;
  "new-project": NewProjectParams;
  "open-project": PathParams;
  "import-model": PathParams;
  "import-texture": PathParams;
  "list-assets": EmptyParams;
  "rename-asset": RenameAssetParams;
  "create-asset-folder": CreateAssetFolderParams;
  "rename-asset-folder": RenameAssetFolderParams;
  "delete-asset-folder": DeleteAssetFolderParams;
  "move-asset": MoveAssetParams;
  "asset-usages": AssetUsagesParams;
  "delete-asset": DeleteAssetParams;
  "assign-asset": AssignAssetParams;
  "save-scene": PathParams;
  "load-scene": PathParams;
  "save-project": OptionalPathParams;
  "load-project": OptionalPathParams;
  "screenshot": ScreenshotParams;
  "get-thumbnail": ThumbnailParams;
  "view-asset": ThumbnailParams;
  "quit": EmptyParams;
}

export interface CommandResultMap {
  "ping": PingResult;
  "render-stats": RenderStatsDto;
  "set-aa": SetAaResult;
  "set-clustered": SetClusteredResult;
  "set-ibl": SetIblResult;
  "set-ssao": SetSsaoResult;
  "set-contact-shadows": SetContactShadowsResult;
  "set-ssgi": SetSsgiResult;
  "set-rt-shadows": SetRtShadowsResult;
  "set-restir": SetRestirResult;
  "set-gi": SetGiResult;
  "set-shadows": SetShadowsResult;
  "set-depth-prepass": SetDepthPrepassResult;
  "viewport-native-info": ViewportNativeInfoResult;
  "attach-native-viewport": AttachNativeViewportResult;
  "resize-native-viewport": ResizeNativeViewportResult;
  "list-entities": EntityList;
  "list-components": ComponentList;
  "create-entity": EntityRef;
  "destroy-entity": DestroyEntityResult;
  "add-component": AddComponentResult;
  "remove-component": RemoveComponentResult;
  "set-component": SetComponentResult;
  "set-transform": EntityRef;
  "set-material": EntityRef;
  "set-light": EntityRef;
  "select": EntityRef;
  "pick": PickResult;
  "inspect": InspectResult;
  "focus": EntityRef;
  "get-environment": EnvironmentDto;
  "set-environment": EnvironmentDto;
  "set-atmosphere": EnvironmentDto;
  "get-selection": SelectionResult;
  "deselect": DeselectResult;
  "add-entity": EntityRef;
  "copy-entity": EntityRef;
  "rename-entity": EntityRef;
  "set-component-field": SetComponentFieldResult;
  "get-camera": EditorCamera;
  "set-camera": EditorCamera;
  "get-gizmo": GizmoState;
  "set-gizmo": GizmoState;
  "gizmo-pointer": GizmoPointerResult;
  "set-probes": SetProbesResult;
  "recapture-probes": RecaptureProbesResult;
  "list-probes": ListProbesResult;
  "set-exposure": SetExposureResult;
  "get-project": ProjectInfoDto;
  "new-project": ProjectInfoDto;
  "open-project": ProjectInfoDto;
  "import-model": ImportModelResult;
  "import-texture": ImportTextureResult;
  "list-assets": AssetList;
  "rename-asset": AssetRef;
  "create-asset-folder": AssetList;
  "rename-asset-folder": AssetList;
  "delete-asset-folder": AssetList;
  "move-asset": AssetRef;
  "asset-usages": AssetUsagesResult;
  "delete-asset": DeleteAssetResult;
  "assign-asset": AssignAssetResult;
  "save-scene": PathResult;
  "load-scene": PathResult;
  "save-project": ProjectInfoDto;
  "load-project": ProjectInfoDto;
  "screenshot": ScreenshotResult;
  "get-thumbnail": ThumbnailResult;
  "view-asset": ThumbnailResult;
  "quit": QuitResult;
}
