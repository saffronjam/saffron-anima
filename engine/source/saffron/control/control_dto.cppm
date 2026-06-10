module;

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

export module Saffron.Control:Dto;

import Saffron.Core;
import Saffron.Json;

export namespace se
{
    template <typename T>
    struct DtoTag
    {
    };

    struct WireUuid
    {
        u64 value;
    };

    struct EntitySelector
    {
        Json value;
    };

    struct AssetSelector
    {
        Json value;
    };

    struct EntityRef
    {
        WireUuid id;
        std::string name;
    };

    struct Vec3
    {
        f32 x;
        f32 y;
        f32 z;
    };

    struct Vec4
    {
        f32 x;
        f32 y;
        f32 z;
        f32 w;
    };

    enum class AddEntityPreset
    {
        Empty,
        Cube,
        Model,
        PointLight,
        SpotLight,
        DirectionalLight,
        Camera,
        ReflectionProbe,
    };

    enum class PickKind
    {
        Billboard,
        Mesh,
    };

    enum class GizmoOpDto
    {
        Translate,
        Rotate,
        Scale,
    };

    enum class GizmoSpaceDto
    {
        World,
        Local,
    };

    enum class GizmoPointerPhase
    {
        Hover,
        Begin,
        Drag,
        End,
    };

    enum class AaModeDto
    {
        Off,
        Fxaa,
        Taa,
        Msaa2,
        Msaa4,
        Msaa8,
    };

    enum class GiModeDto
    {
        Off,
        Ddgi,
    };

    enum class AssetSlotDto
    {
        Mesh,
        Albedo,
        MetallicRoughness,
    };

    enum class ScreenshotTargetDto
    {
        Viewport,
        Window,
    };

    enum class AssetTypeDto
    {
        Mesh,
        Texture,
        Other,
    };

    enum class ProfilerModeDto
    {
        Off,
        Timestamps,
        PipelineStats,
    };

    enum class ProfileLaneDto
    {
        Cpu,
        Gpu,
    };

    enum class CaptureModeDto
    {
        Single,
        Frames,
        Rolling,
    };

    enum class CaptureStateDto
    {
        Idle,
        Arming,
        Recording,
        Ready,
    };

    enum class AlarmSeverityDto
    {
        Info,
        Warning,
        Critical,
    };

    enum class AlarmStateDto
    {
        Firing,
        Resolved,
    };

    struct PingParams
    {
    };

    struct EmptyParams
    {
    };

    struct PingResult
    {
        bool pong;
        std::string engine;
        std::string version;
        i32 pid;
    };

    struct RenderStatsDto
    {
        i32 drawCalls;
        i32 batches;
        i32 instances;
        f32 frameMs;
        f32 fps;
        f32 gpuMs;
        f32 cpuFrameMs;
        f32 gpuFrameMs;
        f32 cpuWaitMs;
        i32 triangles;
        i32 descriptorBinds;
        i32 commandBuffers;
        i32 queueSubmits;
        i32 pipelinesCreated;
        u64 vramUsageBytes;
        u64 vramBudgetBytes;
        bool softwareGpu;
        ProfilerModeDto profilerMode;
        bool clustered;
        bool depthPrepass;
        bool shadows;
        bool ibl;
        bool ssao;
        bool contactShadows;
        bool ssgi;
        bool ddgi;
        bool rtSupported;
        bool rtShadows;
        bool restir;
        i32 blasCount;
        i32 pipelines;
        bool hdr;
        f32 exposureEv;
        AaModeDto aa;
    };

    struct RenderPassTimingDto
    {
        std::string name;
        f32 gpuMs;
    };

    struct RenderPassTimingsDto
    {
        std::vector<RenderPassTimingDto> passes;
        f32 gpuTotalMs;
        bool softwareGpu;
        ProfilerModeDto profilerMode;
    };

    struct ProfilerSetModeParams
    {
        std::optional<ProfilerModeDto> mode;
    };

    struct ProfilerModeResult
    {
        ProfilerModeDto mode;
        bool timestampsSupported;
        bool pipelineStatsSupported;
        bool softwareGpu;
    };

    // Raw pipeline-statistics counts for one pass; the editor derives overdraw
    // (fragmentInvocations / pixels), culling (clippingPrimitives / clippingInvocations), and
    // vertex reuse (vertexInvocations / inputVertices). Present only for PipelineStats captures.
    struct PipelineStatsDto
    {
        u64 inputVertices;
        u64 vertexInvocations;
        u64 clippingInvocations;
        u64 clippingPrimitives;
        u64 fragmentInvocations;
        u64 computeInvocations;
        u64 pixels;
    };

