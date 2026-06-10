#!/usr/bin/env bun
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

interface Field {
  type: string;
  name: string;
}

interface StructDef {
  name: string;
  fields: Field[];
}

interface EnumDef {
  name: string;
  values: string[];
}

interface CommandDef {
  name: string;
  params: string;
  result: string;
  summary: string;
}

interface ManifestCommand {
  name: string;
  params: string;
  result: string;
  status: "typed";
  fixture?: string;
  skip?: string;
}

const scriptDir = dirname(fileURLToPath(import.meta.url));
const repoRoot = dirname(dirname(scriptDir));
const dtoFile = join(repoRoot, "engine/source/saffron/control/control_dto.cppm");
const cppOut = join(repoRoot, "engine/source/saffron/control/control_dto_serde.generated.cpp");
const sceneSerdeOut = join(
  repoRoot,
  "engine/source/saffron/scene/scene_component_serde.generated.cpp",
);
const tsOut = join(repoRoot, "editor/src/protocol/se-types.ts");
const openRpcOut = join(repoRoot, "schemas/control/openrpc.generated.json");
const manifestOut = join(repoRoot, "schemas/control/command-manifest.generated.json");

const scalarTypes = new Set([
  "bool",
  "i32",
  "u32",
  "u64",
  "i64",
  "f32",
  "WireUuid",
  "EntitySelector",
  "AssetSelector",
  "std::string",
  "Json",
]);

const enumWireNames = new Map<string, Record<string, string>>([
  [
    "AddEntityPreset",
    {
      Empty: "empty",
      Cube: "cube",
      Model: "model",
      PointLight: "point-light",
      SpotLight: "spot-light",
      DirectionalLight: "directional-light",
      Camera: "camera",
      ReflectionProbe: "reflection-probe",
    },
  ],
  ["PickKind", { Billboard: "billboard", Mesh: "mesh" }],
  ["GizmoOpDto", { Translate: "translate", Rotate: "rotate", Scale: "scale" }],
  ["GizmoSpaceDto", { World: "world", Local: "local" }],
  ["GizmoPointerPhase", { Hover: "hover", Begin: "begin", Drag: "drag", End: "end" }],
  [
    "AaModeDto",
    { Off: "off", Fxaa: "fxaa", Taa: "taa", Msaa2: "msaa2", Msaa4: "msaa4", Msaa8: "msaa8" },
  ],
  ["GiModeDto", { Off: "off", Ddgi: "ddgi" }],
  ["AssetSlotDto", { Mesh: "mesh", Albedo: "albedo", MetallicRoughness: "metallic-roughness" }],
  ["ScreenshotTargetDto", { Viewport: "viewport", Window: "window" }],
  ["AssetTypeDto", { Mesh: "mesh", Texture: "texture", Other: "other" }],
  ["ProfilerModeDto", { Off: "off", Timestamps: "timestamps", PipelineStats: "pipeline-stats" }],
  ["ProfileLaneDto", { Cpu: "cpu", Gpu: "gpu" }],
  ["CaptureModeDto", { Single: "single", Frames: "frames", Rolling: "rolling" }],
  ["CaptureStateDto", { Idle: "idle", Arming: "arming", Recording: "recording", Ready: "ready" }],
  ["AlarmSeverityDto", { Info: "info", Warning: "warning", Critical: "critical" }],
  ["AlarmStateDto", { Firing: "firing", Resolved: "resolved" }],
]);

