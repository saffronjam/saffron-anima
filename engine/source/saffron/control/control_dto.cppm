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

    struct AttachNativeViewportParams
    {
        WireUuid parentXid;
        std::optional<i32> x;
        std::optional<i32> y;
        std::optional<i32> width;
        std::optional<i32> height;
    };

    struct AttachNativeViewportResult
    {
        bool attached;
        std::string transport;
        i32 x;
        i32 y;
        i32 width;
        i32 height;
    };

    struct ResizeNativeViewportParams
    {
        std::optional<i32> x;
        std::optional<i32> y;
        std::optional<i32> width;
        std::optional<i32> height;
    };

    struct ResizeNativeViewportResult
    {
        bool resized;
        i32 x;
        i32 y;
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
    };

    struct AssetList
    {
        std::vector<AssetEntryDto> assets;
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

    struct DestroyEntityResult
    {
        WireUuid destroyed;
    };

    struct EntityList
    {
        std::vector<EntityRef> entities;
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
    };

    struct SetMaterialParams
    {
        EntitySelector entity;
        std::optional<Vec4> baseColor;
        std::optional<WireUuid> albedoTexture;
        std::optional<f32> metallic;
        std::optional<f32> roughness;
        std::optional<Vec3> emissive;
        std::optional<f32> emissiveStrength;
        std::optional<bool> unlit;
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
    };

    struct SetGizmoParams
    {
        std::optional<GizmoOpDto> op;
        std::optional<GizmoSpaceDto> space;
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
    auto dtoToJson(const Vec3& value) -> Json;
    auto dtoToJson(const Vec4& value) -> Json;
    auto dtoToJson(const EntityRef& value) -> Json;
    auto dtoToJson(const PingResult& value) -> Json;
    auto dtoToJson(const RenderStatsDto& value) -> Json;
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
    auto dtoToJson(const SetDepthPrepassResult& value) -> Json;
    auto dtoToJson(const ViewportNativeInfoResult& value) -> Json;
    auto dtoToJson(const AttachNativeViewportResult& value) -> Json;
    auto dtoToJson(const ResizeNativeViewportResult& value) -> Json;
    auto dtoToJson(const ProjectInfoDto& value) -> Json;
    auto dtoToJson(const ImportModelResult& value) -> Json;
    auto dtoToJson(const ImportTextureResult& value) -> Json;
    auto dtoToJson(const AssetEntryDto& value) -> Json;
    auto dtoToJson(const AssetList& value) -> Json;
    auto dtoToJson(const AssetRef& value) -> Json;
    auto dtoToJson(const AssignAssetResult& value) -> Json;
    auto dtoToJson(const PathResult& value) -> Json;
    auto dtoToJson(const ScreenshotResult& value) -> Json;
    auto dtoToJson(const ThumbnailResult& value) -> Json;
    auto dtoToJson(const QuitResult& value) -> Json;
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
    auto dtoToJson(const DeselectResult& value) -> Json;
    auto dtoToJson(const SetComponentFieldResult& value) -> Json;
    auto dtoToJson(const EditorCamera& value) -> Json;
    auto dtoToJson(const GizmoState& value) -> Json;
    auto dtoToJson(const GizmoPointerResult& value) -> Json;
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
    auto parseDto(const Json& params, DtoTag<ToggleParams>) -> Result<ToggleParams>;
    auto parseDto(const Json& params, DtoTag<SetGiParams>) -> Result<SetGiParams>;
    auto parseDto(const Json& params, DtoTag<AttachNativeViewportParams>) -> Result<AttachNativeViewportParams>;
    auto parseDto(const Json& params, DtoTag<ResizeNativeViewportParams>) -> Result<ResizeNativeViewportParams>;
    auto parseDto(const Json& params, DtoTag<NewProjectParams>) -> Result<NewProjectParams>;
    auto parseDto(const Json& params, DtoTag<PathParams>) -> Result<PathParams>;
    auto parseDto(const Json& params, DtoTag<OptionalPathParams>) -> Result<OptionalPathParams>;
    auto parseDto(const Json& params, DtoTag<RenameAssetParams>) -> Result<RenameAssetParams>;
    auto parseDto(const Json& params, DtoTag<AssignAssetParams>) -> Result<AssignAssetParams>;
    auto parseDto(const Json& params, DtoTag<ScreenshotParams>) -> Result<ScreenshotParams>;
    auto parseDto(const Json& params, DtoTag<ThumbnailParams>) -> Result<ThumbnailParams>;
    auto parseDto(const Json& params, DtoTag<CreateEntityParams>) -> Result<CreateEntityParams>;
    auto parseDto(const Json& params, DtoTag<EntityParams>) -> Result<EntityParams>;
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
    auto parseDto(const Json& params, DtoTag<SetProbesParams>) -> Result<SetProbesParams>;
    auto parseDto(const Json& params, DtoTag<SetExposureParams>) -> Result<SetExposureParams>;
}