    struct ProfileSpanDto
    {
        std::string name;
        ProfileLaneDto lane;
        u64 startNs;
        u64 endNs;
        i32 parentIndex;
        u32 depth;
        std::optional<PipelineStatsDto> pipelineStats;
    };

    struct ProfileCaptureMetadataDto
    {
        bool softwareGpu;
        bool correlated;
        std::string deviceName;
        f32 timestampPeriod;
        f32 targetFps;
        ProfilerModeDto mode;
        std::string filter;
        u32 frameCount;
    };

    struct ProfileCaptureDto
    {
        std::vector<ProfileSpanDto> spans;
        ProfileCaptureMetadataDto metadata;
    };

    struct CaptureStartParams
    {
        std::optional<CaptureModeDto> mode;
        std::optional<i32> frames;
        std::optional<std::string> filter;
        std::optional<bool> includeCpu;
        std::optional<bool> includePipelineStats;
    };

    struct CaptureStartResult
    {
        u32 captureId;
        bool ack;
    };

    // A finished capture. A small `single` capture comes back inline (`capture` + `chromeTrace`);
    // a multi-frame capture is written to `path` (`chromeTrace`/`capture` empty) and `pending`
    // marks a deferred write. `ready` is false when no capture was armed.
    struct CaptureStopResult
    {
        bool ready;
        CaptureModeDto mode;
        u32 frameCount;
        bool inlined;
        ProfileCaptureDto capture;
        std::string chromeTrace;
        std::string path;
        bool pending;
    };

    // Non-destructive capture progress: lets the editor poll the recorder for the live frame
    // count and drain (capture-stop) only once `state` reaches `ready`.
    struct CaptureStatusResult
    {
        CaptureStateDto state;
        u32 capturedFrames;
        u32 targetFrames;
        CaptureModeDto mode;
        bool pipelineStatsSupported;
    };

    struct FrameSampleDto
    {
        i64 frameIndex;
        f32 cpuMs;
        f32 gpuMs;
        f32 cpuWaitMs;
    };

    struct FrameHistoryParams
    {
        std::optional<i32> samples;
    };

    struct FrameHistoryDto
    {
        f32 p50Ms;
        f32 p95Ms;
        f32 p99Ms;
        f32 p999Ms;
        f32 maxMs;
        f32 meanMs;
        f32 stddevMs;
        i64 stutterCount;
        i32 sampleCount;
        f32 budgetMs;
        std::vector<FrameSampleDto> samples;
    };

    struct PerfConfigDto
    {
        f32 targetFps;
        f32 budgetMs;
        f32 greenBudgetFrac;
        f32 greenMedianMul;
        f32 amberMedianMul;
        f32 frozenMs;
        f32 vramWarnFrac;
        f32 vramCritFrac;
    };

    struct SetPerfConfigParams
    {
        std::optional<f32> targetFps;
        std::optional<f32> greenBudgetFrac;
        std::optional<f32> greenMedianMul;
        std::optional<f32> amberMedianMul;
        std::optional<f32> frozenMs;
        std::optional<f32> vramWarnFrac;
        std::optional<f32> vramCritFrac;
    };

    struct AlarmEventDto
    {
        i64 seq;
        std::string fingerprint;
        std::string metric;
        std::string pass;
        AlarmSeverityDto severity;
        AlarmStateDto state;
        f32 value;
        f32 threshold;
        i64 sinceFrame;
        i32 count;
        f32 durationMs;
    };

    struct DrainAlarmsParams
    {
        std::optional<i64> since;
    };

    struct DrainAlarmsResult
    {
        std::vector<AlarmEventDto> events;
        i64 highWaterSeq;
        i64 oldestSeq;
        bool overflowed;
    };

    struct ScriptStatusResult
    {
        std::string state;   // "edit" | "playing" | "paused"
        i32 instances;       // live script instances (0 in edit)
        i64 errorHighWater;  // last assigned script-error seq
    };

    struct ScriptErrorDto
    {
        i64 seq;
        WireUuid entity;  // 0 when the failure has no owning entity
        std::string script;
        std::string message;  // the Lua error + traceback
        i64 tick;             // the play tick the error fired on
    };

