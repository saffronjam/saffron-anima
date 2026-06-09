import type {
  AssetEntryDto,
  AssetList as DtoAssetList,
  CommandParamsMap as DtoCommandParamsMap,
  CommandResultMap as DtoCommandResultMap,
  EditorCamera,
  EntityList,
  EntityRef,
  GizmoState,
  ProjectInfoDto,
  RenderStatsDto,
  ThumbnailResult,
  Vec3,
  Vec4,
  WireUuid,
} from "./se-types";

export type {
  ActiveAlarmDto,
  ActiveAlarmsDto,
  AddEntityParams,
  AlarmEventDto,
  AssetMetadataDto,
  AssetUsageDto,
  AssetUsagesParams,
  AssetUsagesResult,
  AssignAssetParams,
  ComponentBody,
  ComponentList,
  ComponentParams,
  CaptureStartParams,
  CaptureStartResult,
  CaptureStatusResult,
  CaptureStopResult,
  CreateEntityParams,
  CreateAssetFolderParams,
  DeleteAssetParams,
  DeleteAssetResult,
  DeselectResult,
  DestroyEntityResult,
  DrainAlarmsParams,
  DrainAlarmsResult,
  EditorCamera,
  EntityList,
  EntityListEntry,
  EntityParams,
  EntityRef,
  FrameHistoryDto,
  FrameHistoryParams,
  FrameSampleDto,
  GizmoPointerParams,
  GizmoPointerResult,
  GizmoState,
  ImportModelResult,
  ImportTextureResult,
  ListProbesResult,
  OptionalPathParams,
  PathParams,
  PathResult,
  PickParams,
  PickResult,
  PingParams,
  PingResult,
  PerfConfigDto,
  PipelineStatsDto,
  PlayStateResult,
  ProbeRef,
  ProfileCaptureDto,
  ProfileCaptureMetadataDto,
  ProfileSpanDto,
  ProfilerModeResult,
  ProfilerSetModeParams,
  QuitResult,
  RecaptureProbesResult,
  RenderPassTimingDto,
  RenderPassTimingsDto,
  RenameAssetParams,
  RenameAssetFolderParams,
  DeleteAssetFolderParams,
  MoveAssetParams,
  RenameEntityParams,
  ScreenshotParams,
  ScreenshotResult,
  SetAaParams,
  SetAaResult,
  SetAtmosphereParams,
  SetCameraParams,
  SetClusteredResult,
  SetComponentFieldParams,
  SetComponentFieldResult,
  SetComponentParams,
  SetComponentResult,
  SetContactShadowsResult,
  SetDepthPrepassResult,
  SetEnvironmentParams,
  SetExposureParams,
  SetExposureResult,
  SetGiParams,
  SetGiResult,
  SetGizmoParams,
  SetIblResult,
  SetLightParams,
  SetPerfConfigParams,
  SetMaterialParams,
  SetProbesParams,
  SetProbesResult,
  SetRestirResult,
  SetRtShadowsResult,
  SetShadowsResult,
  SetSsaoResult,
  SetSsgiResult,
  SetTransformParams,
  StepParams,
  ThumbnailParams,
  ToggleParams,
  Vec3,
  Vec4,
  ViewportNativeInfoResult,
  WireUuid,
} from "./se-types";

export type Uuid = WireUuid;
export type AssetEntry = AssetEntryDto;
export type AssetList = DtoAssetList;
export type ProjectInfo = ProjectInfoDto;
export type RenderStats = RenderStatsDto;
export type Thumbnail = Omit<ThumbnailResult, "format"> & { format: "png" };

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
  ReflectionProbe?: ReflectionProbe;
  RenderStats?: RenderStats;
  Selection?: Selection;
  SpotLight?: SpotLight;
  Thumbnail?: Thumbnail;
  Transform?: Transform;
  Uuid?: Uuid;
  Vec3?: Vec3;
  Vec4?: Vec4;
}

export interface Camera {
  fov: number;
  near: number;
  far: number;
  primary: boolean;
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
}

export interface Name {
  name: string;
}

export interface Transform {
  translation: Vec3;
  scale: Vec3;
  rotation: Vec3;
}

export interface Mesh {
  mesh: Uuid;
}

export interface Material {
  baseColor: Vec4;
  albedoTexture: Uuid;
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

export interface Envelope {
  ok: boolean;
  error?: string;
  result?: unknown;
}

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
  atmosphere: {
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
  };
}

export interface InspectResult {
  id: Uuid;
  name: string;
  components: Components;
}

export interface Selection {
  entity: EntityRef | null;
  selectionVersion: number;
  sceneVersion: number;
  playState: string;
  playVersion: number;
}

type CompatCommandResultOverrides = {
  "get-environment": Environment;
  "set-environment": Environment;
  "set-atmosphere": Environment;
  "get-selection": Selection;
  inspect: InspectResult;
  "get-thumbnail": Thumbnail;
  "view-asset": Thumbnail;
};

export type CommandParamsMap = DtoCommandParamsMap;
export type CommandResultMap = Omit<
  DtoCommandResultMap,
  keyof CompatCommandResultOverrides
> &
  CompatCommandResultOverrides;
