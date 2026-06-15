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

    /// Debug render-output mode (read back via render-stats; transient, not persisted).
    enum class ViewModeDto
    {
        Lit,
        Wireframe,
        Albedo,
        Normal,
        Roughness,
        Metallic,
        Emissive,
    };

    enum class AssetSlotDto
    {
        Mesh,
        Albedo,
        MetallicRoughness,
        Normal,
        Occlusion,
        Emissive,
        Height,
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
        Animation,
        Material,
        Model,
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
        i32 bindlessTextures;  // bindless slots in use (high-water mark of the array)
        i32 bindlessFree;      // reclaimed slots available for reuse
        bool hdr;
        f32 exposureEv;
        AaModeDto aa;
        ViewModeDto viewMode;  // debug render-output mode
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

    struct PhysicsStateResult
    {
        bool active;       // a PhysicsWorld exists (Playing/Paused)
        i32 bodyCount;     // bodies in the world
        i32 dynamicCount;  // of those, Dynamic
    };

    struct FitColliderParams
    {
        EntitySelector entity;
    };

    struct FitColliderResult
    {
        WireUuid entity;
        std::string shape;  // the shape that was fitted
        Vec3 halfExtents;   // resulting dims (radius/half-height packed for sphere/capsule)
        Vec3 offset;        // the fitted centre
    };

    struct ContactEventDto
    {
        i64 seq;
        std::string kind;  // "begin" | "end"
        WireUuid entityA;
        WireUuid entityB;
        bool sensor;  // either body is a sensor (a trigger overlap, not a solid touch)
        Vec3 point;
        Vec3 normal;
        i64 tick;
    };

    struct DrainContactsParams
    {
        std::optional<i64> since;
    };

    struct DrainContactsResult
    {
        std::vector<ContactEventDto> events;
        i64 highWaterSeq;
        i64 oldestSeq;
        bool overflowed;
    };

    struct PhysicsBodyDto
    {
        WireUuid entity;
        std::string motion;  // "static" | "kinematic" | "dynamic"
        bool active;         // awake vs sleeping
        Vec3 position;       // world-space body origin
    };

    struct PhysicsBodiesResult
    {
        std::vector<PhysicsBodyDto> bodies;
    };

    struct ApplyImpulseParams
    {
        EntitySelector entity;
        Vec3 impulse;
    };

    struct ApplyImpulseResult
    {
        Vec3 velocity;  // the body's linear velocity after the impulse
    };

    struct SetKinematicBonesParams
    {
        EntitySelector entity;
        std::optional<bool> enabled;
    };

    struct KinematicBonesResult
    {
        WireUuid entity;
        bool enabled;
        i32 boneCount;  // the rig's SkinnedMesh joint count
    };

    struct MoveCharacterParams
    {
        EntitySelector entity;
        Vec3 velocity;             // desired world-space horizontal velocity (m/s); y ignored
        std::optional<bool> jump;  // a one-step upward impulse
    };

    struct MoveCharacterResult
    {
        Vec3 position;  // resolved world position after the last step
        bool onGround;  // ground state from the last step
    };

    struct RaycastParams
    {
        Vec3 origin;
        Vec3 dir;                    // need not be normalized; `distance` reads back in dir units
        std::optional<f32> maxDist;  // default 1000
    };

    struct ShapecastParams
    {
        Vec3 origin;
        Vec3 dir;
        f32 radius;
        std::optional<f32> maxDist;  // default 1000
    };

    struct RaycastResult
    {
        bool hit;
        WireUuid entity;  // 0 when nothing was hit / the hit body has no entity
        Vec3 point;
        Vec3 normal;
        f32 distance;
    };

    struct EnableRagdollParams
    {
        EntitySelector entity;
        std::optional<bool> enabled;  // default true
    };

    struct RagdollResult
    {
        bool present;    // a ragdoll instance is live this play session
        bool active;     // motors on (active blend) vs passive limp
        f32 bodyWeight;  // mean target physics weight across bones
        i32 bones;       // BonePhysicsComponent.bones count
    };

    struct SetRagdollParams
    {
        EntitySelector entity;
        std::optional<bool> active;     // motors on (active, drive to animation) vs off (passive limp)
        std::optional<f32> bodyWeight;  // uniform target physics weight 0..1 across the whole rig
        std::optional<i32> bone;        // a single bone index to target (with `weight`) for a partial ragdoll
        std::optional<f32> weight;      // the target physics weight for `bone`
    };

    struct GetRagdollParams
    {
        EntitySelector entity;
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

    struct ScriptLogDto
    {
        i64 seq;
        WireUuid entity;      // the logging entity, or 0 when logged outside an entity handler
        std::string message;  // the se.log(...) text
        i64 epochMs;          // wall-clock ms when logged (display only)
        i64 tick;             // the play tick the line was logged on
    };

    struct DrainScriptLogsParams
    {
        std::optional<i64> since;
    };

    struct DrainScriptLogsResult
    {
        std::vector<ScriptLogDto> events;
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

    struct SetViewModeParams
    {
        std::optional<ViewModeDto> mode;
    };

    struct SetViewModeResult
    {
        ViewModeDto viewMode;
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
        std::optional<std::string> view;  // "scene" (default) | "assetPreview" — which view to resize
        std::optional<i32> width;
        std::optional<i32> height;
    };

    struct SetViewportSizeResult
    {
        i32 width;
        i32 height;
    };

    struct SetActiveViewParams
    {
        std::string view;  // "scene" | "assetPreview" — the view the engine renders + publishes
    };

    struct SetActiveViewResult
    {
        std::string view;  // the now-active view, echoed back
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
        std::string type;
    };

    struct InstantiateModelParams
    {
        AssetSelector asset;
        std::optional<std::string> name;
    };

    struct ExtractSubAssetParams
    {
        AssetSelector asset;
        WireUuid subAsset;
        std::optional<std::string> dest;
    };

    struct ClearExtractionParams
    {
        AssetSelector asset;
        WireUuid subAsset;
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
        std::optional<WireUuid> container;  // present for a sub-asset embedded in a .smodel
        std::optional<f32> duration;        // present (animation rows): clip length in seconds
        std::optional<bool> rigged;         // present (true) when the asset belongs to a rigged .smodel
    };

    struct AssetList
    {
        std::vector<AssetEntryDto> assets;
        std::vector<std::string> folders;
    };

    struct ScanAssetsResult
    {
        i32 added;
        i32 removed;
    };

    struct ReimportModelResult
    {
        i32 updated;
        i32 added;
        i32 removedFromSource;
        bool skipped;
    };

    struct ReimportModelParams
    {
        AssetSelector asset;
    };

    struct ModelInfoParams
    {
        AssetSelector asset;
    };

    struct ModelSubAssetDto
    {
        WireUuid id;
        std::string name;
        std::string type;
        u64 bytes;
    };

    struct ModelInfoResult
    {
        WireUuid id;
        std::string name;
        std::string sourcePath;
        std::string sourceHash;
        i32 materialCount;
        bool hasSkin;
        i32 nodeCount;
        u64 totalBytes;
        std::vector<ModelSubAssetDto> subAssets;
    };

    struct AssetReferencesParams
    {
        AssetSelector asset;
    };

    struct AssetReferencesResult
    {
        std::vector<std::string> referencedBy;
        std::vector<std::string> references;
        u64 footprint;
    };

    struct CleanCandidateDto
    {
        WireUuid id;
        std::string path;
        std::string category;
        u64 bytes;
        std::string reason;
    };

    struct CleanReport
    {
        std::vector<CleanCandidateDto> candidates;
        u64 reclaimableBytes;
    };

    struct CleanAssetsParams
    {
        std::optional<bool> dryRun;
        std::optional<std::vector<std::string>> exclude;
    };

    struct DeleteUnusedParams
    {
        std::vector<std::string> ids;
        std::optional<bool> confirm;
    };

    struct DeleteUnusedResult
    {
        i32 deleted;
        u64 reclaimedBytes;
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

    struct MaterialCreateParams
    {
        std::string name;
    };

    struct MaterialCreateResult
    {
        WireUuid id;
        std::string name;
    };

    struct MaterialAssignParams
    {
        EntitySelector entity;
        AssetSelector material;  // 0 / empty clears the assignment
    };

    struct MaterialAssignResult
    {
        WireUuid material;
    };

    struct MaterialImportParams
    {
        std::string path;  // a folder of PBR textures (suffix-detected) or a single texture
        std::string name;
    };

    struct MaterialImportResultDto
    {
        WireUuid id;
        std::string roles;  // space-joined detected map roles, for the editor's confirmation proposal
    };

    struct MaterialRefDto
    {
        WireUuid id;
        std::string name;
        std::string folder;
    };

    struct MaterialListResult
    {
        std::vector<MaterialRefDto> materials;
    };

    struct MaterialGetParams
    {
        AssetSelector material;
    };

    struct MaterialGetResult
    {
        WireUuid id;
        std::string blend;
        bool unlit;
        Vec4 baseColor;
        f32 metallic;
        f32 roughness;
        Vec3 emissive;
        f32 emissiveStrength;
        WireUuid albedoTexture;
        WireUuid ormTexture;
        WireUuid normalTexture;
        WireUuid emissiveTexture;
        WireUuid heightTexture;
        Json graph;  // the material's node graph ({} when none) — the source the editor loads to edit
    };

    struct MaterialUpdateParams
    {
        AssetSelector material;
        std::optional<Vec4> baseColor;
        std::optional<f32> metallic;
        std::optional<f32> roughness;
        std::optional<Vec3> emissive;
        std::optional<f32> emissiveStrength;
        std::optional<f32> normalStrength;
        std::optional<WireUuid> albedoTexture;
        std::optional<WireUuid> ormTexture;
        std::optional<WireUuid> normalTexture;
        std::optional<WireUuid> emissiveTexture;
        std::optional<WireUuid> heightTexture;
    };

    struct MaterialUpdateResult
    {
        WireUuid id;
    };

    struct PreviewRenderParams
    {
        AssetSelector material;
        std::optional<u32> size;
    };

    struct PreviewRenderResult
    {
        std::string png;  // base64-encoded PNG of the material on a studio-lit sphere
    };

    struct MaterialSetGraphParams
    {
        AssetSelector material;
        Json graph;  // { nodes:[{id,type,props}], edges:[{from:[node,pin],to:[node,pin]}] }
    };

    struct MaterialSetGraphResult
    {
        WireUuid id;
        bool foldable;  // true if the graph folded entirely to params (no codegen-only node)
    };

    struct MaterialCreateInstanceParams
    {
        AssetSelector parent;
        std::string name;
    };

    struct MaterialSetOverrideParams
    {
        AssetSelector material;
        std::string field;  // baseColor|metallic|roughness|emissive|emissiveStrength|normalStrength|*Texture
        Json value;
    };

    struct MaterialSetOverrideResult
    {
        WireUuid id;
    };

    struct MaterialCompileParams
    {
        AssetSelector material;
    };

    struct MaterialCompileResult
    {
        WireUuid id;
        bool ok;  // the graph emitted compilable Slang and slangc produced a .spv
    };

    struct MaterialCookResult
    {
        u32 compiled;  // codegen materials whose übershader variant compiled
        u32 failed;    // codegen materials whose compile failed
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
        bool pending;  // true: generation was enqueued off-thread — the client should retry
    };

    struct ThumbnailCacheParams
    {
        std::string action;  // "stats" | "clear"
    };

    struct ThumbnailCacheResult
    {
        i32 entries;  // cache entries counted (stats) or removed (clear)
        i64 bytes;    // total bytes of those entries
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
        i32 animationVersion;  // bumped by play/pause/seek/loop so the reconcile poll can gate on it
    };

    struct PlayStateResult
    {
        std::string state;  // "edit" | "playing" | "paused"
        i32 playVersion;
        i32 sceneVersion;       // echoed so a stop reads as a scene change
        bool hasPrimaryCamera;  // captured at enterPlay; false drives the editor warning
        i32 animationVersion;   // bumped by the animation commands so the reconcile poll can gate on it
        WireUuid previewAsset;  // the model being previewed, 0 when none (Edit/Play/Preview triad)
    };

    struct AnimationClipDto
    {
        WireUuid id;
        std::string name;
        f32 duration;
        i32 tracks;  // animated joint-channel count (0 when unknown, e.g. the global list-clips catalog)
    };

    struct BoneDto
    {
        i32 index;
        std::string name;
        i32 parent;
        bool joint;
    };

    // What a model can do — a flat, additive capability struct read once when the asset editor opens.
    // A new capability (physics bodies, sockets, LODs) appends a field here; existing readers ignore
    // unknown fields by construction, so neither the wire nor the editor needs a structural change.
    struct AssetCapabilitiesDto
    {
        i32 meshCount;      // Mesh sub-assets (every openable model has >= 1)
        i32 materialCount;  // Material sub-assets
        i32 nodeCount;      // node-forest size
        bool hasRig;        // the container carries a skin -> skeleton tree + overlay
        i32 boneCount;      // bones.size() (0 when !hasRig)
        i32 clipCount;      // clips.size() (0 when none) -> clip list + timeline
    };

    struct GetAssetModelParams
    {
        AssetSelector asset;  // a model, a mesh sub-asset, or a clip sub-asset (all resolve to the container)
    };

    struct AssetModelResult
    {
        WireUuid mesh;  // the owning container id (the editor's tab key)
        std::string name;
        AssetCapabilitiesDto capabilities;    // what the model can do; gates the editor's panels
        std::vector<BoneDto> bones;           // empty unless capabilities.hasRig
        std::vector<AnimationClipDto> clips;  // empty unless capabilities.clipCount > 0
    };

    struct EnterAssetPreviewParams
    {
        AssetSelector asset;
    };

    struct BoneEntityDto
    {
        i32 index;        // the get-asset-model node index this preview entity stands in for
        WireUuid entity;  // the spawned preview-scene entity uuid for that joint
    };

    struct AssetPreviewResult
    {
        WireUuid rootEntity;               // the spawned model root entity (the timeline target)
        std::vector<BoneEntityDto> bones;  // joint node index -> spawned entity (empty for an unrigged model)
        Vec3 target;                       // the framed orbit pivot (bounding-sphere center)
        f32 distance;                      // the framing distance from the target (orbit radius)
    };

    struct ListClipsParams
    {
        std::optional<AssetSelector> asset;  // filter to a model container's clips (omit for the full catalog)
    };

    struct ListClipsResult
    {
        std::vector<AnimationClipDto> clips;
    };

    struct PlayAnimationParams
    {
        EntitySelector entity;
        AssetSelector clip;
        std::optional<f32> speed;    // default 1
        std::optional<bool> loop;    // default true
        std::optional<f32> blend;    // default 0 (transition seconds)
        std::optional<bool> paused;  // default false; true loads the clip at frame 0 without playing
    };

    struct SeekAnimationParams
    {
        EntitySelector entity;
        f32 time;
        std::optional<f32> seekBlend;  // blend seconds; poses ease to the seeked time instead of snapping (def 0)
    };

    struct SetAnimationLoopParams
    {
        EntitySelector entity;
        std::string wrap;  // "once" | "loop" | "pingpong"
    };

    struct SetAnimationPlayingParams
    {
        EntitySelector entity;
        bool playing;  // true resumes from the current playhead; false pauses (keeps the pose shown)
    };

    struct AnimationStateParams
    {
        EntitySelector entity;
    };

    struct AnimationStateResult
    {
        WireUuid clip;
        std::string clipName;
        f32 duration;
        f32 time;
        bool playing;
        std::string wrap;
        f32 speed;
        i32 animationVersion;
    };

    struct SetSkeletonOverlayParams
    {
        std::optional<bool> show;      // master toggle
        std::optional<bool> axes;      // per-joint RGB axis lines
        std::optional<f32> jointSize;  // joint-dot radius in pixels (screen-constant)
    };

    struct SkeletonOverlayResult
    {
        bool show;
        bool axes;
        f32 jointSize;
        i32 highlightJoint;  // get-asset-model node index of the tinted joint, -1 = none
    };

    struct DebugOverlaysParams
    {
        std::optional<bool> bounds;
        std::optional<bool> sceneAabb;
        std::optional<bool> lightVolumes;
        std::optional<bool> grid;
        std::optional<bool> colliders;
    };

    struct DebugOverlaysResult
    {
        bool bounds;
        bool sceneAabb;
        bool lightVolumes;
        bool grid;
        bool colliders;
    };

    struct SetSkeletonHighlightParams
    {
        i32 joint;  // a get-asset-model node index to tint in the preview overlay, -1 = clear
    };

    struct PickSkeletonJointParams
    {
        f32 u;                        // normalized [0,1] viewport x of the click
        f32 v;                        // normalized [0,1] viewport y of the click
        std::optional<f32> radiusPx;  // hit-test radius in pixels around the click (def 8)
    };

    struct PickSkeletonJointResult
    {
        bool found;
        i32 nodeIndex;  // the picked joint's get-asset-model node index, -1 when none within radius
    };

    struct SetAssetPreviewOptionsParams
    {
        std::optional<bool> floor;  // show the preview floor slab
    };

    struct AssetPreviewOptionsResult
    {
        bool floor;
    };

    struct SetFootIkParams
    {
        EntitySelector entity;
        std::optional<bool> enabled;
        std::optional<f32> groundHeight;
    };

    struct GetFootIkParams
    {
        EntitySelector entity;
    };

    struct FootIkResult
    {
        bool enabled;
        f32 groundHeight;
        i32 chains;
    };

    /// An entity's composed world-space transform (the cached WorldTransformComponent), so a
    /// caller can read a bone's world position — e.g. to verify foot IK plants on the ground.
    struct WorldTransformResult
    {
        Vec3 translation;
        Vec3 scale;
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
        std::optional<i32> index;  // when set, `field` names an array and `value` merges into element `index`
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
        std::vector<std::string> keys;                         // the full held-key set (always replaces)
        std::optional<std::vector<std::string>> mouseButtons;  // "left"/"right"/"middle"; replaces when present
        std::optional<f32> mouseX;                             // viewport-relative pointer; unchanged when absent
        std::optional<f32> mouseY;
        std::optional<f32> scroll;
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
    auto dtoToJson(ViewModeDto value) -> Json;
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
    auto dtoToJson(const PhysicsStateResult& value) -> Json;
    auto dtoToJson(const FitColliderResult& value) -> Json;
    auto dtoToJson(const ContactEventDto& value) -> Json;
    auto dtoToJson(const DrainContactsResult& value) -> Json;
    auto dtoToJson(const PhysicsBodyDto& value) -> Json;
    auto dtoToJson(const PhysicsBodiesResult& value) -> Json;
    auto dtoToJson(const ApplyImpulseResult& value) -> Json;
    auto dtoToJson(const KinematicBonesResult& value) -> Json;
    auto dtoToJson(const MoveCharacterResult& value) -> Json;
    auto dtoToJson(const RaycastResult& value) -> Json;
    auto dtoToJson(const RagdollResult& value) -> Json;
    auto dtoToJson(const ScriptErrorDto& value) -> Json;
    auto dtoToJson(const DrainScriptErrorsResult& value) -> Json;
    auto dtoToJson(const ScriptLogDto& value) -> Json;
    auto dtoToJson(const DrainScriptLogsResult& value) -> Json;
    auto dtoToJson(const ScriptFieldDto& value) -> Json;
    auto dtoToJson(const GetScriptSchemaResult& value) -> Json;
    auto dtoToJson(const SetScriptOverrideResult& value) -> Json;
    auto dtoToJson(const CreateScriptResult& value) -> Json;
    auto dtoToJson(const SetAaResult& value) -> Json;
    auto dtoToJson(const SetViewModeResult& value) -> Json;
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
    auto dtoToJson(const SetActiveViewResult& value) -> Json;
    auto dtoToJson(const ProjectInfoDto& value) -> Json;
    auto dtoToJson(const ImportModelResult& value) -> Json;
    auto dtoToJson(const ScanAssetsResult& value) -> Json;
    auto dtoToJson(const ReimportModelResult& value) -> Json;
    auto dtoToJson(const ModelSubAssetDto& value) -> Json;
    auto dtoToJson(const ModelInfoResult& value) -> Json;
    auto dtoToJson(const AssetReferencesResult& value) -> Json;
    auto dtoToJson(const CleanCandidateDto& value) -> Json;
    auto dtoToJson(const CleanReport& value) -> Json;
    auto dtoToJson(const DeleteUnusedResult& value) -> Json;
    auto dtoToJson(const ImportTextureResult& value) -> Json;
    auto dtoToJson(const AssetEntryDto& value) -> Json;
    auto dtoToJson(const AssetList& value) -> Json;
    auto dtoToJson(const AssetRef& value) -> Json;
    auto dtoToJson(const AssetUsageDto& value) -> Json;
    auto dtoToJson(const AssetUsagesResult& value) -> Json;
    auto dtoToJson(const AssetMetadataDto& value) -> Json;
    auto dtoToJson(const DeleteAssetResult& value) -> Json;
    auto dtoToJson(const AssignAssetResult& value) -> Json;
    auto dtoToJson(const MaterialCreateResult& value) -> Json;
    auto dtoToJson(const MaterialAssignResult& value) -> Json;
    auto dtoToJson(const MaterialImportResultDto& value) -> Json;
    auto dtoToJson(const MaterialRefDto& value) -> Json;
    auto dtoToJson(const MaterialListResult& value) -> Json;
    auto dtoToJson(const MaterialGetResult& value) -> Json;
    auto dtoToJson(const MaterialUpdateResult& value) -> Json;
    auto dtoToJson(const PreviewRenderResult& value) -> Json;
    auto dtoToJson(const MaterialSetGraphResult& value) -> Json;
    auto dtoToJson(const MaterialSetOverrideResult& value) -> Json;
    auto dtoToJson(const MaterialCompileResult& value) -> Json;
    auto dtoToJson(const MaterialCookResult& value) -> Json;
    auto dtoToJson(const PathResult& value) -> Json;
    auto dtoToJson(const ScreenshotResult& value) -> Json;
    auto dtoToJson(const ThumbnailResult& value) -> Json;
    auto dtoToJson(const ThumbnailCacheResult& value) -> Json;
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
    auto dtoToJson(const AnimationClipDto& value) -> Json;
    auto dtoToJson(const BoneDto& value) -> Json;
    auto dtoToJson(const AssetCapabilitiesDto& value) -> Json;
    auto dtoToJson(const AssetModelResult& value) -> Json;
    auto dtoToJson(const BoneEntityDto& value) -> Json;
    auto dtoToJson(const AssetPreviewResult& value) -> Json;
    auto dtoToJson(const ListClipsResult& value) -> Json;
    auto dtoToJson(const AnimationStateResult& value) -> Json;
    auto dtoToJson(const SkeletonOverlayResult& value) -> Json;
    auto dtoToJson(const DebugOverlaysResult& value) -> Json;
    auto dtoToJson(const PickSkeletonJointResult& value) -> Json;
    auto dtoToJson(const AssetPreviewOptionsResult& value) -> Json;
    auto dtoToJson(const FootIkResult& value) -> Json;
    auto dtoToJson(const WorldTransformResult& value) -> Json;
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
    auto parseDto(const Json& params, DtoTag<SetViewModeParams>) -> Result<SetViewModeParams>;
    auto parseDto(const Json& params, DtoTag<ProfilerSetModeParams>) -> Result<ProfilerSetModeParams>;
    auto parseDto(const Json& params, DtoTag<CaptureStartParams>) -> Result<CaptureStartParams>;
    auto parseDto(const Json& params, DtoTag<FrameHistoryParams>) -> Result<FrameHistoryParams>;
    auto parseDto(const Json& params, DtoTag<SetPerfConfigParams>) -> Result<SetPerfConfigParams>;
    auto parseDto(const Json& params, DtoTag<DrainAlarmsParams>) -> Result<DrainAlarmsParams>;
    auto parseDto(const Json& params, DtoTag<DrainScriptErrorsParams>) -> Result<DrainScriptErrorsParams>;
    auto parseDto(const Json& params, DtoTag<DrainScriptLogsParams>) -> Result<DrainScriptLogsParams>;
    auto parseDto(const Json& params, DtoTag<GetScriptSchemaParams>) -> Result<GetScriptSchemaParams>;
    auto parseDto(const Json& params, DtoTag<SetScriptOverrideParams>) -> Result<SetScriptOverrideParams>;
    auto parseDto(const Json& params, DtoTag<CreateScriptParams>) -> Result<CreateScriptParams>;
    auto parseDto(const Json& params, DtoTag<ToggleParams>) -> Result<ToggleParams>;
    auto parseDto(const Json& params, DtoTag<SetViewportSizeParams>) -> Result<SetViewportSizeParams>;
    auto parseDto(const Json& params, DtoTag<SetActiveViewParams>) -> Result<SetActiveViewParams>;
    auto parseDto(const Json& params, DtoTag<SetGiParams>) -> Result<SetGiParams>;
    auto parseDto(const Json& params, DtoTag<NewProjectParams>) -> Result<NewProjectParams>;
    auto parseDto(const Json& params, DtoTag<PathParams>) -> Result<PathParams>;
    auto parseDto(const Json& params, DtoTag<OptionalPathParams>) -> Result<OptionalPathParams>;
    auto parseDto(const Json& params, DtoTag<RenameAssetParams>) -> Result<RenameAssetParams>;
    auto parseDto(const Json& params, DtoTag<InstantiateModelParams>) -> Result<InstantiateModelParams>;
    auto parseDto(const Json& params, DtoTag<ExtractSubAssetParams>) -> Result<ExtractSubAssetParams>;
    auto parseDto(const Json& params, DtoTag<ClearExtractionParams>) -> Result<ClearExtractionParams>;
    auto parseDto(const Json& params, DtoTag<ReimportModelParams>) -> Result<ReimportModelParams>;
    auto parseDto(const Json& params, DtoTag<ModelInfoParams>) -> Result<ModelInfoParams>;
    auto parseDto(const Json& params, DtoTag<AssetReferencesParams>) -> Result<AssetReferencesParams>;
    auto parseDto(const Json& params, DtoTag<CleanAssetsParams>) -> Result<CleanAssetsParams>;
    auto parseDto(const Json& params, DtoTag<DeleteUnusedParams>) -> Result<DeleteUnusedParams>;
    auto parseDto(const Json& params, DtoTag<CreateAssetFolderParams>) -> Result<CreateAssetFolderParams>;
    auto parseDto(const Json& params, DtoTag<RenameAssetFolderParams>) -> Result<RenameAssetFolderParams>;
    auto parseDto(const Json& params, DtoTag<DeleteAssetFolderParams>) -> Result<DeleteAssetFolderParams>;
    auto parseDto(const Json& params, DtoTag<MoveAssetParams>) -> Result<MoveAssetParams>;
    auto parseDto(const Json& params, DtoTag<AssetUsagesParams>) -> Result<AssetUsagesParams>;
    auto parseDto(const Json& params, DtoTag<AssetMetadataParams>) -> Result<AssetMetadataParams>;
    auto parseDto(const Json& params, DtoTag<DeleteAssetParams>) -> Result<DeleteAssetParams>;
    auto parseDto(const Json& params, DtoTag<AssignAssetParams>) -> Result<AssignAssetParams>;
    auto parseDto(const Json& params, DtoTag<MaterialCreateParams>) -> Result<MaterialCreateParams>;
    auto parseDto(const Json& params, DtoTag<MaterialAssignParams>) -> Result<MaterialAssignParams>;
    auto parseDto(const Json& params, DtoTag<MaterialImportParams>) -> Result<MaterialImportParams>;
    auto parseDto(const Json& params, DtoTag<MaterialGetParams>) -> Result<MaterialGetParams>;
    auto parseDto(const Json& params, DtoTag<MaterialUpdateParams>) -> Result<MaterialUpdateParams>;
    auto parseDto(const Json& params, DtoTag<PreviewRenderParams>) -> Result<PreviewRenderParams>;
    auto parseDto(const Json& params, DtoTag<MaterialSetGraphParams>) -> Result<MaterialSetGraphParams>;
    auto parseDto(const Json& params, DtoTag<MaterialCreateInstanceParams>) -> Result<MaterialCreateInstanceParams>;
    auto parseDto(const Json& params, DtoTag<MaterialSetOverrideParams>) -> Result<MaterialSetOverrideParams>;
    auto parseDto(const Json& params, DtoTag<MaterialCompileParams>) -> Result<MaterialCompileParams>;
    auto parseDto(const Json& params, DtoTag<ScreenshotParams>) -> Result<ScreenshotParams>;
    auto parseDto(const Json& params, DtoTag<ThumbnailParams>) -> Result<ThumbnailParams>;
    auto parseDto(const Json& params, DtoTag<ThumbnailCacheParams>) -> Result<ThumbnailCacheParams>;
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
    auto parseDto(const Json& params, DtoTag<GetAssetModelParams>) -> Result<GetAssetModelParams>;
    auto parseDto(const Json& params, DtoTag<EnterAssetPreviewParams>) -> Result<EnterAssetPreviewParams>;
    auto parseDto(const Json& params, DtoTag<ListClipsParams>) -> Result<ListClipsParams>;
    auto parseDto(const Json& params, DtoTag<PlayAnimationParams>) -> Result<PlayAnimationParams>;
    auto parseDto(const Json& params, DtoTag<SeekAnimationParams>) -> Result<SeekAnimationParams>;
    auto parseDto(const Json& params, DtoTag<SetAnimationLoopParams>) -> Result<SetAnimationLoopParams>;
    auto parseDto(const Json& params, DtoTag<SetAnimationPlayingParams>) -> Result<SetAnimationPlayingParams>;
    auto parseDto(const Json& params, DtoTag<AnimationStateParams>) -> Result<AnimationStateParams>;
    auto parseDto(const Json& params, DtoTag<SetSkeletonOverlayParams>) -> Result<SetSkeletonOverlayParams>;
    auto parseDto(const Json& params, DtoTag<DebugOverlaysParams>) -> Result<DebugOverlaysParams>;
    auto parseDto(const Json& params, DtoTag<SetSkeletonHighlightParams>) -> Result<SetSkeletonHighlightParams>;
    auto parseDto(const Json& params, DtoTag<PickSkeletonJointParams>) -> Result<PickSkeletonJointParams>;
    auto parseDto(const Json& params, DtoTag<SetAssetPreviewOptionsParams>) -> Result<SetAssetPreviewOptionsParams>;
    auto parseDto(const Json& params, DtoTag<SetFootIkParams>) -> Result<SetFootIkParams>;
    auto parseDto(const Json& params, DtoTag<FitColliderParams>) -> Result<FitColliderParams>;
    auto parseDto(const Json& params, DtoTag<ApplyImpulseParams>) -> Result<ApplyImpulseParams>;
    auto parseDto(const Json& params, DtoTag<DrainContactsParams>) -> Result<DrainContactsParams>;
    auto parseDto(const Json& params, DtoTag<SetKinematicBonesParams>) -> Result<SetKinematicBonesParams>;
    auto parseDto(const Json& params, DtoTag<MoveCharacterParams>) -> Result<MoveCharacterParams>;
    auto parseDto(const Json& params, DtoTag<RaycastParams>) -> Result<RaycastParams>;
    auto parseDto(const Json& params, DtoTag<ShapecastParams>) -> Result<ShapecastParams>;
    auto parseDto(const Json& params, DtoTag<EnableRagdollParams>) -> Result<EnableRagdollParams>;
    auto parseDto(const Json& params, DtoTag<SetRagdollParams>) -> Result<SetRagdollParams>;
    auto parseDto(const Json& params, DtoTag<GetRagdollParams>) -> Result<GetRagdollParams>;
    auto parseDto(const Json& params, DtoTag<GetFootIkParams>) -> Result<GetFootIkParams>;
}