    struct DrainScriptErrorsParams
    {
        std::optional<i64> since;
    };

    struct DrainScriptErrorsResult
    {
        std::vector<ScriptErrorDto> events;
        i64 highWaterSeq;
        i64 oldestSeq;
        bool overflowed;
    };

    struct GetScriptSchemaParams
    {
        std::string path;  // relative to the project src/, e.g. "turret.lua"
    };

    struct ScriptFieldDto
    {
        std::string name;
        std::string type;  // "number" | "bool" | "string" | "vec3"
        Json defaultValue;
    };

    struct GetScriptSchemaResult
    {
        std::vector<ScriptFieldDto> fields;
    };

    struct SetScriptOverrideParams
    {
        EntitySelector entity;
        i32 slot;          // index into the ScriptComponent slot list
        std::string name;  // the declared field name
        Json value;        // null clears the override (the default applies again)
    };

    struct SetScriptOverrideResult
    {
        std::string scriptPath;
        Json overrides;  // the slot's overrides after the write
    };

    struct CreateScriptParams
    {
        std::string name;  // src/-relative; ".lua" appended when missing
    };

    struct CreateScriptResult
    {
        std::string path;  // the src/-relative path a ScriptSlot stores
    };

    struct ActiveAlarmDto
    {
        std::string fingerprint;
        std::string metric;
        std::string pass;
        AlarmSeverityDto severity;
        f32 value;
        f32 threshold;
        i64 sinceFrame;
        i32 count;
    };

    struct ActiveAlarmsDto
    {
        std::vector<ActiveAlarmDto> alarms;
    };

    struct SetAaParams
    {
        std::optional<AaModeDto> mode;
    };

    struct SetAaResult
    {
        AaModeDto aa;
    };

    struct ToggleParams
    {
        std::optional<bool> enabled;
    };

    struct SetClusteredResult
    {
        bool clustered;
    };

    struct SetIblResult
    {
        bool ibl;
    };

    struct SetSsaoResult
    {
        bool ssao;
    };

    struct SetContactShadowsResult
    {
        bool contactShadows;
    };

    struct SetSsgiResult
    {
        bool ssgi;
    };

    struct SetRtShadowsResult
    {
        bool rtShadows;
    };

    struct SetRestirResult
    {
        bool restir;
    };

    struct SetGiParams
    {
        GiModeDto mode;
    };

    struct SetGiResult
    {
        bool ddgi;
    };

    struct SetShadowsResult
    {
        bool shadows;
    };

    struct SetSkinningResult
    {
        bool skinning;
    };

    struct SetDepthPrepassResult
    {
        bool depthPrepass;
    };

    struct ViewportNativeInfoResult
    {
        std::string platform;
        std::string transport;
        std::string status;
        std::string controlSocket;
        i32 width;
        i32 height;
        std::string message;
    };

    struct SetViewportSizeParams
    {
        std::optional<i32> width;
        std::optional<i32> height;
    };

    struct SetViewportSizeResult
    {
        i32 width;
        i32 height;
    };

    struct ProjectInfoDto
    {
        bool loaded;
        std::string root;
        std::string path;
        std::string name;
        std::string displayName;
    };

    struct NewProjectParams
    {
        std::optional<std::string> name;
        std::optional<std::string> displayName;
        std::optional<std::string> root;
    };

    struct PathParams
    {
        std::string path;
    };

    struct OptionalPathParams
    {
        std::optional<std::string> path;
    };

    struct ImportModelResult
    {
        WireUuid id;
        std::string name;
        WireUuid mesh;
        WireUuid albedoTexture;
    };

    struct ImportTextureResult
    {
        WireUuid texture;
    };

    struct AssetEntryDto
    {
        WireUuid id;
        std::string name;
        AssetTypeDto type;
        std::string path;
        std::optional<std::string> folder;
    };

    struct AssetList
    {
        std::vector<AssetEntryDto> assets;
        std::vector<std::string> folders;
    };

    struct RenameAssetParams
    {
        AssetSelector asset;
        std::string name;
    };

    struct AssetRef
    {
        WireUuid id;
        std::string name;
        std::optional<std::string> folder;
    };

    struct CreateAssetFolderParams
    {
        std::string folder;
    };

    struct RenameAssetFolderParams
    {
        std::string folder;
        std::string name;
    };

    struct DeleteAssetFolderParams
    {
        std::string folder;
    };

