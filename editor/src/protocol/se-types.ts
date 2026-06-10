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
  showModel: boolean;
  showFrustum: boolean;
  frustumMaxDistance: number;
}

export interface Material {
  baseColor: Vec4;
  albedoTexture: WireUuid;
  metallicRoughnessTexture: WireUuid;
  metallic: number;
  roughness: number;
  emissive: Vec3;
  emissiveStrength: number;
  unlit: boolean;
}

export interface MaterialSet {
  slots: Material[];
}

export interface ScriptSlot {
  scriptPath: string;
  overrides: Record<string, unknown>;
}

export interface Script {
  scripts: ScriptSlot[];
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

export interface SkinnedMesh {
  mesh: WireUuid;
  rootBone: WireUuid;
  bones: WireUuid[];
  inverseBind: number[][];
}

export interface Bone {}

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
  MaterialSet?: MaterialSet;
  Script?: Script;
  DirectionalLight?: DirectionalLight;
  PointLight?: PointLight;
  SpotLight?: SpotLight;
  ReflectionProbe?: ReflectionProbe;
  Relationship?: Relationship;
  SkinnedMesh?: SkinnedMesh;
  Bone?: Bone;
}

export type ComponentBody =
  | Name
  | Transform
  | Mesh
  | Camera
  | Material
  | MaterialSet
  | Script
  | DirectionalLight
  | PointLight
  | SpotLight
  | ReflectionProbe
  | Relationship
  | SkinnedMesh
  | Bone
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
  cpuFrameMs: number;
  gpuFrameMs: number;
  cpuWaitMs: number;
  triangles: number;
  descriptorBinds: number;
  commandBuffers: number;
  queueSubmits: number;
  pipelinesCreated: number;
  vramUsageBytes: number;
  vramBudgetBytes: number;
  softwareGpu: boolean;
  profilerMode: "off" | "timestamps" | "pipeline-stats";
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

export interface ProfilerSetModeParams {
  mode?: "off" | "timestamps" | "pipeline-stats";
}

export interface ProfilerModeResult {
  mode: "off" | "timestamps" | "pipeline-stats";
  timestampsSupported: boolean;
  pipelineStatsSupported: boolean;
  softwareGpu: boolean;
}

export interface RenderPassTimingsDto {
  passes: RenderPassTimingDto[];
  gpuTotalMs: number;
  softwareGpu: boolean;
  profilerMode: "off" | "timestamps" | "pipeline-stats";
}

export interface RenderPassTimingDto {
  name: string;
  gpuMs: number;
}

export interface CaptureStartParams {
  mode?: "single" | "frames" | "rolling";
  frames?: number;
  filter?: string;
  includeCpu?: boolean;
  includePipelineStats?: boolean;
}

export interface CaptureStartResult {
  captureId: number;
  ack: boolean;
}

export interface CaptureStopResult {
  ready: boolean;
  mode: "single" | "frames" | "rolling";
  frameCount: number;
  inlined: boolean;
  capture: ProfileCaptureDto;
  chromeTrace: string;
  path: string;
  pending: boolean;
}

export interface ProfileCaptureDto {
  spans: ProfileSpanDto[];
  metadata: ProfileCaptureMetadataDto;
}

export interface ProfileSpanDto {
  name: string;
  lane: "cpu" | "gpu";
  startNs: number;
  endNs: number;
  parentIndex: number;
  depth: number;
  pipelineStats?: PipelineStatsDto;
}

export interface PipelineStatsDto {
  inputVertices: number;
  vertexInvocations: number;
  clippingInvocations: number;
  clippingPrimitives: number;
  fragmentInvocations: number;
  computeInvocations: number;
  pixels: number;
}

export interface ProfileCaptureMetadataDto {
  softwareGpu: boolean;
  correlated: boolean;
  deviceName: string;
  timestampPeriod: number;
  targetFps: number;
  mode: "off" | "timestamps" | "pipeline-stats";
  filter: string;
  frameCount: number;
}

export interface CaptureStatusResult {
  state: "idle" | "arming" | "recording" | "ready";
  capturedFrames: number;
  targetFrames: number;
  mode: "single" | "frames" | "rolling";
  pipelineStatsSupported: boolean;
}

export interface FrameHistoryParams {
  samples?: number;
}

export interface FrameHistoryDto {
  p50Ms: number;
  p95Ms: number;
  p99Ms: number;
  p999Ms: number;
  maxMs: number;
  meanMs: number;
  stddevMs: number;
  stutterCount: number;
  sampleCount: number;
  budgetMs: number;
  samples: FrameSampleDto[];
}

export interface FrameSampleDto {
  frameIndex: number;
  cpuMs: number;
  gpuMs: number;
  cpuWaitMs: number;
}

export interface PerfConfigDto {
  targetFps: number;
  budgetMs: number;
  greenBudgetFrac: number;
  greenMedianMul: number;
  amberMedianMul: number;
  frozenMs: number;
  vramWarnFrac: number;
  vramCritFrac: number;
}