const commands: CommandDef[] = [
  { name: "ping", params: "PingParams", result: "PingResult", summary: "liveness + engine info" },
  {
    name: "render-stats",
    params: "EmptyParams",
    result: "RenderStatsDto",
    summary: "last frame draw counters",
  },
  {
    name: "profiler.set-mode",
    params: "ProfilerSetModeParams",
    result: "ProfilerModeResult",
    summary: "set the GPU profiler mode",
  },
  {
    name: "pass-timings",
    params: "EmptyParams",
    result: "RenderPassTimingsDto",
    summary: "last frame per-pass GPU timings",
  },
  {
    name: "profiler.capture-start",
    params: "CaptureStartParams",
    result: "CaptureStartResult",
    summary: "arm a bounded profiler capture",
  },
  {
    name: "profiler.capture-stop",
    params: "EmptyParams",
    result: "CaptureStopResult",
    summary: "finish + return the armed profiler capture",
  },
  {
    name: "profiler.capture-status",
    params: "EmptyParams",
    result: "CaptureStatusResult",
    summary: "non-destructive capture progress",
  },
  {
    name: "frame-history",
    params: "FrameHistoryParams",
    result: "FrameHistoryDto",
    summary: "frame-time percentiles + stutter count",
  },
  {
    name: "get-perf-config",
    params: "EmptyParams",
    result: "PerfConfigDto",
    summary: "shared frame-budget / threshold config",
  },
  {
    name: "set-perf-config",
    params: "SetPerfConfigParams",
    result: "PerfConfigDto",
    summary: "set the frame budget + thresholds",
  },
  {
    name: "drain-alarms",
    params: "DrainAlarmsParams",
    result: "DrainAlarmsResult",
    summary: "drain perf-alarm events (seq cursor)",
  },
  {
    name: "list-active-alarms",
    params: "EmptyParams",
    result: "ActiveAlarmsDto",
    summary: "currently firing perf alarms",
  },
  {
    name: "set-aa",
    params: "SetAaParams",
    result: "SetAaResult",
    summary: "set anti-aliasing mode",
  },
  {
    name: "set-clustered",
    params: "ToggleParams",
    result: "SetClusteredResult",
    summary: "toggle clustered lighting",
  },
  {
    name: "set-ibl",
    params: "ToggleParams",
    result: "SetIblResult",
    summary: "toggle image-based lighting",
  },
  {
    name: "set-ssao",
    params: "ToggleParams",
    result: "SetSsaoResult",
    summary: "toggle ambient occlusion",
  },
  {
    name: "set-contact-shadows",
    params: "ToggleParams",
    result: "SetContactShadowsResult",
    summary: "toggle contact shadows",
  },
  { name: "set-ssgi", params: "ToggleParams", result: "SetSsgiResult", summary: "toggle SSGI" },
  {
    name: "set-rt-shadows",
    params: "ToggleParams",
    result: "SetRtShadowsResult",
    summary: "toggle ray-traced shadows",
  },
  {
    name: "set-restir",
    params: "ToggleParams",
    result: "SetRestirResult",
    summary: "toggle ReSTIR",
  },
  { name: "set-gi", params: "SetGiParams", result: "SetGiResult", summary: "set GI mode" },
  {
    name: "set-shadows",
    params: "ToggleParams",
    result: "SetShadowsResult",
    summary: "toggle shadows",
  },
  {
    name: "set-skinning",
    params: "ToggleParams",
    result: "SetSkinningResult",
    summary: "toggle GPU skinning",
  },
  {
    name: "set-depth-prepass",
    params: "ToggleParams",
    result: "SetDepthPrepassResult",
    summary: "toggle depth prepass",
  },
  {
    name: "viewport-native-info",
    params: "EmptyParams",
    result: "ViewportNativeInfoResult",
    summary: "native viewport bridge status",
  },
  {
    name: "set-viewport-size",
    params: "SetViewportSizeParams",
    result: "SetViewportSizeResult",
    summary: "set the offscreen render size",
  },
  {
    name: "list-entities",
    params: "EmptyParams",
    result: "EntityList",
    summary: "list all entities",
  },
  {
    name: "list-components",
    params: "EmptyParams",
    result: "ComponentList",
    summary: "list registered component types",
  },
  {
    name: "create-entity",
    params: "CreateEntityParams",
    result: "EntityRef",
    summary: "create-entity {name}",
  },
  {
    name: "destroy-entity",
    params: "EntityParams",
    result: "DestroyEntityResult",
    summary: "destroy-entity {entity}",
  },
  {
    name: "set-parent",
    params: "SetParentParams",
    result: "EntityRef",
    summary: "set-parent {entity, parent?} — reparent (absent/0 parent detaches to root)",
  },
  {
    name: "add-component",
    params: "ComponentParams",
    result: "AddComponentResult",
    summary: "add-component {entity, component}",
  },
  {
    name: "remove-component",
    params: "ComponentParams",
    result: "RemoveComponentResult",
    summary: "remove-component {entity, component}",
  },
  {
    name: "set-component",
    params: "SetComponentParams",
    result: "SetComponentResult",
    summary: "set-component {entity, component, json}",
  },
  {
    name: "set-transform",
    params: "SetTransformParams",
    result: "EntityRef",
    summary: "set-transform {entity, translation?, rotation?, scale?}",
  },
  {
    name: "set-material",
    params: "SetMaterialParams",
    result: "EntityRef",
    summary: "set-material {entity, material fields..., slot?}",
  },
  {
    name: "set-light",
    params: "SetLightParams",
    result: "EntityRef",
    summary: "set-light {entity?, direction?, color?, intensity?, ambient?}",
  },
  { name: "select", params: "EntityParams", result: "EntityRef", summary: "select {entity}" },
  { name: "pick", params: "PickParams", result: "PickResult", summary: "pick {u=0.5, v=0.5}" },
  {
    name: "inspect",
    params: "EntityParams",
    result: "InspectResult",
    summary: "inspect {entity}",
  },
  { name: "focus", params: "EntityParams", result: "EntityRef", summary: "focus {entity}" },
  {
    name: "get-environment",
    params: "EmptyParams",
    result: "EnvironmentDto",
    summary: "get environment settings",
  },
  {
    name: "set-environment",
    params: "SetEnvironmentParams",
    result: "EnvironmentDto",
    summary: "set environment settings",
  },
  {
    name: "set-atmosphere",
    params: "SetAtmosphereParams",
    result: "EnvironmentDto",
    summary: "set procedural-atmosphere settings",
  },
  {
    name: "get-selection",
    params: "EmptyParams",
    result: "SelectionResult",
    summary: "get current selection",
  },
  { name: "deselect", params: "EmptyParams", result: "DeselectResult", summary: "clear selection" },
  {
    name: "play",
    params: "EmptyParams",
    result: "PlayStateResult",
    summary: "enter or resume play mode",
  },
  {
    name: "pause",
    params: "EmptyParams",
    result: "PlayStateResult",
    summary: "pause the running scene",
  },
  {
    name: "step",
    params: "StepParams",
    result: "PlayStateResult",
    summary: "step {frames=1} while paused",
  },
  {
    name: "stop",
    params: "EmptyParams",
    result: "PlayStateResult",
    summary: "stop play and restore the authored scene",
  },
  {
    name: "get-play-state",
    params: "EmptyParams",
    result: "PlayStateResult",
    summary: "current play state",
  },
  {
    name: "get-script-status",
    params: "EmptyParams",
    result: "ScriptStatusResult",
    summary: "play state, live script instances, error high-water",
  },
  {
    name: "drain-script-errors",
    params: "DrainScriptErrorsParams",
    result: "DrainScriptErrorsResult",
    summary: "drain script errors (seq cursor)",
  },
  {
    name: "get-script-schema",
    params: "GetScriptSchemaParams",
    result: "GetScriptSchemaResult",
    summary: "a project script's declared fields",
  },
  {
    name: "set-script-override",
    params: "SetScriptOverrideParams",
    result: "SetScriptOverrideResult",
    summary: "write one per-instance script field override",
  },
  {
    name: "add-entity",
    params: "AddEntityParams",
    result: "EntityRef",
    summary: "add-entity {preset}",
  },
  {
    name: "copy-entity",
    params: "EntityParams",
    result: "EntityRef",
    summary: "copy-entity {entity}",
  },
  {
    name: "rename-entity",
    params: "RenameEntityParams",
    result: "EntityRef",
    summary: "rename-entity {entity, name}",
  },
  {
    name: "set-component-field",
    params: "SetComponentFieldParams",
    result: "SetComponentFieldResult",
    summary: "set-component-field {entity, component, field, value}",
  },
  { name: "get-camera", params: "EmptyParams", result: "EditorCamera", summary: "get camera" },
  {
    name: "set-camera",
    params: "SetCameraParams",
    result: "EditorCamera",
    summary: "set camera",
  },
  { name: "get-gizmo", params: "EmptyParams", result: "GizmoState", summary: "get gizmo" },
  {
    name: "set-gizmo",
    params: "SetGizmoParams",
    result: "GizmoState",
    summary: "set gizmo",
  },
  {
    name: "gizmo-pointer",
    params: "GizmoPointerParams",
    result: "GizmoPointerResult",
    summary: "drive gizmo pointer",
  },
  {
    name: "fly-input",
    params: "FlyInputParams",
    result: "FlyInputResult",
    summary: "stream editor fly-cam input",
  },
  {
    name: "script-input",
    params: "ScriptInputParams",
    result: "ScriptInputResult",
    summary: "set Lua gameplay key state",
  },
  {
    name: "set-probes",
    params: "SetProbesParams",
    result: "SetProbesResult",
    summary: "toggle reflection-probe sampling",
  },
  {
    name: "recapture-probes",
    params: "EmptyParams",
    result: "RecaptureProbesResult",
    summary: "mark reflection probes dirty",
  },
  {
    name: "list-probes",
    params: "EmptyParams",
    result: "ListProbesResult",
    summary: "list captured reflection probes",
  },
  {
    name: "set-exposure",
    params: "SetExposureParams",
    result: "SetExposureResult",
    summary: "set-exposure {ev}",
  },
  {
    name: "get-project",
    params: "EmptyParams",
    result: "ProjectInfoDto",
    summary: "active project metadata",
  },
  {
    name: "new-project",
    params: "NewProjectParams",
    result: "ProjectInfoDto",
    summary: "new-project {name}",
  },
  {
    name: "create-script",
    params: "CreateScriptParams",
    result: "CreateScriptResult",
    summary: "boilerplate .lua under the project src/",
  },
  {
    name: "open-project",
    params: "PathParams",
    result: "ProjectInfoDto",
    summary: "open-project {path}",
  },
  {
    name: "import-model",
    params: "PathParams",
    result: "ImportModelResult",
    summary: "import-model {path}",
  },
  {
    name: "import-texture",
    params: "PathParams",
    result: "ImportTextureResult",
    summary: "import-texture {path}",
  },
  {
    name: "list-assets",
    params: "EmptyParams",
    result: "AssetList",
    summary: "list project asset catalog",
  },
  {
    name: "rename-asset",
    params: "RenameAssetParams",
    result: "AssetRef",
    summary: "rename-asset {asset, name}",
  },
  {
    name: "create-asset-folder",
    params: "CreateAssetFolderParams",
    result: "AssetList",
    summary: "create virtual asset folder",
  },
  {
    name: "rename-asset-folder",
    params: "RenameAssetFolderParams",
    result: "AssetList",
    summary: "rename virtual asset folder",
  },
  {
    name: "delete-asset-folder",
    params: "DeleteAssetFolderParams",
    result: "AssetList",
    summary: "delete virtual asset folder",
  },
  {
    name: "move-asset",
    params: "MoveAssetParams",
    result: "AssetRef",
    summary: "move asset to virtual folder",
  },
  {
    name: "asset-usages",
    params: "AssetUsagesParams",
    result: "AssetUsagesResult",
    summary: "list scene usages of an asset",
  },
  {
    name: "probe-asset",
    params: "AssetMetadataParams",
    result: "AssetMetadataDto",
    summary: "probe asset metadata (size, vertices, created)",
  },
  {
    name: "delete-asset",
    params: "DeleteAssetParams",
    result: "DeleteAssetResult",
    summary: "delete asset",
  },
  {
    name: "assign-asset",
    params: "AssignAssetParams",
    result: "AssignAssetResult",
    summary: "assign asset to entity",
  },
  { name: "save-scene", params: "PathParams", result: "PathResult", summary: "save-scene {path}" },
  { name: "load-scene", params: "PathParams", result: "PathResult", summary: "load-scene {path}" },
  {
    name: "save-project",
    params: "OptionalPathParams",
    result: "ProjectInfoDto",
    summary: "save active project",
  },
  {
    name: "load-project",
    params: "OptionalPathParams",
    result: "ProjectInfoDto",
    summary: "load-project {path}",
  },
  {
    name: "reload-project",
    params: "EmptyParams",
    result: "ProjectInfoDto",
    summary: "reload the active project",
  },
  {
    name: "screenshot",
    params: "ScreenshotParams",
    result: "ScreenshotResult",
    summary: "capture screenshot",
  },
  {
    name: "get-thumbnail",
    params: "ThumbnailParams",
    result: "ThumbnailResult",
    summary: "get asset thumbnail",
  },
  {
    name: "view-asset",
    params: "ThumbnailParams",
    result: "ThumbnailResult",
    summary: "view asset thumbnail",
  },
  { name: "quit", params: "EmptyParams", result: "QuitResult", summary: "close the running app" },
];