    struct MoveAssetParams
    {
        AssetSelector asset;
        std::optional<std::string> folder;
    };

    struct AssetUsagesParams
    {
        AssetSelector asset;
    };

    struct AssetUsageDto
    {
        std::optional<WireUuid> entity;
        std::optional<std::string> entityName;
        std::string slot;
    };

    struct AssetUsagesResult
    {
        std::vector<AssetUsageDto> usages;
    };

    struct AssetMetadataParams
    {
        AssetSelector asset;
    };

    struct AssetMetadataDto
    {
        WireUuid id;
        std::string name;
        AssetTypeDto type;
        std::string path;
        std::optional<std::string> folder;
        u64 sizeBytes;
        std::optional<u32> vertexCount;
        std::optional<u32> triangleCount;
        i64 createdAt;
    };

    struct DeleteAssetParams
    {
        AssetSelector asset;
    };

    struct DeleteAssetResult
    {
        WireUuid id;
        std::string name;
        std::vector<AssetUsageDto> cleared;
        bool fileDeleted;
    };

    struct AssignAssetParams
    {
        EntitySelector entity;
        AssetSlotDto slot;
        AssetSelector asset;
    };

    struct AssignAssetResult
    {
        WireUuid id;
        std::string name;
        AssetSlotDto slot;
    };

    struct PathResult
    {
        std::string path;
    };

    struct ScreenshotParams
    {
        std::optional<ScreenshotTargetDto> target;
        std::string path;
    };

    struct ScreenshotResult
    {
        ScreenshotTargetDto target;
        std::string path;
        bool pending;
    };

    struct ThumbnailParams
    {
        AssetSelector asset;
        std::optional<i32> size;
    };

    struct ThumbnailResult
    {
        WireUuid id;
        std::string format;
        i32 width;
        i32 height;
        std::string base64;
    };

    struct QuitResult
    {
        bool quitting;
    };

    struct CreateEntityParams
    {
        std::string name;
    };

    struct EntityParams
    {
        EntitySelector entity;
    };

    struct SetParentParams
    {
        EntitySelector entity;
        std::optional<EntitySelector> parent;
    };

    struct DestroyEntityResult
    {
        WireUuid destroyed;
    };

    struct EntityListEntry
    {
        WireUuid id;
        std::string name;
        std::optional<WireUuid> parentId;
        std::optional<bool> bone;
    };

    struct EntityList
    {
        std::vector<EntityListEntry> entities;
    };

    struct ComponentList
    {
        std::vector<std::string> components;
    };

    struct ComponentParams
    {
        EntitySelector entity;
        std::string component;
    };

    struct AddComponentResult
    {
        std::string added;
    };

    struct RemoveComponentResult
    {
        std::string removed;
    };

    struct SetComponentParams
    {
        EntitySelector entity;
        std::string component;
        Json json;
    };

    struct SetComponentResult
    {
        std::string set;
    };

    struct SetTransformParams
    {
        EntitySelector entity;
        std::optional<Vec3> translation;
        std::optional<Vec3> rotation;
        std::optional<Vec3> scale;
        /// Animate the fields toward the given values over ~25ms instead of snapping
        /// (ignored when preserve-children must rebase the subtree on each write).
        std::optional<bool> smooth;
    };

    struct SetMaterialParams
    {
        EntitySelector entity;
        std::optional<Vec4> baseColor;
        std::optional<WireUuid> albedoTexture;
        std::optional<WireUuid> metallicRoughnessTexture;
        std::optional<f32> metallic;
        std::optional<f32> roughness;
        std::optional<Vec3> emissive;
        std::optional<f32> emissiveStrength;
        std::optional<bool> unlit;
        /// Target a slot of the entity's MaterialSetComponent instead of its
        /// MaterialComponent. Out of range is an error; ignored without a MaterialSet.
        std::optional<u32> slot;
        /// Animate numeric fields toward the given values over ~25ms instead of
        /// snapping; texture/unlit still apply immediately.
        std::optional<bool> smooth;
    };

    struct SetLightParams
    {
        std::optional<EntitySelector> entity;
        std::optional<Vec3> direction;
        std::optional<Vec3> color;
        std::optional<f32> intensity;
        std::optional<f32> ambient;
    };

    struct PickParams
    {
        std::optional<f32> u;
        std::optional<f32> v;
    };

    struct PickResult
    {
        bool hit;
        std::optional<WireUuid> id;
        std::optional<std::string> name;
        std::optional<PickKind> kind;
    };