export interface SetPerfConfigParams {
  targetFps?: number;
  greenBudgetFrac?: number;
  greenMedianMul?: number;
  amberMedianMul?: number;
  frozenMs?: number;
  vramWarnFrac?: number;
  vramCritFrac?: number;
}

export interface DrainAlarmsParams {
  since?: number;
}

export interface DrainAlarmsResult {
  events: AlarmEventDto[];
  highWaterSeq: number;
  oldestSeq: number;
  overflowed: boolean;
}

export interface AlarmEventDto {
  seq: number;
  fingerprint: string;
  metric: string;
  pass: string;
  severity: "info" | "warning" | "critical";
  state: "firing" | "resolved";
  value: number;
  threshold: number;
  sinceFrame: number;
  count: number;
  durationMs: number;
}

export interface ActiveAlarmsDto {
  alarms: ActiveAlarmDto[];
}

export interface ActiveAlarmDto {
  fingerprint: string;
  metric: string;
  pass: string;
  severity: "info" | "warning" | "critical";
  value: number;
  threshold: number;
  sinceFrame: number;
  count: number;
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

export interface SetSkinningResult {
  skinning: boolean;
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

export interface SetViewportSizeParams {
  width?: number;
  height?: number;
}

export interface SetViewportSizeResult {
  width: number;
  height: number;
}

export interface EntityList {
  entities: EntityListEntry[];
}

export interface EntityListEntry {
  id: WireUuid;
  name: string;
  parentId?: WireUuid;
  bone?: boolean;
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

export interface SetParentParams {
  entity: WireUuid | string | number;
  parent?: WireUuid | string | number;
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
  smooth?: boolean;
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
  metallicRoughnessTexture?: WireUuid;
  metallic?: number;
  roughness?: number;
  emissive?: Vec3;
  emissiveStrength?: number;
  unlit?: boolean;
  slot?: number;
  smooth?: boolean;
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
  playState: string;
  playVersion: number;
}

export interface DeselectResult {
  selectionVersion: number;
}

export interface PlayStateResult {
  state: string;
  playVersion: number;
  sceneVersion: number;
  hasPrimaryCamera: boolean;
}

export interface StepParams {
  frames?: number;
}

export interface ScriptStatusResult {
  state: string;
  instances: number;
  errorHighWater: number;
}

export interface DrainScriptErrorsParams {
  since?: number;
}

export interface DrainScriptErrorsResult {
  events: ScriptErrorDto[];
  highWaterSeq: number;
  oldestSeq: number;
  overflowed: boolean;
}

export interface ScriptErrorDto {
  seq: number;
  entity: WireUuid;
  script: string;
  message: string;
  tick: number;
}

export interface GetScriptSchemaParams {
  path: string;
}

export interface GetScriptSchemaResult {
  fields: ScriptFieldDto[];
}

export interface ScriptFieldDto {
  name: string;
  type: string;
  defaultValue: unknown;
}

export interface SetScriptOverrideParams {
  entity: WireUuid | string | number;
  slot: number;
  name: string;
  value: unknown;
}

export interface SetScriptOverrideResult {
  scriptPath: string;
  overrides: unknown;
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
  preserveChildren: boolean;
}

export interface SetGizmoParams {
  op?: "translate" | "rotate" | "scale";
  space?: "world" | "local";
  preserveChildren?: boolean;
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

export interface FlyInputParams {
  active?: boolean;
  lookDx?: number;
  lookDy?: number;
  forward?: boolean;
  back?: boolean;
  left?: boolean;
  right?: boolean;
  up?: boolean;
  down?: boolean;
}

export interface FlyInputResult {
  active: boolean;
}

export interface ScriptInputParams {
  keys: string[];
}

export interface ScriptInputResult {
  keys: string[];
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

export interface CreateScriptParams {
  name: string;
}

export interface CreateScriptResult {
  path: string;
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

export interface AssetMetadataParams {
  asset: WireUuid | string | number;
}

export interface AssetMetadataDto {
  id: WireUuid;
  name: string;
  type: "mesh" | "texture" | "other";
  path: string;
  folder?: string;
  sizeBytes: number;
  vertexCount?: number;
  triangleCount?: number;
  createdAt: number;
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
  slot: "mesh" | "albedo" | "metallic-roughness";
  asset: WireUuid | string | number;
}

export interface AssignAssetResult {
  id: WireUuid;
  name: string;
  slot: "mesh" | "albedo" | "metallic-roughness";
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
  "profiler.set-mode": ProfilerSetModeParams;
  "pass-timings": EmptyParams;
  "profiler.capture-start": CaptureStartParams;
  "profiler.capture-stop": EmptyParams;
  "profiler.capture-status": EmptyParams;
  "frame-history": FrameHistoryParams;
  "get-perf-config": EmptyParams;
  "set-perf-config": SetPerfConfigParams;
  "drain-alarms": DrainAlarmsParams;
  "list-active-alarms": EmptyParams;
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
  "set-skinning": ToggleParams;
  "set-depth-prepass": ToggleParams;
  "viewport-native-info": EmptyParams;
  "set-viewport-size": SetViewportSizeParams;
  "list-entities": EmptyParams;
  "list-components": EmptyParams;
  "create-entity": CreateEntityParams;
  "destroy-entity": EntityParams;
  "set-parent": SetParentParams;
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
  "play": EmptyParams;
  "pause": EmptyParams;
  "step": StepParams;
  "stop": EmptyParams;
  "get-play-state": EmptyParams;
  "get-script-status": EmptyParams;
  "drain-script-errors": DrainScriptErrorsParams;
  "get-script-schema": GetScriptSchemaParams;
  "set-script-override": SetScriptOverrideParams;
  "add-entity": AddEntityParams;
  "copy-entity": EntityParams;
  "rename-entity": RenameEntityParams;
  "set-component-field": SetComponentFieldParams;
  "get-camera": EmptyParams;
  "set-camera": SetCameraParams;
  "get-gizmo": EmptyParams;
  "set-gizmo": SetGizmoParams;
  "gizmo-pointer": GizmoPointerParams;
  "fly-input": FlyInputParams;
  "script-input": ScriptInputParams;
  "set-probes": SetProbesParams;
  "recapture-probes": EmptyParams;
  "list-probes": EmptyParams;
  "set-exposure": SetExposureParams;
  "get-project": EmptyParams;
  "new-project": NewProjectParams;
  "create-script": CreateScriptParams;
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
  "probe-asset": AssetMetadataParams;
  "delete-asset": DeleteAssetParams;
  "assign-asset": AssignAssetParams;
  "save-scene": PathParams;
  "load-scene": PathParams;
  "save-project": OptionalPathParams;
  "load-project": OptionalPathParams;
  "reload-project": EmptyParams;
  "screenshot": ScreenshotParams;
  "get-thumbnail": ThumbnailParams;
  "view-asset": ThumbnailParams;
  "quit": EmptyParams;
}

export interface CommandResultMap {
  "ping": PingResult;
  "render-stats": RenderStatsDto;
  "profiler.set-mode": ProfilerModeResult;
  "pass-timings": RenderPassTimingsDto;
  "profiler.capture-start": CaptureStartResult;
  "profiler.capture-stop": CaptureStopResult;
  "profiler.capture-status": CaptureStatusResult;
  "frame-history": FrameHistoryDto;
  "get-perf-config": PerfConfigDto;
  "set-perf-config": PerfConfigDto;
  "drain-alarms": DrainAlarmsResult;
  "list-active-alarms": ActiveAlarmsDto;
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
  "set-skinning": SetSkinningResult;
  "set-depth-prepass": SetDepthPrepassResult;
  "viewport-native-info": ViewportNativeInfoResult;
  "set-viewport-size": SetViewportSizeResult;
  "list-entities": EntityList;
  "list-components": ComponentList;
  "create-entity": EntityRef;
  "destroy-entity": DestroyEntityResult;
  "set-parent": EntityRef;
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
  "play": PlayStateResult;
  "pause": PlayStateResult;
  "step": PlayStateResult;
  "stop": PlayStateResult;
  "get-play-state": PlayStateResult;
  "get-script-status": ScriptStatusResult;
  "drain-script-errors": DrainScriptErrorsResult;
  "get-script-schema": GetScriptSchemaResult;
  "set-script-override": SetScriptOverrideResult;
  "add-entity": EntityRef;
  "copy-entity": EntityRef;
  "rename-entity": EntityRef;
  "set-component-field": SetComponentFieldResult;
  "get-camera": EditorCamera;
  "set-camera": EditorCamera;
  "get-gizmo": GizmoState;
  "set-gizmo": GizmoState;
  "gizmo-pointer": GizmoPointerResult;
  "fly-input": FlyInputResult;
  "script-input": ScriptInputResult;
  "set-probes": SetProbesResult;
  "recapture-probes": RecaptureProbesResult;
  "list-probes": ListProbesResult;
  "set-exposure": SetExposureResult;
  "get-project": ProjectInfoDto;
  "new-project": ProjectInfoDto;
  "create-script": CreateScriptResult;
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
  "probe-asset": AssetMetadataDto;
  "delete-asset": DeleteAssetResult;
  "assign-asset": AssignAssetResult;
  "save-scene": PathResult;
  "load-scene": PathResult;
  "save-project": ProjectInfoDto;
  "load-project": ProjectInfoDto;
  "reload-project": ProjectInfoDto;
  "screenshot": ScreenshotResult;
  "get-thumbnail": ThumbnailResult;
  "view-asset": ThumbnailResult;
  "quit": QuitResult;
}