const commandFixtures = new Map<string, string>([
  ["ping", "empty"],
  ["render-stats", "empty"],
  ["profiler.set-mode", "profiler-timestamps"],
  ["pass-timings", "empty"],
  ["profiler.capture-start", "capture-single"],
  ["profiler.capture-stop", "empty"],
  ["profiler.capture-status", "empty"],
  ["frame-history", "frame-history-samples"],
  ["get-perf-config", "empty"],
  ["set-perf-config", "perf-config-30"],
  ["drain-alarms", "alarms-since-0"],
  ["list-active-alarms", "empty"],
  ["set-aa", "aa"],
  ["set-clustered", "toggle-on"],
  ["set-ibl", "toggle-on"],
  ["set-ssao", "toggle-on"],
  ["set-contact-shadows", "toggle-on"],
  ["set-ssgi", "toggle-on"],
  ["set-rt-shadows", "toggle-off"],
  ["set-restir", "toggle-off"],
  ["set-gi", "gi-off"],
  ["set-shadows", "toggle-on"],
  ["set-skinning", "toggle-on"],
  ["set-depth-prepass", "toggle-on"],
  ["viewport-native-info", "empty"],
  ["list-entities", "empty"],
  ["list-components", "empty"],
  ["create-entity", "new-entity"],
  ["destroy-entity", "temp-entity"],
  ["set-parent", "temp-child-under-cube"],
  ["add-component", "temp-camera-entity"],
  ["remove-component", "temp-camera-component"],
  ["set-component", "cube-name-component"],
  ["set-transform", "cube-transform"],
  ["set-material", "cube-material"],
  ["set-light", "temp-directional-light"],
  ["select", "cube-entity"],
  ["pick", "viewport-center"],
  ["inspect", "cube-entity"],
  ["focus", "cube-entity"],
  ["get-environment", "empty"],
  ["set-environment", "environment-intensity"],
  ["set-atmosphere", "atmosphere-disabled"],
  ["get-selection", "empty"],
  ["deselect", "empty"],
  ["play", "empty"],
  ["pause", "empty"],
  ["step", "step-one"],
  ["stop", "empty"],
  ["get-play-state", "empty"],
  ["get-script-status", "empty"],
  ["drain-script-errors", "alarms-since-0"],
  ["get-script-schema", "script-schema-file"],
  ["set-script-override", "script-override-slot"],
  ["add-entity", "cube-preset"],
  ["copy-entity", "cube-entity"],
  ["rename-entity", "cube-rename"],
  ["set-component-field", "cube-name-field"],
  ["get-camera", "empty"],
  ["set-camera", "camera-yaw"],
  ["get-gizmo", "empty"],
  ["set-gizmo", "gizmo-rotate-local"],
  ["gizmo-pointer", "gizmo-hover"],
  ["fly-input", "fly-idle"],
  ["script-input", "script-input-w"],
  ["set-viewport-size", "viewport-size"],
  ["set-probes", "toggle-on"],
  ["recapture-probes", "empty"],
  ["list-probes", "empty"],
  ["set-exposure", "exposure-zero"],
  ["get-project", "empty"],
  ["new-project", "new-project"],
  ["open-project", "project-name"],
  ["list-assets", "empty"],
  ["rename-asset", "mesh-asset-rename"],
  ["asset-usages", "mesh-asset"],
  ["probe-asset", "mesh-asset"],
  ["assign-asset", "cube-mesh-asset"],
  ["save-project", "empty"],
  ["load-project", "project-name"],
  ["get-thumbnail", "mesh-asset"],
  ["view-asset", "mesh-asset-view"],
]);

const commandSkips = new Map<string, string>([
  ["import-model", "requires an external model fixture path"],
  ["import-texture", "requires an external texture fixture path"],
  ["create-asset-folder", "mutates the project asset catalog"],
  ["rename-asset-folder", "mutates the project asset catalog"],
  ["delete-asset-folder", "mutates the project asset catalog"],
  ["move-asset", "mutates the project asset catalog"],
  ["delete-asset", "removes a project asset"],
  ["create-script", "writes a script file into the project src/"],
  ["save-scene", "writes a scene file"],
  ["load-scene", "loads and replaces the scene from a file"],
  ["reload-project", "reloads and replaces the active project's scene and catalog"],
  ["screenshot", "writes an image file and can be deferred"],
  ["quit", "terminates the host process"],
]);