    struct InspectResult
    {
        WireUuid id;
        std::string name;
        Json components;
    };

    struct EnvironmentDto
    {
        Json value;
    };

    struct SetEnvironmentParams
    {
        std::optional<Json> json;
        std::optional<std::string> skyMode;
        std::optional<Vec3> clearColor;
        std::optional<WireUuid> skyTexture;
        std::optional<f32> skyIntensity;
        std::optional<f32> skyRotation;
        std::optional<f32> exposure;
        std::optional<bool> visible;
        std::optional<bool> useSkyForAmbient;
        std::optional<Vec3> ambientColor;
        std::optional<f32> ambientIntensity;
    };

    struct SetAtmosphereParams
    {
        std::optional<Json> json;
        std::optional<bool> enabled;
        std::optional<f32> planetRadius;
        std::optional<f32> atmosphereHeight;
        std::optional<Vec3> rayleighScattering;
        std::optional<f32> rayleighScaleHeight;
        std::optional<f32> mieScattering;
        std::optional<f32> mieScaleHeight;
        std::optional<f32> mieAnisotropy;
        std::optional<Vec3> ozoneAbsorption;
        std::optional<f32> sunDiskAngularRadius;
        std::optional<f32> sunDiskIntensity;
    };

    struct SelectionResult
    {
        i32 selectionVersion;
        i32 sceneVersion;
        std::optional<EntityRef> entity;
        std::string playState;
        i32 playVersion;
    };

    struct PlayStateResult
    {
        std::string state;  // "edit" | "playing" | "paused"
        i32 playVersion;
        i32 sceneVersion;       // echoed so a stop reads as a scene change
        bool hasPrimaryCamera;  // captured at enterPlay; false drives the editor warning
    };

    struct StepParams
    {
        std::optional<i32> frames;  // default 1
    };

    struct DeselectResult
    {
        i32 selectionVersion;
    };

    struct AddEntityParams
    {
        std::optional<AddEntityPreset> preset;
    };

    struct RenameEntityParams
    {
        EntitySelector entity;
        std::string name;
    };

    struct SetComponentFieldParams
    {
        EntitySelector entity;
        std::string component;
        std::string field;
        Json value;
    };

    struct SetComponentFieldResult
    {
        std::string set;
        std::string field;
    };

    struct EditorCamera
    {
        Vec3 position;
        f32 yaw;
        f32 pitch;
        f32 fov;
        f32 near;
        f32 far;
        f32 moveSpeed;
        f32 lookSpeed;
    };

    struct SetCameraParams
    {
        std::optional<Vec3> position;
        std::optional<f32> yaw;
        std::optional<f32> pitch;
        std::optional<f32> fov;
        std::optional<f32> near;
        std::optional<f32> far;
        std::optional<f32> moveSpeed;
        std::optional<f32> lookSpeed;
    };

    struct GizmoState
    {
        GizmoOpDto op;
        GizmoSpaceDto space;
        bool preserveChildren;
    };

    struct SetGizmoParams
    {
        std::optional<GizmoOpDto> op;
        std::optional<GizmoSpaceDto> space;
        std::optional<bool> preserveChildren;
    };

    struct GizmoPointerParams
    {
        std::optional<GizmoPointerPhase> phase;
        std::optional<f32> x;
        std::optional<f32> y;
    };

    struct GizmoPointerResult
    {
        std::string hovered;
        bool dragging;
    };

    struct FlyInputParams
    {
        std::optional<bool> active;
        std::optional<f32> lookDx;
        std::optional<f32> lookDy;
        std::optional<bool> forward;
        std::optional<bool> back;
        std::optional<bool> left;
        std::optional<bool> right;
        std::optional<bool> up;
        std::optional<bool> down;
    };

    struct FlyInputResult
    {
        bool active;
    };

    struct ScriptInputParams
    {
        std::vector<std::string> keys;
    };

    struct ScriptInputResult
    {
        std::vector<std::string> keys;
    };

    struct SetProbesParams
    {
        std::optional<bool> enabled;
    };

    struct SetProbesResult
    {
        bool probes;
    };

    struct RecaptureProbesResult
    {
        u32 marked;
    };

    struct ProbeRef
    {
        u32 slot;
        WireUuid entity;
        Vec3 origin;
        f32 influenceRadius;
        f32 intensity;
        bool boxProjection;
        bool valid;
        bool dirty;
    };

    struct ListProbesResult
    {
        bool enabled;
        u32 count;
        std::vector<ProbeRef> probes;
    };

    struct SetExposureParams
    {
        f32 ev;
    };

    struct SetExposureResult
    {
        f32 exposureEv;
    };

    auto dtoToJson(WireUuid value) -> Json;
    auto dtoToJson(AddEntityPreset value) -> Json;
    auto dtoToJson(PickKind value) -> Json;
    auto dtoToJson(GizmoOpDto value) -> Json;
    auto dtoToJson(GizmoSpaceDto value) -> Json;
    auto dtoToJson(GizmoPointerPhase value) -> Json;
    auto dtoToJson(AaModeDto value) -> Json;
    auto dtoToJson(GiModeDto value) -> Json;
    auto dtoToJson(AssetSlotDto value) -> Json;
    auto dtoToJson(ScreenshotTargetDto value) -> Json;
    auto dtoToJson(AssetTypeDto value) -> Json;
    auto dtoToJson(ProfilerModeDto value) -> Json;
    auto dtoToJson(ProfileLaneDto value) -> Json;
    auto dtoToJson(CaptureModeDto value) -> Json;
    auto dtoToJson(CaptureStateDto value) -> Json;
    auto dtoToJson(AlarmSeverityDto value) -> Json;
    auto dtoToJson(AlarmStateDto value) -> Json;
    auto dtoToJson(const Vec3& value) -> Json;
    auto dtoToJson(const Vec4& value) -> Json;
    auto dtoToJson(const EntityRef& value) -> Json;
    auto dtoToJson(const PingResult& value) -> Json;
    auto dtoToJson(const RenderStatsDto& value) -> Json;
    auto dtoToJson(const RenderPassTimingDto& value) -> Json;
    auto dtoToJson(const RenderPassTimingsDto& value) -> Json;
    auto dtoToJson(const ProfilerModeResult& value) -> Json;
    auto dtoToJson(const PipelineStatsDto& value) -> Json;
    auto dtoToJson(const ProfileSpanDto& value) -> Json;
    auto dtoToJson(const ProfileCaptureMetadataDto& value) -> Json;
    auto dtoToJson(const ProfileCaptureDto& value) -> Json;
    auto dtoToJson(const CaptureStartResult& value) -> Json;
    auto dtoToJson(const CaptureStopResult& value) -> Json;
    auto dtoToJson(const CaptureStatusResult& value) -> Json;
    auto dtoToJson(const FrameSampleDto& value) -> Json;
    auto dtoToJson(const FrameHistoryDto& value) -> Json;
    auto dtoToJson(const PerfConfigDto& value) -> Json;
    auto dtoToJson(const AlarmEventDto& value) -> Json;
    auto dtoToJson(const DrainAlarmsResult& value) -> Json;
    auto dtoToJson(const ActiveAlarmDto& value) -> Json;
    auto dtoToJson(const ActiveAlarmsDto& value) -> Json;
    auto dtoToJson(const ScriptStatusResult& value) -> Json;
    auto dtoToJson(const ScriptErrorDto& value) -> Json;
    auto dtoToJson(const DrainScriptErrorsResult& value) -> Json;
    auto dtoToJson(const ScriptFieldDto& value) -> Json;
    auto dtoToJson(const GetScriptSchemaResult& value) -> Json;
    auto dtoToJson(const SetScriptOverrideResult& value) -> Json;
    auto dtoToJson(const CreateScriptResult& value) -> Json;
    auto dtoToJson(const SetAaResult& value) -> Json;
    auto dtoToJson(const SetClusteredResult& value) -> Json;
    auto dtoToJson(const SetIblResult& value) -> Json;
    auto dtoToJson(const SetSsaoResult& value) -> Json;
    auto dtoToJson(const SetContactShadowsResult& value) -> Json;
    auto dtoToJson(const SetSsgiResult& value) -> Json;
    auto dtoToJson(const SetRtShadowsResult& value) -> Json;
    auto dtoToJson(const SetRestirResult& value) -> Json;
    auto dtoToJson(const SetGiResult& value) -> Json;
    auto dtoToJson(const SetShadowsResult& value) -> Json;
    auto dtoToJson(const SetSkinningResult& value) -> Json;
    auto dtoToJson(const SetDepthPrepassResult& value) -> Json;
    auto dtoToJson(const ViewportNativeInfoResult& value) -> Json;
    auto dtoToJson(const SetViewportSizeResult& value) -> Json;
    auto dtoToJson(const ProjectInfoDto& value) -> Json;
    auto dtoToJson(const ImportModelResult& value) -> Json;
    auto dtoToJson(const ImportTextureResult& value) -> Json;
    auto dtoToJson(const AssetEntryDto& value) -> Json;
    auto dtoToJson(const AssetList& value) -> Json;
    auto dtoToJson(const AssetRef& value) -> Json;
    auto dtoToJson(const AssetUsageDto& value) -> Json;
    auto dtoToJson(const AssetUsagesResult& value) -> Json;
    auto dtoToJson(const AssetMetadataDto& value) -> Json;
    auto dtoToJson(const DeleteAssetResult& value) -> Json;
    auto dtoToJson(const AssignAssetResult& value) -> Json;
    auto dtoToJson(const PathResult& value) -> Json;
    auto dtoToJson(const ScreenshotResult& value) -> Json;
    auto dtoToJson(const ThumbnailResult& value) -> Json;
    auto dtoToJson(const QuitResult& value) -> Json;
    auto dtoToJson(const EntityListEntry& value) -> Json;
    auto dtoToJson(const EntityList& value) -> Json;
    auto dtoToJson(const ComponentList& value) -> Json;
    auto dtoToJson(const DestroyEntityResult& value) -> Json;
    auto dtoToJson(const AddComponentResult& value) -> Json;
    auto dtoToJson(const RemoveComponentResult& value) -> Json;
    auto dtoToJson(const SetComponentResult& value) -> Json;
    auto dtoToJson(const PickResult& value) -> Json;
    auto dtoToJson(const InspectResult& value) -> Json;
    auto dtoToJson(const EnvironmentDto& value) -> Json;
    auto dtoToJson(const SelectionResult& value) -> Json;
    auto dtoToJson(const PlayStateResult& value) -> Json;
    auto dtoToJson(const DeselectResult& value) -> Json;
    auto dtoToJson(const SetComponentFieldResult& value) -> Json;
    auto dtoToJson(const EditorCamera& value) -> Json;
    auto dtoToJson(const GizmoState& value) -> Json;
    auto dtoToJson(const GizmoPointerResult& value) -> Json;
    auto dtoToJson(const FlyInputResult& value) -> Json;
    auto dtoToJson(const ScriptInputResult& value) -> Json;
    auto dtoToJson(const SetProbesResult& value) -> Json;
    auto dtoToJson(const RecaptureProbesResult& value) -> Json;
    auto dtoToJson(const ProbeRef& value) -> Json;
    auto dtoToJson(const ListProbesResult& value) -> Json;
    auto dtoToJson(const SetExposureResult& value) -> Json;