function stripComments(text: string): string {
  return text.replaceAll(/\/\/.*$/gm, "").replaceAll(/\/\*[\s\S]*?\*\//g, "");
}

function parseStructs(text: string): Map<string, StructDef> {
  const structs = new Map<string, StructDef>();
  const re = /struct\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{([\s\S]*?)\};/g;
  for (const match of stripComments(text).matchAll(re)) {
    const name = match[1];
    const body = match[2];
    const fields: Field[] = [];
    for (const raw of body.split(";")) {
      const line = raw.trim();
      if (line === "") {
        continue;
      }
      if (line.includes("(") || line.includes(")") || line.includes("=")) {
        throw new Error(`unsupported member in ${name}: ${line}`);
      }
      const field = /^(.+?)\s+([A-Za-z_][A-Za-z0-9_]*)$/.exec(line);
      if (!field) {
        throw new Error(`cannot parse field in ${name}: ${line}`);
      }
      fields.push({ type: field[1].trim(), name: field[2] });
    }
    structs.set(name, { name, fields });
  }
  return structs;
}

function parseEnums(text: string): Map<string, EnumDef> {
  const enums = new Map<string, EnumDef>();
  const re = /enum\s+class\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{([\s\S]*?)\};/g;
  for (const match of stripComments(text).matchAll(re)) {
    const name = match[1];
    const values = match[2]
      .split(",")
      .map((value) => value.trim())
      .filter(Boolean);
    enums.set(name, { name, values });
  }
  return enums;
}

function vectorInner(type: string): string | undefined {
  return /^std::vector<(.+)>$/.exec(type)?.[1].trim();
}

function optionalInner(type: string): string | undefined {
  return /^std::optional<(.+)>$/.exec(type)?.[1].trim();
}

function validateType(
  type: string,
  structs: Map<string, StructDef>,
  enums: Map<string, EnumDef>,
): void {
  if (scalarTypes.has(type) || structs.has(type) || enums.has(type)) {
    return;
  }
  const vector = vectorInner(type);
  if (vector) {
    validateType(vector, structs, enums);
    return;
  }
  const optional = optionalInner(type);
  if (optional) {
    validateType(optional, structs, enums);
    return;
  }
  throw new Error(`unsupported DTO field type: ${type}`);
}

function requireStructs(structs: Map<string, StructDef>, names: Iterable<string>): StructDef[] {
  return [...names].map((name) => {
    const def = structs.get(name);
    if (!def) {
      throw new Error(`missing DTO struct ${name}`);
    }
    return def;
  });
}

function commandTypeNames(): string[] {
  return [...new Set(commands.flatMap((command) => [command.params, command.result]))];
}

function structDeps(type: string, structs: Map<string, StructDef>): string[] {
  const optional = optionalInner(type);
  if (optional) {
    return structDeps(optional, structs);
  }
  const vector = vectorInner(type);
  if (vector) {
    return structDeps(vector, structs);
  }
  if (!structs.has(type) || type === "DtoTag" || scalarTypes.has(type)) {
    return [];
  }
  return [type, ...structs.get(type)!.fields.flatMap((field) => structDeps(field.type, structs))];
}

function transitiveStructs(roots: string[], structs: Map<string, StructDef>): string[] {
  return [...new Set(roots.flatMap((name) => structDeps(name, structs)))];
}

function cppParseValue(type: string, valueExpr: string, keyExpr: string): string {
  const optional = optionalInner(type);
  if (optional) {
    throw new Error(`unexpected optional parse value ${type}`);
  }
  const vector = vectorInner(type);
  if (vector) {
    return `readVector<${vector}>(${valueExpr}, ${keyExpr})`;
  }
  switch (type) {
    case "std::string":
      return `readString(${valueExpr}, ${keyExpr})`;
    case "f32":
      return `readF32(${valueExpr}, ${keyExpr})`;
    case "i32":
      return `readI32(${valueExpr}, ${keyExpr})`;
    case "u32":
      return `readU32(${valueExpr}, ${keyExpr})`;
    case "i64":
      return `readI64(${valueExpr}, ${keyExpr})`;
    case "u64":
      return `readU64(${valueExpr}, ${keyExpr})`;
    case "bool":
      return `readBool(${valueExpr}, ${keyExpr})`;
    case "WireUuid":
      return `readWireUuid(${valueExpr}, ${keyExpr})`;
    case "EntitySelector":
      return `readEntitySelector(${valueExpr}, ${keyExpr})`;
    case "AssetSelector":
      return `readAssetSelector(${valueExpr}, ${keyExpr})`;
    case "Json":
      return `readJson(${valueExpr}, ${keyExpr})`;
    default:
      if (enumWireNames.has(type)) {
        return `read${type}(${valueExpr}, ${keyExpr})`;
      }
      return `parseDto(${valueExpr}, DtoTag<${type}>{})`;
  }
}

function cppParseField(field: Field, index: number): string {
  const key = JSON.stringify(field.name);
  const assign = `out.${field.name}`;
  const optional = optionalInner(field.type);
  if (optional) {
    const parse = cppParseValue(optional, "*value", key);
    return `
        {
            auto value = optionalField(params, ${key}, ${index}, true);
            if (value && !value->is_null())
            {
                auto parsed = ${parse};
                if (!parsed) { return Err(std::move(parsed.error())); }
                ${assign} = std::move(*parsed);
            }
        }`;
  }
  const parse = cppParseValue(field.type, "**value", key);
  return `
        {
            auto value = requiredField(params, ${key}, ${index}, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = ${parse};
            if (!parsed) { return Err(std::move(parsed.error())); }
            ${assign} = std::move(*parsed);
        }`;
}

function cppJsonValue(type: string, expr: string): string {
  const vector = vectorInner(type);
  if (vector) {
    return `dtoVectorToJson(${expr})`;
  }
  switch (type) {
    case "bool":
    case "i32":
    case "u32":
    case "u64":
    case "i64":
    case "f32":
    case "std::string":
      return expr;
    case "WireUuid":
      return `dtoToJson(${expr})`;
    case "Json":
      return expr;
    default:
      return `dtoToJson(${expr})`;
  }
}

function emitEnumHelpers(enums: Map<string, EnumDef>): string {
  return [...enums.values()]
    .filter((def) => enumWireNames.has(def.name))
    .map((def) => {
      const names = enumWireNames.get(def.name)!;
      const readCases = def.values
        .map((value) => {
          const wire = names[value];
          if (!wire) {
            throw new Error(`missing wire name for ${def.name}::${value}`);
          }
          return `            if (text == ${JSON.stringify(wire)}) { return ${def.name}::${value}; }`;
        })
        .join("\n");
      const writeCases = def.values
        .map(
          (value) =>
            `            case ${def.name}::${value}: return ${JSON.stringify(names[value])};`,
        )
        .join("\n");
      return `        auto read${def.name}(const Json& value, std::string_view key) -> Result<${def.name}>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
${readCases}
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto ${def.name}Name(${def.name} value) -> const char*
        {
            switch (value)
            {
${writeCases}
            }
            return "";
        }`;
    })
    .join("\n\n");
}

function emitCpp(structs: Map<string, StructDef>, enums: Map<string, EnumDef>): string {
  const parseNames = transitiveStructs(
    commands.map((command) => command.params),
    structs,
  );
  const toJsonNames = transitiveStructs(
    [...commands.map((command) => command.result), "Vec3", "Vec4"],
    structs,
  );
  const parseDefs = requireStructs(structs, parseNames);
  const toJsonDefs = requireStructs(structs, toJsonNames);
  for (const def of [...parseDefs, ...toJsonDefs]) {
    for (const field of def.fields) {
      validateType(field.type, structs, enums);
    }
  }

  const parseFns = parseDefs
    .map((def) => {
      const body =
        def.fields.length === 0
          ? ""
          : def.fields.map((field, index) => cppParseField(field, index)).join("\n");
      return `auto parseDto(const Json& params, DtoTag<${def.name}>) -> Result<${def.name}>
    {
        ${def.name} out;
${body}
        return out;
    }`;
    })
    .join("\n\n    ");

  const enumJsonFns = [...enums.values()]
    .filter((def) => enumWireNames.has(def.name))
    .map(
      (def) => `auto dtoToJson(${def.name} value) -> Json
    {
        return ${def.name}Name(value);
    }`,
    )
    .join("\n\n    ");

  const toJsonFns = toJsonDefs
    .map((def) => {
      const fields = def.fields
        .map((field) => {
          const optional = optionalInner(field.type);
          if (optional) {
            const value = cppJsonValue(optional, `*value.${field.name}`);
            if (def.name === "SelectionResult" && field.name === "entity") {
              return `        if (value.${field.name}) { out[${JSON.stringify(field.name)}] = ${value}; }
        else { out[${JSON.stringify(field.name)}] = nullptr; }`;
            }
            return `        if (value.${field.name}) { out[${JSON.stringify(field.name)}] = ${value}; }`;
          }
          const value = cppJsonValue(field.type, `value.${field.name}`);
          return `        out[${JSON.stringify(field.name)}] = ${value};`;
        })
        .join("\n");
      if (def.name === "EnvironmentDto") {
        return `auto dtoToJson(const ${def.name}& value) -> Json
    {
        return value.value;
    }`;
      }
      return `auto dtoToJson(const ${def.name}& value) -> Json
    {
        Json out = Json::object();
${fields}
        return out;
    }`;
    })
    .join("\n\n    ");

  return `// GENERATED - do not edit.
// Produced by tools/gen-control-dto/gen.ts from control_dto.cppm.

module;

#include <nlohmann/json.hpp>

#include <charconv>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module Saffron.Control;

import Saffron.Core;
import Saffron.Json;

namespace se
{
    namespace
    {
        auto fieldValue(const Json& params, std::string_view key, std::size_t index, bool positional) -> const Json*
        {
            if (params.is_object())
            {
                auto it = params.find(std::string{ key });
                if (it != params.end())
                {
                    return &*it;
                }
                if (positional)
                {
                    auto args = params.find("args");
                    if (args != params.end() && args->is_array() && index < args->size())
                    {
                        return &(*args)[index];
                    }
                }
            }
            return nullptr;
        }

        auto optionalField(const Json& params, std::string_view key, std::size_t index, bool positional) -> const Json*
        {
            return fieldValue(params, key, index, positional);
        }

        auto requiredField(const Json& params, std::string_view key, std::size_t index, bool positional) -> Result<const Json*>
        {
            if (const Json* value = fieldValue(params, key, index, positional))
            {
                return value;
            }
            return Err(std::format("missing key '{}'", key));
        }

        auto readString(const Json& value, std::string_view key) -> Result<std::string>
        {
            if (value.is_string())
            {
                return value.get<std::string>();
            }
            return Err(std::format("key '{}' is not a string", key));
        }

        auto readBool(const Json& value, std::string_view key) -> Result<bool>
        {
            if (value.is_boolean())
            {
                return value.get<bool>();
            }
            if (value.is_number())
            {
                return value.get<f64>() != 0.0;
            }
            if (value.is_string())
            {
                const std::string text = value.get<std::string>();
                return !(text == "0" || text == "false" || text == "off");
            }
            return Err(std::format("key '{}' is not a boolean", key));
        }

        auto readF32(const Json& value, std::string_view key) -> Result<f32>
        {
            if (value.is_number())
            {
                return static_cast<f32>(value.get<f64>());
            }
            return Err(std::format("key '{}' is not a number", key));
        }

        auto readI32(const Json& value, std::string_view key) -> Result<i32>
        {
            if (!value.is_number_integer())
            {
                return Err(std::format("key '{}' is not an integer", key));
            }
            const i64 parsed = value.get<i64>();
            if (parsed < std::numeric_limits<i32>::min() || parsed > std::numeric_limits<i32>::max())
            {
                return Err(std::format("key '{}' is outside i32 range", key));
            }
            return static_cast<i32>(parsed);
        }

        auto readU32(const Json& value, std::string_view key) -> Result<u32>
        {
            if (value.is_number_unsigned())
            {
                const u64 parsed = value.get<u64>();
                if (parsed <= std::numeric_limits<u32>::max())
                {
                    return static_cast<u32>(parsed);
                }
            }
            else if (value.is_number_integer())
            {
                const i64 parsed = value.get<i64>();
                if (parsed >= 0 && parsed <= std::numeric_limits<u32>::max())
                {
                    return static_cast<u32>(parsed);
                }
            }
            return Err(std::format("key '{}' is not a u32", key));
        }

        auto readI64(const Json& value, std::string_view key) -> Result<i64>
        {
            if (!value.is_number_integer())
            {
                return Err(std::format("key '{}' is not an integer", key));
            }
            return value.get<i64>();
        }

        auto readU64(const Json& value, std::string_view key) -> Result<u64>
        {
            if (value.is_number_unsigned())
            {
                return value.get<u64>();
            }
            if (value.is_number_integer())
            {
                const i64 parsed = value.get<i64>();
                if (parsed >= 0)
                {
                    return static_cast<u64>(parsed);
                }
            }
            return Err(std::format("key '{}' is not a u64", key));
        }

        auto readWireUuid(const Json& value, std::string_view key) -> Result<WireUuid>
        {
            if (value.is_number_unsigned())
            {
                return WireUuid{ value.get<u64>() };
            }
            if (value.is_number_integer())
            {
                const i64 parsed = value.get<i64>();
                if (parsed >= 0)
                {
                    return WireUuid{ static_cast<u64>(parsed) };
                }
            }
            if (value.is_string())
            {
                const std::string text = value.get<std::string>();
                u64 parsed = 0;
                const std::from_chars_result fc = std::from_chars(text.data(), text.data() + text.size(), parsed);
                if (fc.ec == std::errc{} && fc.ptr == text.data() + text.size())
                {
                    return WireUuid{ parsed };
                }
            }
            return Err(std::format("key '{}' is not a uuid", key));
        }

        auto readEntitySelector(const Json& value, std::string_view) -> Result<EntitySelector>
        {
            return EntitySelector{ value };
        }

        auto readAssetSelector(const Json& value, std::string_view) -> Result<AssetSelector>
        {
            return AssetSelector{ value };
        }

        auto readJson(const Json& value, std::string_view) -> Result<Json>
        {
            return value;
        }

${emitEnumHelpers(enums)}

        template <typename T>
        auto readVector(const Json& value, std::string_view key) -> Result<std::vector<T>>
        {
            if (!value.is_array())
            {
                return Err(std::format("key '{}' is not an array", key));
            }
            std::vector<T> out;
            out.reserve(value.size());
            for (const Json& item : value)
            {
                auto parsed = parseDto(item, DtoTag<T>{});
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.push_back(std::move(*parsed));
            }
            return out;
        }

        template <>
        auto readVector<std::string>(const Json& value, std::string_view key) -> Result<std::vector<std::string>>
        {
            if (!value.is_array())
            {
                return Err(std::format("key '{}' is not an array", key));
            }
            std::vector<std::string> out;
            out.reserve(value.size());
            for (const Json& item : value)
            {
                auto parsed = readString(item, key);
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.push_back(std::move(*parsed));
            }
            return out;
        }

        template <typename T>
        auto dtoVectorToJson(const std::vector<T>& values) -> Json
        {
            Json out = Json::array();
            for (const T& value : values)
            {
                out.push_back(dtoToJson(value));
            }
            return out;
        }

        template <>
        auto dtoVectorToJson<std::string>(const std::vector<std::string>& values) -> Json
        {
            Json out = Json::array();
            for (const std::string& value : values)
            {
                out.push_back(value);
            }
            return out;
        }
    }

    auto dtoToJson(WireUuid value) -> Json
    {
        return uuidToJson(value.value);
    }

    ${enumJsonFns}

    ${parseFns}

    ${toJsonFns}
}
`;
}

function tsType(type: string): string {
  const optional = optionalInner(type);
  if (optional) {
    return tsType(optional);
  }
  const vector = vectorInner(type);
  if (vector) {
    return `${tsType(vector)}[]`;
  }
  switch (type) {
    case "bool":
      return "boolean";
    case "i32":
    case "u32":
    case "u64":
    case "i64":
    case "f32":
      return "number";
    case "WireUuid":
      return "WireUuid";
    case "EntitySelector":
      return "WireUuid | string | number";
    case "AssetSelector":
      return "WireUuid | string | number";
    case "std::string":
      return "string";
    case "Json":
      return "unknown";
    case "AddEntityPreset":
      return '"empty" | "cube" | "model" | "point-light" | "spot-light" | "directional-light" | "camera" | "reflection-probe"';
    case "PickKind":
      return '"billboard" | "mesh"';
    case "GizmoOpDto":
      return '"translate" | "rotate" | "scale"';
    case "GizmoSpaceDto":
      return '"world" | "local"';
    case "GizmoPointerPhase":
      return '"hover" | "begin" | "drag" | "end"';
    case "AaModeDto":
      return '"off" | "fxaa" | "taa" | "msaa2" | "msaa4" | "msaa8"';
    case "GiModeDto":
      return '"off" | "ddgi"';
    case "AssetSlotDto":
      return '"mesh" | "albedo" | "metallic-roughness"';
    case "ScreenshotTargetDto":
      return '"viewport" | "window"';
    case "AssetTypeDto":
      return '"mesh" | "texture" | "other"';
    case "ProfilerModeDto":
      return '"off" | "timestamps" | "pipeline-stats"';
    case "ProfileLaneDto":
      return '"cpu" | "gpu"';
    case "CaptureModeDto":
      return '"single" | "frames" | "rolling"';
    case "CaptureStateDto":
      return '"idle" | "arming" | "recording" | "ready"';
    case "AlarmSeverityDto":
      return '"info" | "warning" | "critical"';
    case "AlarmStateDto":
      return '"firing" | "resolved"';
    default:
      return type;
  }
}

function emitTs(structs: Map<string, StructDef>): string {
  const names = [
    ...new Set([
      "EntityRef",
      ...transitiveStructs(commandTypeNames(), structs),
      ...transitiveStructs(["Vec3", "Vec4", "ProbeRef"], structs),
    ]),
  ];
  const interfaces = names
    .map((name) => structs.get(name))
    .filter((def): def is StructDef => Boolean(def))
    .map((def) => {
      if (def.name === "EnvironmentDto") {
        return `export interface EnvironmentDto {
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
}`;
      }
      const body = def.fields
        .map((field) => {
          const optional = optionalInner(field.type);
          if (def.name === "InspectResult" && field.name === "components") {
            return "  components: Components;";
          }
          if (def.name === "SetComponentParams" && field.name === "json") {
            return "  json: ComponentBody;";
          }
          return `  ${field.name}${optional ? "?" : ""}: ${tsType(field.type)};`;
        })
        .join("\n");
      return `export interface ${def.name} {\n${body}\n}`;
    })
    .join("\n\n");
  const componentInterfaces = `export interface Name {
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
  | Record<string, unknown>;`;
  const paramsMap = commands
    .map((command) => `  ${JSON.stringify(command.name)}: ${command.params};`)
    .join("\n");
  const resultMap = commands
    .map((command) => `  ${JSON.stringify(command.name)}: ${command.result};`)
    .join("\n");
  return `/**
 * GENERATED - do not edit.
 *
 * Produced by tools/gen-control-dto/gen.ts from control_dto.cppm.
 */

export type WireUuid = string;

${componentInterfaces}

${interfaces}

export interface CommandParamsMap {
${paramsMap}
}

export interface CommandResultMap {
${resultMap}
}
`;
}

function jsonSchemaFor(type: string): Record<string, unknown> {
  const optional = optionalInner(type);
  if (optional) {
    return jsonSchemaFor(optional);
  }
  const vector = vectorInner(type);
  if (vector) {
    return { type: "array", items: jsonSchemaFor(vector) };
  }
  switch (type) {
    case "bool":
      return { type: "boolean" };
    case "i32":
    case "u32":
    case "u64":
    case "i64":
      return { type: "integer" };
    case "f32":
      return { type: "number" };
    case "WireUuid":
      return { type: "string" };
    case "EntitySelector":
      return { oneOf: [{ type: "string" }, { type: "integer" }] };
    case "AssetSelector":
      return { oneOf: [{ type: "string" }, { type: "integer" }] };
    case "std::string":
      return { type: "string" };
    case "Json":
      return {};
    default:
      if (enumWireNames.has(type)) {
        return { type: "string", enum: Object.values(enumWireNames.get(type)!) };
      }
      return { $ref: `#/components/schemas/${type}` };
  }
}

function schemaFor(def: StructDef): Record<string, unknown> {
  if (def.name === "EnvironmentDto") {
    return { $ref: "#/components/schemas/Environment" };
  }
  return {
    type: "object",
    additionalProperties: false,
    properties: Object.fromEntries(
      def.fields.map((field) => {
        const schema = jsonSchemaFor(field.type);
        if (def.name === "SelectionResult" && field.name === "entity") {
          return [field.name, { oneOf: [schema, { type: "null" }] }];
        }
        if (def.name === "InspectResult" && field.name === "components") {
          return [field.name, { $ref: "#/components/schemas/Components" }];
        }
        if (def.name === "SetComponentParams" && field.name === "json") {
          return [field.name, { $ref: "#/components/schemas/ComponentBody" }];
        }
        return [field.name, schema];
      }),
    ),
    required: def.fields.filter((field) => !optionalInner(field.type)).map((field) => field.name),
  };
}

function componentSchemas(): Record<string, unknown> {
  const vec3 = { $ref: "#/components/schemas/Vec3" };
  const vec4 = { $ref: "#/components/schemas/Vec4" };
  const uuid = { type: "string" };
  const componentNames = [
    "Name",
    "Transform",
    "Mesh",
    "Camera",
    "Material",
    "MaterialSet",
    "Script",
    "DirectionalLight",
    "PointLight",
    "SpotLight",
    "ReflectionProbe",
    "Relationship",
    "SkinnedMesh",
    "Bone",
  ];
  const schemas: Record<string, unknown> = {
    Name: {
      type: "object",
      additionalProperties: false,
      properties: { name: { type: "string" } },
      required: ["name"],
    },
    Transform: {
      type: "object",
      additionalProperties: false,
      properties: { translation: vec3, scale: vec3, rotation: vec3 },
      required: ["translation", "scale", "rotation"],
    },
    Mesh: {
      type: "object",
      additionalProperties: false,
      properties: { mesh: uuid },
      required: ["mesh"],
    },
    Camera: {
      type: "object",
      additionalProperties: false,
      properties: {
        fov: { type: "number" },
        near: { type: "number" },
        far: { type: "number" },
        primary: { type: "boolean" },
        showModel: { type: "boolean" },
        showFrustum: { type: "boolean" },
        frustumMaxDistance: { type: "number" },
      },
      required: ["fov", "near", "far", "primary", "showModel", "showFrustum", "frustumMaxDistance"],
    },
    Material: {
      type: "object",
      additionalProperties: false,
      properties: {
        baseColor: vec4,
        albedoTexture: uuid,
        metallicRoughnessTexture: uuid,
        metallic: { type: "number" },
        roughness: { type: "number" },
        emissive: vec3,
        emissiveStrength: { type: "number" },
        unlit: { type: "boolean" },
      },
      required: [
        "baseColor",
        "albedoTexture",
        "metallicRoughnessTexture",
        "metallic",
        "roughness",
        "emissive",
        "emissiveStrength",
        "unlit",
      ],
    },
    MaterialSet: {
      type: "object",
      additionalProperties: false,
      properties: { slots: { type: "array", items: { $ref: "#/components/schemas/Material" } } },
      required: ["slots"],
    },
    Script: {
      type: "object",
      additionalProperties: false,
      properties: {
        scripts: {
          type: "array",
          items: {
            type: "object",
            additionalProperties: false,
            properties: { scriptPath: { type: "string" }, overrides: { type: "object" } },
            required: ["scriptPath", "overrides"],
          },
        },
      },
      required: ["scripts"],
    },
    DirectionalLight: {
      type: "object",
      additionalProperties: false,
      properties: {
        direction: vec3,
        color: vec3,
        intensity: { type: "number" },
        ambient: { type: "number" },
      },
      required: ["direction", "color", "intensity", "ambient"],
    },
    PointLight: {
      type: "object",
      additionalProperties: false,
      properties: { color: vec3, intensity: { type: "number" }, range: { type: "number" } },
      required: ["color", "intensity", "range"],
    },
    SpotLight: {
      type: "object",
      additionalProperties: false,
      properties: {
        direction: vec3,
        color: vec3,
        intensity: { type: "number" },
        range: { type: "number" },
        innerAngle: { type: "number" },
        outerAngle: { type: "number" },
      },
      required: ["direction", "color", "intensity", "range", "innerAngle", "outerAngle"],
    },
    ReflectionProbe: {
      type: "object",
      additionalProperties: false,
      properties: {
        influenceRadius: { type: "number" },
        intensity: { type: "number" },
        boxProjection: { type: "boolean" },
        boxExtent: vec3,
      },
      required: ["influenceRadius", "intensity", "boxProjection", "boxExtent"],
    },
    Relationship: {
      type: "object",
      additionalProperties: false,
      properties: { parent: uuid },
      required: ["parent"],
    },
    SkinnedMesh: {
      type: "object",
      additionalProperties: false,
      properties: {
        mesh: uuid,
        rootBone: uuid,
        bones: { type: "array", items: uuid },
        inverseBind: {
          type: "array",
          items: { type: "array", items: { type: "number" }, minItems: 16, maxItems: 16 },
        },
      },
      required: ["mesh", "rootBone", "bones", "inverseBind"],
    },
    Bone: {
      type: "object",
      additionalProperties: false,
      properties: {},
    },
    AtmosphereSettingsDto: {
      type: "object",
      additionalProperties: false,
      properties: {
        enabled: { type: "boolean" },
        planetRadius: { type: "number" },
        atmosphereHeight: { type: "number" },
        rayleighScattering: vec3,
        rayleighScaleHeight: { type: "number" },
        mieScattering: { type: "number" },
        mieScaleHeight: { type: "number" },
        mieAnisotropy: { type: "number" },
        ozoneAbsorption: vec3,
        sunDiskAngularRadius: { type: "number" },
        sunDiskIntensity: { type: "number" },
      },
      required: [
        "enabled",
        "planetRadius",
        "atmosphereHeight",
        "rayleighScattering",
        "rayleighScaleHeight",
        "mieScattering",
        "mieScaleHeight",
        "mieAnisotropy",
        "ozoneAbsorption",
        "sunDiskAngularRadius",
        "sunDiskIntensity",
      ],
    },
  };
  schemas.Components = {
    type: "object",
    additionalProperties: false,
    properties: Object.fromEntries(
      componentNames.map((name) => [name, { $ref: `#/components/schemas/${name}` }]),
    ),
  };
  schemas.ComponentBody = {
    oneOf: componentNames.map((name) => ({ $ref: `#/components/schemas/${name}` })),
  };
  schemas.Environment = {
    type: "object",
    additionalProperties: false,
    properties: {
      skyMode: { type: "string", enum: ["color", "texture", "procedural"] },
      clearColor: vec3,
      skyTexture: uuid,
      skyIntensity: { type: "number" },
      skyRotation: { type: "number" },
      exposure: { type: "number" },
      visible: { type: "boolean" },
      useSkyForAmbient: { type: "boolean" },
      ambientColor: vec3,
      ambientIntensity: { type: "number" },
      atmosphere: { $ref: "#/components/schemas/AtmosphereSettingsDto" },
    },
    required: [
      "skyMode",
      "clearColor",
      "skyTexture",
      "skyIntensity",
      "skyRotation",
      "exposure",
      "visible",
      "useSkyForAmbient",
      "ambientColor",
      "ambientIntensity",
      "atmosphere",
    ],
  };
  return schemas;
}

function emitOpenRpc(structs: Map<string, StructDef>): string {
  const schemaNames = [...new Set([...structs.keys()].filter((name) => name !== "DtoTag"))].sort();
  const doc = {
    openrpc: "1.3.2",
    info: { title: "Saffron control DTOs", version: "0.2.0" },
    methods: commands.map((command) => ({
      name: command.name,
      summary: command.summary,
      params: [
        {
          name: "params",
          schema: { $ref: `#/components/schemas/${command.params}` },
        },
      ],
      result: {
        name: "result",
        schema: { $ref: `#/components/schemas/${command.result}` },
      },
    })),
    components: {
      schemas: {
        ...Object.fromEntries(schemaNames.map((name) => [name, schemaFor(structs.get(name)!)])),
        ...componentSchemas(),
      },
    },
  };
  return `${JSON.stringify(doc, null, 2)}\n`;
}

function emitManifest(): string {
  const manifestCommands: ManifestCommand[] = commands.map((command) => {
    const fixture = commandFixtures.get(command.name);
    const skip = commandSkips.get(command.name);
    if (!fixture && !skip) {
      throw new Error(`missing contract-test fixture or skip reason for command ${command.name}`);
    }
    return {
      name: command.name,
      params: command.params,
      result: command.result,
      status: "typed",
      ...(fixture ? { fixture } : {}),
      ...(skip ? { skip } : {}),
    };
  });
  return `${JSON.stringify(
    {
      generatedBy: "tools/gen-control-dto/gen.ts",
      commands: manifestCommands,
      skips: [{ name: "help", reason: "reflective registry" }],
    },
    null,
    2,
  )}\n`;
}

function emitSceneSerde(): string {
  return `// GENERATED - do not edit.
// Produced by tools/gen-control-dto/gen.ts from the scene component DTO catalog.

module;

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <format>
#include <string>

module Saffron.Scene;

import Saffron.Core;
import Saffron.Json;

namespace se
{
    namespace
    {
        auto skyModeName(SkyMode mode) -> const char*
        {
            switch (mode)
            {
                case SkyMode::Color: return "color";
                case SkyMode::Texture: return "texture";
                case SkyMode::Procedural: return "procedural";
            }
            return "procedural";
        }

        // A bare json value as u64: unsigned numbers directly, decimal strings parsed
        // (uuid arrays serialize as strings, like every id on the wire).
        auto u64FromJson(const nlohmann::json& value) -> u64
        {
            if (value.is_number_unsigned())
            {
                return value.get<u64>();
            }
            if (value.is_string())
            {
                const std::string text = value.get<std::string>();
                char* end = nullptr;
                const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
                if (end != text.c_str() && *end == '\\0')
                {
                    return parsed;
                }
            }
            return 0;
        }

        auto skyModeFromName(const std::string& name) -> SkyMode
        {
            if (name == "color") { return SkyMode::Color; }
            if (name == "texture") { return SkyMode::Texture; }
            if (name == "procedural") { return SkyMode::Procedural; }
            logWarn(std::format("unknown sky mode '{}', defaulting to procedural", name));
            return SkyMode::Procedural;
        }

        auto atmosphereToJson(const AtmosphereSettings& a) -> nlohmann::json
        {
            return nlohmann::json{
                { "enabled", a.enabled },
                { "planetRadius", a.planetRadius },
                { "atmosphereHeight", a.atmosphereHeight },
                { "rayleighScattering", vec3ToJson(a.rayleighScattering) },
                { "rayleighScaleHeight", a.rayleighScaleHeight },
                { "mieScattering", a.mieScattering },
                { "mieScaleHeight", a.mieScaleHeight },
                { "mieAnisotropy", a.mieAnisotropy },
                { "ozoneAbsorption", vec3ToJson(a.ozoneAbsorption) },
                { "sunDiskAngularRadius", a.sunDiskAngularRadius },
                { "sunDiskIntensity", a.sunDiskIntensity },
            };
        }

        auto atmosphereFromJson(const nlohmann::json& j) -> AtmosphereSettings
        {
            AtmosphereSettings a;
            if (!j.is_object())
            {
                return a;
            }
            a.enabled = jsonBoolOr(j, "enabled", false);
            a.planetRadius = jsonF32Or(j, "planetRadius", 6360.0f);
            a.atmosphereHeight = jsonF32Or(j, "atmosphereHeight", 100.0f);
            if (j.contains("rayleighScattering")) { a.rayleighScattering = vec3FromJson(j["rayleighScattering"]); }
            a.rayleighScaleHeight = jsonF32Or(j, "rayleighScaleHeight", 8.0f);
            a.mieScattering = jsonF32Or(j, "mieScattering", 3.996f);
            a.mieScaleHeight = jsonF32Or(j, "mieScaleHeight", 1.2f);
            a.mieAnisotropy = jsonF32Or(j, "mieAnisotropy", 0.8f);
            if (j.contains("ozoneAbsorption")) { a.ozoneAbsorption = vec3FromJson(j["ozoneAbsorption"]); }
            a.sunDiskAngularRadius = jsonF32Or(j, "sunDiskAngularRadius", 0.00465f);
            a.sunDiskIntensity = jsonF32Or(j, "sunDiskIntensity", 20.0f);
            return a;
        }
    }

    auto nameComponentToJson(const NameComponent& c) -> nlohmann::json
    {
        return nlohmann::json{ { "name", c.name } };
    }

    auto nameComponentFromJson(NameComponent& c, const nlohmann::json& j) -> Result<void>
    {
        c.name = jsonStringOr(j, "name", std::string{});
        return {};
    }

    auto transformComponentToJson(const TransformComponent& t) -> nlohmann::json
    {
        return nlohmann::json{ { "translation", vec3ToJson(t.translation) },
                               { "scale", vec3ToJson(t.scale) },
                               { "rotation", vec3ToJson(t.rotation) } };
    }

    auto transformComponentFromJson(TransformComponent& t, const nlohmann::json& j) -> Result<void>
    {
        t.translation = vec3FromJson(j.value("translation", nlohmann::json::object()));
        t.scale = vec3FromJson(j.value("scale", nlohmann::json::object()));
        t.rotation = vec3FromJson(j.value("rotation", nlohmann::json::object()));
        return {};
    }

    auto meshComponentToJson(const MeshComponent& c) -> nlohmann::json
    {
        return nlohmann::json{ { "mesh", uuidToJson(c.mesh.value) } };
    }

    auto meshComponentFromJson(MeshComponent& c, const nlohmann::json& j) -> Result<void>
    {
        c.mesh = Uuid{ jsonU64Or(j, "mesh", 0) };
        return {};
    }

    auto cameraComponentToJson(const CameraComponent& c) -> nlohmann::json
    {
        return nlohmann::json{ { "fov", c.fov }, { "near", c.nearPlane },
                               { "far", c.farPlane }, { "primary", c.primary },
                               { "showModel", c.showModel }, { "showFrustum", c.showFrustum },
                               { "frustumMaxDistance", c.frustumMaxDistance } };
    }

    auto cameraComponentFromJson(CameraComponent& c, const nlohmann::json& j) -> Result<void>
    {
        c.fov = jsonF32Or(j, "fov", 45.0f);
        c.nearPlane = jsonF32Or(j, "near", 0.1f);
        c.farPlane = jsonF32Or(j, "far", 100.0f);
        c.primary = jsonBoolOr(j, "primary", true);
        c.showModel = jsonBoolOr(j, "showModel", true);
        c.showFrustum = jsonBoolOr(j, "showFrustum", true);
        c.frustumMaxDistance = jsonF32Or(j, "frustumMaxDistance", 25.0f);
        return {};
    }

    auto materialComponentToJson(const MaterialComponent& c) -> nlohmann::json
    {
        return nlohmann::json{ { "baseColor", vec4ToJson(c.baseColor) },
                               { "albedoTexture", uuidToJson(c.albedoTexture.value) },
                               { "metallicRoughnessTexture", uuidToJson(c.metallicRoughnessTexture.value) },
                               { "metallic", c.metallic },
                               { "roughness", c.roughness },
                               { "emissive", vec3ToJson(c.emissive) },
                               { "emissiveStrength", c.emissiveStrength },
                               { "unlit", c.unlit } };
    }

    auto materialComponentFromJson(MaterialComponent& c, const nlohmann::json& j) -> Result<void>
    {
        c.baseColor = vec4FromJson(j.value("baseColor", nlohmann::json::object()));
        c.albedoTexture = Uuid{ jsonU64Or(j, "albedoTexture", 0) };
        c.metallicRoughnessTexture = Uuid{ jsonU64Or(j, "metallicRoughnessTexture", 0) };
        c.metallic = jsonF32Or(j, "metallic", 0.0f);
        c.roughness = jsonF32Or(j, "roughness", 1.0f);
        c.emissive = vec3FromJson(j.value("emissive", nlohmann::json::object()));
        c.emissiveStrength = jsonF32Or(j, "emissiveStrength", 1.0f);
        c.unlit = jsonBoolOr(j, "unlit", false);
        return {};
    }

    auto materialSetComponentToJson(const MaterialSetComponent& c) -> nlohmann::json
    {
        nlohmann::json slots = nlohmann::json::array();
        for (const MaterialSlot& s : c.slots)
        {
            slots.push_back(nlohmann::json{ { "baseColor", vec4ToJson(s.baseColor) },
                                            { "albedoTexture", uuidToJson(s.albedoTexture.value) },
                                            { "metallicRoughnessTexture", uuidToJson(s.metallicRoughnessTexture.value) },
                                            { "metallic", s.metallic },
                                            { "roughness", s.roughness },
                                            { "emissive", vec3ToJson(s.emissive) },
                                            { "emissiveStrength", s.emissiveStrength },
                                            { "unlit", s.unlit } });
        }
        return nlohmann::json{ { "slots", std::move(slots) } };
    }

    auto materialSetComponentFromJson(MaterialSetComponent& c, const nlohmann::json& j) -> Result<void>
    {
        c.slots.clear();
        if (auto it = j.find("slots"); it != j.end() && it->is_array())
        {
            for (const nlohmann::json& sj : *it)
            {
                MaterialSlot s;
                s.baseColor = vec4FromJson(sj.value("baseColor", nlohmann::json::object()));
                s.albedoTexture = Uuid{ jsonU64Or(sj, "albedoTexture", 0) };
                s.metallicRoughnessTexture = Uuid{ jsonU64Or(sj, "metallicRoughnessTexture", 0) };
                s.metallic = jsonF32Or(sj, "metallic", 0.0f);
                s.roughness = jsonF32Or(sj, "roughness", 1.0f);
                s.emissive = vec3FromJson(sj.value("emissive", nlohmann::json::object()));
                s.emissiveStrength = jsonF32Or(sj, "emissiveStrength", 1.0f);
                s.unlit = jsonBoolOr(sj, "unlit", false);
                c.slots.push_back(s);
            }
        }
        return {};
    }

    auto scriptComponentToJson(const ScriptComponent& c) -> nlohmann::json
    {
        nlohmann::json scripts = nlohmann::json::array();
        for (const ScriptSlot& s : c.scripts)
        {
            scripts.push_back(nlohmann::json{ { "scriptPath", s.scriptPath }, { "overrides", s.overrides } });
        }
        return nlohmann::json{ { "scripts", std::move(scripts) } };
    }

    auto scriptComponentFromJson(ScriptComponent& c, const nlohmann::json& j) -> Result<void>
    {
        c.scripts.clear();
        if (auto it = j.find("scripts"); it != j.end() && it->is_array())
        {
            for (const nlohmann::json& sj : *it)
            {
                ScriptSlot s;
                s.scriptPath = jsonStringOr(sj, "scriptPath", std::string{});
                s.overrides = sj.value("overrides", nlohmann::json::object());
                if (!s.overrides.is_object())
                {
                    s.overrides = nlohmann::json::object();
                }
                c.scripts.push_back(std::move(s));
            }
        }
        return {};
    }

    auto directionalLightComponentToJson(const DirectionalLightComponent& c) -> nlohmann::json
    {
        return nlohmann::json{ { "direction", vec3ToJson(c.direction) },
                               { "color", vec3ToJson(c.color) },
                               { "intensity", c.intensity }, { "ambient", c.ambient } };
    }

    auto directionalLightComponentFromJson(DirectionalLightComponent& c, const nlohmann::json& j) -> Result<void>
    {
        c.direction = vec3FromJson(j.value("direction", nlohmann::json::object()));
        c.color = vec3FromJson(j.value("color", nlohmann::json::object()));
        c.intensity = jsonF32Or(j, "intensity", 1.0f);
        c.ambient = jsonF32Or(j, "ambient", 0.15f);
        return {};
    }

    auto pointLightComponentToJson(const PointLightComponent& c) -> nlohmann::json
    {
        return nlohmann::json{ { "color", vec3ToJson(c.color) },
                               { "intensity", c.intensity }, { "range", c.range } };
    }

    auto pointLightComponentFromJson(PointLightComponent& c, const nlohmann::json& j) -> Result<void>
    {
        c.color = vec3FromJson(j.value("color", nlohmann::json::object()));
        c.intensity = jsonF32Or(j, "intensity", 5.0f);
        c.range = jsonF32Or(j, "range", 10.0f);
        return {};
    }

    auto spotLightComponentToJson(const SpotLightComponent& c) -> nlohmann::json
    {
        return nlohmann::json{ { "direction", vec3ToJson(c.direction) },
                               { "color", vec3ToJson(c.color) }, { "intensity", c.intensity },
                               { "range", c.range }, { "innerAngle", c.innerAngle },
                               { "outerAngle", c.outerAngle } };
    }

    auto spotLightComponentFromJson(SpotLightComponent& c, const nlohmann::json& j) -> Result<void>
    {
        c.direction = vec3FromJson(j.value("direction", nlohmann::json::object()));
        c.color = vec3FromJson(j.value("color", nlohmann::json::object()));
        c.intensity = jsonF32Or(j, "intensity", 5.0f);
        c.range = jsonF32Or(j, "range", 10.0f);
        c.innerAngle = jsonF32Or(j, "innerAngle", 20.0f);
        c.outerAngle = jsonF32Or(j, "outerAngle", 30.0f);
        return {};
    }

    auto reflectionProbeComponentToJson(const ReflectionProbeComponent& c) -> nlohmann::json
    {
        return nlohmann::json{ { "influenceRadius", c.influenceRadius },
                               { "intensity", c.intensity },
                               { "boxProjection", c.boxProjection },
                               { "boxExtent", vec3ToJson(c.boxExtent) } };
    }

    auto reflectionProbeComponentFromJson(ReflectionProbeComponent& c, const nlohmann::json& j) -> Result<void>
    {
        c.influenceRadius = jsonF32Or(j, "influenceRadius", 10.0f);
        c.intensity = jsonF32Or(j, "intensity", 1.0f);
        c.boxProjection = jsonBoolOr(j, "boxProjection", false);
        c.boxExtent = vec3FromJson(j.value("boxExtent", nlohmann::json::object()));
        c.dirty = true;
        return {};
    }

    auto relationshipComponentToJson(const RelationshipComponent& c) -> nlohmann::json
    {
        return nlohmann::json{ { "parent", uuidToJson(c.parent.value) } };
    }

    auto relationshipComponentFromJson(RelationshipComponent& c, const nlohmann::json& j) -> Result<void>
    {
        c.parent = Uuid{ jsonU64Or(j, "parent", 0) };
        return {};
    }

    auto boneComponentToJson(const BoneComponent&) -> nlohmann::json
    {
        return nlohmann::json::object();
    }

    auto boneComponentFromJson(BoneComponent&, const nlohmann::json&) -> Result<void>
    {
        return {};
    }

    auto skinnedMeshComponentToJson(const SkinnedMeshComponent& c) -> nlohmann::json
    {
        nlohmann::json bones = nlohmann::json::array();
        for (const Uuid& bone : c.bones)
        {
            bones.push_back(uuidToJson(bone.value));
        }
        nlohmann::json inverseBind = nlohmann::json::array();
        for (const glm::mat4& m : c.inverseBind)
        {
            nlohmann::json mat = nlohmann::json::array();
            const float* p = &m[0][0];
            for (int i = 0; i < 16; i = i + 1)
            {
                mat.push_back(p[i]);
            }
            inverseBind.push_back(std::move(mat));
        }
        return nlohmann::json{ { "mesh", uuidToJson(c.mesh.value) },
                               { "rootBone", uuidToJson(c.rootBone.value) },
                               { "bones", std::move(bones) },
                               { "inverseBind", std::move(inverseBind) } };
    }

    auto skinnedMeshComponentFromJson(SkinnedMeshComponent& c, const nlohmann::json& j) -> Result<void>
    {
        c.mesh = Uuid{ jsonU64Or(j, "mesh", 0) };
        c.rootBone = Uuid{ jsonU64Or(j, "rootBone", 0) };
        c.bones.clear();
        if (j.contains("bones") && j["bones"].is_array())
        {
            for (const nlohmann::json& bone : j["bones"])
            {
                c.bones.push_back(Uuid{ u64FromJson(bone) });
            }
        }
        c.inverseBind.clear();
        if (j.contains("inverseBind") && j["inverseBind"].is_array())
        {
            for (const nlohmann::json& mat : j["inverseBind"])
            {
                glm::mat4 m{ 1.0f };
                if (mat.is_array() && mat.size() == 16)
                {
                    float* p = &m[0][0];
                    for (int i = 0; i < 16; i = i + 1)
                    {
                        if (mat[i].is_number())
                        {
                            p[i] = mat[i].get<float>();
                        }
                    }
                }
                c.inverseBind.push_back(m);
            }
        }
        c.boneHandles.clear();  // resolved cache — relinkHierarchy rebuilds it
        return {};
    }

    auto environmentToJson(const SceneEnvironment& env) -> nlohmann::json
    {
        return nlohmann::json{
            { "skyMode", skyModeName(env.skyMode) },
            { "clearColor", vec3ToJson(env.clearColor) },
            { "skyTexture", uuidToJson(env.skyTexture.value) },
            { "skyIntensity", env.skyIntensity },
            { "skyRotation", env.skyRotation },
            { "exposure", env.exposure },
            { "visible", env.visible },
            { "useSkyForAmbient", env.useSkyForAmbient },
            { "ambientColor", vec3ToJson(env.ambientColor) },
            { "ambientIntensity", env.ambientIntensity },
            { "atmosphere", atmosphereToJson(env.atmosphere) },
        };
    }

    auto environmentFromJson(const nlohmann::json& j) -> SceneEnvironment
    {
        SceneEnvironment env;
        if (!j.is_object())
        {
            return env;
        }
        env.skyMode = skyModeFromName(jsonStringOr(j, "skyMode", "procedural"));
        if (j.contains("clearColor")) { env.clearColor = vec3FromJson(j["clearColor"]); }
        env.skyTexture = Uuid{ jsonU64Or(j, "skyTexture", 0) };
        env.skyIntensity = jsonF32Or(j, "skyIntensity", 1.0f);
        env.skyRotation = jsonF32Or(j, "skyRotation", 0.0f);
        env.exposure = jsonF32Or(j, "exposure", 1.0f);
        env.visible = jsonBoolOr(j, "visible", true);
        env.useSkyForAmbient = jsonBoolOr(j, "useSkyForAmbient", true);
        if (j.contains("ambientColor")) { env.ambientColor = vec3FromJson(j["ambientColor"]); }
        env.ambientIntensity = jsonF32Or(j, "ambientIntensity", 0.15f);
        if (j.contains("atmosphere")) { env.atmosphere = atmosphereFromJson(j["atmosphere"]); }
        return env;
    }
}
`;
}

async function main(): Promise<void> {
  const text = await readFile(dtoFile, "utf8");
  const structs = parseStructs(text);
  structs.delete("DtoTag");
  const enums = parseEnums(text);
  for (const def of structs.values()) {
    for (const field of def.fields) {
      validateType(field.type, structs, enums);
    }
  }
  for (const command of commands) {
    if (!structs.has(command.params)) {
      throw new Error(`missing params DTO ${command.params} for ${command.name}`);
    }
    if (!structs.has(command.result)) {
      throw new Error(`missing result DTO ${command.result} for ${command.name}`);
    }
  }
  await mkdir(dirname(cppOut), { recursive: true });
  await mkdir(dirname(sceneSerdeOut), { recursive: true });
  await mkdir(dirname(tsOut), { recursive: true });
  await mkdir(dirname(openRpcOut), { recursive: true });
  await writeFile(cppOut, emitCpp(structs, enums), "utf8");
  await writeFile(sceneSerdeOut, emitSceneSerde(), "utf8");
  await writeFile(tsOut, emitTs(structs), "utf8");
  await writeFile(openRpcOut, emitOpenRpc(structs), "utf8");
  await writeFile(manifestOut, emitManifest(), "utf8");
  console.log(`wrote ${cppOut}`);
  console.log(`wrote ${sceneSerdeOut}`);
  console.log(`wrote ${tsOut}`);
  console.log(`wrote ${openRpcOut}`);
  console.log(`wrote ${manifestOut}`);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