    auto parseDto(const Json& params, DtoTag<PingParams>) -> Result<PingParams>;
    auto parseDto(const Json& params, DtoTag<EmptyParams>) -> Result<EmptyParams>;
    auto parseDto(const Json& params, DtoTag<Vec3>) -> Result<Vec3>;
    auto parseDto(const Json& params, DtoTag<Vec4>) -> Result<Vec4>;
    auto parseDto(const Json& params, DtoTag<SetAaParams>) -> Result<SetAaParams>;
    auto parseDto(const Json& params, DtoTag<ProfilerSetModeParams>) -> Result<ProfilerSetModeParams>;
    auto parseDto(const Json& params, DtoTag<CaptureStartParams>) -> Result<CaptureStartParams>;
    auto parseDto(const Json& params, DtoTag<FrameHistoryParams>) -> Result<FrameHistoryParams>;
    auto parseDto(const Json& params, DtoTag<SetPerfConfigParams>) -> Result<SetPerfConfigParams>;
    auto parseDto(const Json& params, DtoTag<DrainAlarmsParams>) -> Result<DrainAlarmsParams>;
    auto parseDto(const Json& params, DtoTag<DrainScriptErrorsParams>) -> Result<DrainScriptErrorsParams>;
    auto parseDto(const Json& params, DtoTag<GetScriptSchemaParams>) -> Result<GetScriptSchemaParams>;
    auto parseDto(const Json& params, DtoTag<SetScriptOverrideParams>) -> Result<SetScriptOverrideParams>;
    auto parseDto(const Json& params, DtoTag<CreateScriptParams>) -> Result<CreateScriptParams>;
    auto parseDto(const Json& params, DtoTag<ToggleParams>) -> Result<ToggleParams>;
    auto parseDto(const Json& params, DtoTag<SetViewportSizeParams>) -> Result<SetViewportSizeParams>;
    auto parseDto(const Json& params, DtoTag<SetGiParams>) -> Result<SetGiParams>;
    auto parseDto(const Json& params, DtoTag<NewProjectParams>) -> Result<NewProjectParams>;
    auto parseDto(const Json& params, DtoTag<PathParams>) -> Result<PathParams>;
    auto parseDto(const Json& params, DtoTag<OptionalPathParams>) -> Result<OptionalPathParams>;
    auto parseDto(const Json& params, DtoTag<RenameAssetParams>) -> Result<RenameAssetParams>;
    auto parseDto(const Json& params, DtoTag<CreateAssetFolderParams>) -> Result<CreateAssetFolderParams>;
    auto parseDto(const Json& params, DtoTag<RenameAssetFolderParams>) -> Result<RenameAssetFolderParams>;
    auto parseDto(const Json& params, DtoTag<DeleteAssetFolderParams>) -> Result<DeleteAssetFolderParams>;
    auto parseDto(const Json& params, DtoTag<MoveAssetParams>) -> Result<MoveAssetParams>;
    auto parseDto(const Json& params, DtoTag<AssetUsagesParams>) -> Result<AssetUsagesParams>;
    auto parseDto(const Json& params, DtoTag<AssetMetadataParams>) -> Result<AssetMetadataParams>;
    auto parseDto(const Json& params, DtoTag<DeleteAssetParams>) -> Result<DeleteAssetParams>;
    auto parseDto(const Json& params, DtoTag<AssignAssetParams>) -> Result<AssignAssetParams>;
    auto parseDto(const Json& params, DtoTag<ScreenshotParams>) -> Result<ScreenshotParams>;
    auto parseDto(const Json& params, DtoTag<ThumbnailParams>) -> Result<ThumbnailParams>;
    auto parseDto(const Json& params, DtoTag<CreateEntityParams>) -> Result<CreateEntityParams>;
    auto parseDto(const Json& params, DtoTag<EntityParams>) -> Result<EntityParams>;
    auto parseDto(const Json& params, DtoTag<SetParentParams>) -> Result<SetParentParams>;
    auto parseDto(const Json& params, DtoTag<ComponentParams>) -> Result<ComponentParams>;
    auto parseDto(const Json& params, DtoTag<SetComponentParams>) -> Result<SetComponentParams>;
    auto parseDto(const Json& params, DtoTag<SetTransformParams>) -> Result<SetTransformParams>;
    auto parseDto(const Json& params, DtoTag<SetMaterialParams>) -> Result<SetMaterialParams>;
    auto parseDto(const Json& params, DtoTag<SetLightParams>) -> Result<SetLightParams>;
    auto parseDto(const Json& params, DtoTag<PickParams>) -> Result<PickParams>;
    auto parseDto(const Json& params, DtoTag<SetEnvironmentParams>) -> Result<SetEnvironmentParams>;
    auto parseDto(const Json& params, DtoTag<SetAtmosphereParams>) -> Result<SetAtmosphereParams>;
    auto parseDto(const Json& params, DtoTag<AddEntityParams>) -> Result<AddEntityParams>;
    auto parseDto(const Json& params, DtoTag<RenameEntityParams>) -> Result<RenameEntityParams>;
    auto parseDto(const Json& params, DtoTag<SetComponentFieldParams>) -> Result<SetComponentFieldParams>;
    auto parseDto(const Json& params, DtoTag<SetCameraParams>) -> Result<SetCameraParams>;
    auto parseDto(const Json& params, DtoTag<SetGizmoParams>) -> Result<SetGizmoParams>;
    auto parseDto(const Json& params, DtoTag<GizmoPointerParams>) -> Result<GizmoPointerParams>;
    auto parseDto(const Json& params, DtoTag<FlyInputParams>) -> Result<FlyInputParams>;
    auto parseDto(const Json& params, DtoTag<ScriptInputParams>) -> Result<ScriptInputParams>;
    auto parseDto(const Json& params, DtoTag<SetProbesParams>) -> Result<SetProbesParams>;
    auto parseDto(const Json& params, DtoTag<SetExposureParams>) -> Result<SetExposureParams>;
    auto parseDto(const Json& params, DtoTag<StepParams>) -> Result<StepParams>;
}
