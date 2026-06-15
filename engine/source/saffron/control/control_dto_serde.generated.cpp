// GENERATED - do not edit.
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

namespace sa
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

        auto readAddEntityPreset(const Json& value, std::string_view key) -> Result<AddEntityPreset>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
            if (text == "empty") { return AddEntityPreset::Empty; }
            if (text == "cube") { return AddEntityPreset::Cube; }
            if (text == "model") { return AddEntityPreset::Model; }
            if (text == "point-light") { return AddEntityPreset::PointLight; }
            if (text == "spot-light") { return AddEntityPreset::SpotLight; }
            if (text == "directional-light") { return AddEntityPreset::DirectionalLight; }
            if (text == "camera") { return AddEntityPreset::Camera; }
            if (text == "reflection-probe") { return AddEntityPreset::ReflectionProbe; }
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto AddEntityPresetName(AddEntityPreset value) -> const char*
        {
            switch (value)
            {
            case AddEntityPreset::Empty: return "empty";
            case AddEntityPreset::Cube: return "cube";
            case AddEntityPreset::Model: return "model";
            case AddEntityPreset::PointLight: return "point-light";
            case AddEntityPreset::SpotLight: return "spot-light";
            case AddEntityPreset::DirectionalLight: return "directional-light";
            case AddEntityPreset::Camera: return "camera";
            case AddEntityPreset::ReflectionProbe: return "reflection-probe";
            }
            return "";
        }

        auto readPickKind(const Json& value, std::string_view key) -> Result<PickKind>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
            if (text == "billboard") { return PickKind::Billboard; }
            if (text == "mesh") { return PickKind::Mesh; }
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto PickKindName(PickKind value) -> const char*
        {
            switch (value)
            {
            case PickKind::Billboard: return "billboard";
            case PickKind::Mesh: return "mesh";
            }
            return "";
        }

        auto readGizmoOpDto(const Json& value, std::string_view key) -> Result<GizmoOpDto>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
            if (text == "translate") { return GizmoOpDto::Translate; }
            if (text == "rotate") { return GizmoOpDto::Rotate; }
            if (text == "scale") { return GizmoOpDto::Scale; }
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto GizmoOpDtoName(GizmoOpDto value) -> const char*
        {
            switch (value)
            {
            case GizmoOpDto::Translate: return "translate";
            case GizmoOpDto::Rotate: return "rotate";
            case GizmoOpDto::Scale: return "scale";
            }
            return "";
        }

        auto readGizmoSpaceDto(const Json& value, std::string_view key) -> Result<GizmoSpaceDto>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
            if (text == "world") { return GizmoSpaceDto::World; }
            if (text == "local") { return GizmoSpaceDto::Local; }
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto GizmoSpaceDtoName(GizmoSpaceDto value) -> const char*
        {
            switch (value)
            {
            case GizmoSpaceDto::World: return "world";
            case GizmoSpaceDto::Local: return "local";
            }
            return "";
        }

        auto readGizmoPointerPhase(const Json& value, std::string_view key) -> Result<GizmoPointerPhase>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
            if (text == "hover") { return GizmoPointerPhase::Hover; }
            if (text == "begin") { return GizmoPointerPhase::Begin; }
            if (text == "drag") { return GizmoPointerPhase::Drag; }
            if (text == "end") { return GizmoPointerPhase::End; }
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto GizmoPointerPhaseName(GizmoPointerPhase value) -> const char*
        {
            switch (value)
            {
            case GizmoPointerPhase::Hover: return "hover";
            case GizmoPointerPhase::Begin: return "begin";
            case GizmoPointerPhase::Drag: return "drag";
            case GizmoPointerPhase::End: return "end";
            }
            return "";
        }

        auto readAaModeDto(const Json& value, std::string_view key) -> Result<AaModeDto>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
            if (text == "off") { return AaModeDto::Off; }
            if (text == "fxaa") { return AaModeDto::Fxaa; }
            if (text == "taa") { return AaModeDto::Taa; }
            if (text == "msaa2") { return AaModeDto::Msaa2; }
            if (text == "msaa4") { return AaModeDto::Msaa4; }
            if (text == "msaa8") { return AaModeDto::Msaa8; }
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto AaModeDtoName(AaModeDto value) -> const char*
        {
            switch (value)
            {
            case AaModeDto::Off: return "off";
            case AaModeDto::Fxaa: return "fxaa";
            case AaModeDto::Taa: return "taa";
            case AaModeDto::Msaa2: return "msaa2";
            case AaModeDto::Msaa4: return "msaa4";
            case AaModeDto::Msaa8: return "msaa8";
            }
            return "";
        }

        auto readGiModeDto(const Json& value, std::string_view key) -> Result<GiModeDto>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
            if (text == "off") { return GiModeDto::Off; }
            if (text == "ddgi") { return GiModeDto::Ddgi; }
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto GiModeDtoName(GiModeDto value) -> const char*
        {
            switch (value)
            {
            case GiModeDto::Off: return "off";
            case GiModeDto::Ddgi: return "ddgi";
            }
            return "";
        }

        auto readViewModeDto(const Json& value, std::string_view key) -> Result<ViewModeDto>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
            if (text == "lit") { return ViewModeDto::Lit; }
            if (text == "wireframe") { return ViewModeDto::Wireframe; }
            if (text == "albedo") { return ViewModeDto::Albedo; }
            if (text == "normal") { return ViewModeDto::Normal; }
            if (text == "roughness") { return ViewModeDto::Roughness; }
            if (text == "metallic") { return ViewModeDto::Metallic; }
            if (text == "emissive") { return ViewModeDto::Emissive; }
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto ViewModeDtoName(ViewModeDto value) -> const char*
        {
            switch (value)
            {
            case ViewModeDto::Lit: return "lit";
            case ViewModeDto::Wireframe: return "wireframe";
            case ViewModeDto::Albedo: return "albedo";
            case ViewModeDto::Normal: return "normal";
            case ViewModeDto::Roughness: return "roughness";
            case ViewModeDto::Metallic: return "metallic";
            case ViewModeDto::Emissive: return "emissive";
            }
            return "";
        }

        auto readAssetSlotDto(const Json& value, std::string_view key) -> Result<AssetSlotDto>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
            if (text == "mesh") { return AssetSlotDto::Mesh; }
            if (text == "albedo") { return AssetSlotDto::Albedo; }
            if (text == "metallic-roughness") { return AssetSlotDto::MetallicRoughness; }
            if (text == "normal") { return AssetSlotDto::Normal; }
            if (text == "occlusion") { return AssetSlotDto::Occlusion; }
            if (text == "emissive") { return AssetSlotDto::Emissive; }
            if (text == "height") { return AssetSlotDto::Height; }
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto AssetSlotDtoName(AssetSlotDto value) -> const char*
        {
            switch (value)
            {
            case AssetSlotDto::Mesh: return "mesh";
            case AssetSlotDto::Albedo: return "albedo";
            case AssetSlotDto::MetallicRoughness: return "metallic-roughness";
            case AssetSlotDto::Normal: return "normal";
            case AssetSlotDto::Occlusion: return "occlusion";
            case AssetSlotDto::Emissive: return "emissive";
            case AssetSlotDto::Height: return "height";
            }
            return "";
        }

        auto readScreenshotTargetDto(const Json& value, std::string_view key) -> Result<ScreenshotTargetDto>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
            if (text == "viewport") { return ScreenshotTargetDto::Viewport; }
            if (text == "window") { return ScreenshotTargetDto::Window; }
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto ScreenshotTargetDtoName(ScreenshotTargetDto value) -> const char*
        {
            switch (value)
            {
            case ScreenshotTargetDto::Viewport: return "viewport";
            case ScreenshotTargetDto::Window: return "window";
            }
            return "";
        }

        auto readAssetTypeDto(const Json& value, std::string_view key) -> Result<AssetTypeDto>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
            if (text == "mesh") { return AssetTypeDto::Mesh; }
            if (text == "texture") { return AssetTypeDto::Texture; }
            if (text == "other") { return AssetTypeDto::Other; }
            if (text == "animation") { return AssetTypeDto::Animation; }
            if (text == "material") { return AssetTypeDto::Material; }
            if (text == "model") { return AssetTypeDto::Model; }
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto AssetTypeDtoName(AssetTypeDto value) -> const char*
        {
            switch (value)
            {
            case AssetTypeDto::Mesh: return "mesh";
            case AssetTypeDto::Texture: return "texture";
            case AssetTypeDto::Other: return "other";
            case AssetTypeDto::Animation: return "animation";
            case AssetTypeDto::Material: return "material";
            case AssetTypeDto::Model: return "model";
            }
            return "";
        }

        auto readProfilerModeDto(const Json& value, std::string_view key) -> Result<ProfilerModeDto>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
            if (text == "off") { return ProfilerModeDto::Off; }
            if (text == "timestamps") { return ProfilerModeDto::Timestamps; }
            if (text == "pipeline-stats") { return ProfilerModeDto::PipelineStats; }
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto ProfilerModeDtoName(ProfilerModeDto value) -> const char*
        {
            switch (value)
            {
            case ProfilerModeDto::Off: return "off";
            case ProfilerModeDto::Timestamps: return "timestamps";
            case ProfilerModeDto::PipelineStats: return "pipeline-stats";
            }
            return "";
        }

        auto readProfileLaneDto(const Json& value, std::string_view key) -> Result<ProfileLaneDto>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
            if (text == "cpu") { return ProfileLaneDto::Cpu; }
            if (text == "gpu") { return ProfileLaneDto::Gpu; }
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto ProfileLaneDtoName(ProfileLaneDto value) -> const char*
        {
            switch (value)
            {
            case ProfileLaneDto::Cpu: return "cpu";
            case ProfileLaneDto::Gpu: return "gpu";
            }
            return "";
        }

        auto readCaptureModeDto(const Json& value, std::string_view key) -> Result<CaptureModeDto>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
            if (text == "single") { return CaptureModeDto::Single; }
            if (text == "frames") { return CaptureModeDto::Frames; }
            if (text == "rolling") { return CaptureModeDto::Rolling; }
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto CaptureModeDtoName(CaptureModeDto value) -> const char*
        {
            switch (value)
            {
            case CaptureModeDto::Single: return "single";
            case CaptureModeDto::Frames: return "frames";
            case CaptureModeDto::Rolling: return "rolling";
            }
            return "";
        }

        auto readCaptureStateDto(const Json& value, std::string_view key) -> Result<CaptureStateDto>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
            if (text == "idle") { return CaptureStateDto::Idle; }
            if (text == "arming") { return CaptureStateDto::Arming; }
            if (text == "recording") { return CaptureStateDto::Recording; }
            if (text == "ready") { return CaptureStateDto::Ready; }
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto CaptureStateDtoName(CaptureStateDto value) -> const char*
        {
            switch (value)
            {
            case CaptureStateDto::Idle: return "idle";
            case CaptureStateDto::Arming: return "arming";
            case CaptureStateDto::Recording: return "recording";
            case CaptureStateDto::Ready: return "ready";
            }
            return "";
        }

        auto readAlarmSeverityDto(const Json& value, std::string_view key) -> Result<AlarmSeverityDto>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
            if (text == "info") { return AlarmSeverityDto::Info; }
            if (text == "warning") { return AlarmSeverityDto::Warning; }
            if (text == "critical") { return AlarmSeverityDto::Critical; }
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto AlarmSeverityDtoName(AlarmSeverityDto value) -> const char*
        {
            switch (value)
            {
            case AlarmSeverityDto::Info: return "info";
            case AlarmSeverityDto::Warning: return "warning";
            case AlarmSeverityDto::Critical: return "critical";
            }
            return "";
        }

        auto readAlarmStateDto(const Json& value, std::string_view key) -> Result<AlarmStateDto>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
            if (text == "firing") { return AlarmStateDto::Firing; }
            if (text == "resolved") { return AlarmStateDto::Resolved; }
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto AlarmStateDtoName(AlarmStateDto value) -> const char*
        {
            switch (value)
            {
            case AlarmStateDto::Firing: return "firing";
            case AlarmStateDto::Resolved: return "resolved";
            }
            return "";
        }

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

    auto dtoToJson(AddEntityPreset value) -> Json
    {
        return AddEntityPresetName(value);
    }

    auto dtoToJson(PickKind value) -> Json
    {
        return PickKindName(value);
    }

    auto dtoToJson(GizmoOpDto value) -> Json
    {
        return GizmoOpDtoName(value);
    }

    auto dtoToJson(GizmoSpaceDto value) -> Json
    {
        return GizmoSpaceDtoName(value);
    }

    auto dtoToJson(GizmoPointerPhase value) -> Json
    {
        return GizmoPointerPhaseName(value);
    }

    auto dtoToJson(AaModeDto value) -> Json
    {
        return AaModeDtoName(value);
    }

    auto dtoToJson(GiModeDto value) -> Json
    {
        return GiModeDtoName(value);
    }

    auto dtoToJson(ViewModeDto value) -> Json
    {
        return ViewModeDtoName(value);
    }

    auto dtoToJson(AssetSlotDto value) -> Json
    {
        return AssetSlotDtoName(value);
    }

    auto dtoToJson(ScreenshotTargetDto value) -> Json
    {
        return ScreenshotTargetDtoName(value);
    }

    auto dtoToJson(AssetTypeDto value) -> Json
    {
        return AssetTypeDtoName(value);
    }

    auto dtoToJson(ProfilerModeDto value) -> Json
    {
        return ProfilerModeDtoName(value);
    }

    auto dtoToJson(ProfileLaneDto value) -> Json
    {
        return ProfileLaneDtoName(value);
    }

    auto dtoToJson(CaptureModeDto value) -> Json
    {
        return CaptureModeDtoName(value);
    }

    auto dtoToJson(CaptureStateDto value) -> Json
    {
        return CaptureStateDtoName(value);
    }

    auto dtoToJson(AlarmSeverityDto value) -> Json
    {
        return AlarmSeverityDtoName(value);
    }

    auto dtoToJson(AlarmStateDto value) -> Json
    {
        return AlarmStateDtoName(value);
    }

    auto parseDto(const Json& params, DtoTag<PingParams>) -> Result<PingParams>
    {
        PingParams out;

        return out;
    }

    auto parseDto(const Json& params, DtoTag<EmptyParams>) -> Result<EmptyParams>
    {
        EmptyParams out;

        return out;
    }

    auto parseDto(const Json& params, DtoTag<ProfilerSetModeParams>) -> Result<ProfilerSetModeParams>
    {
        ProfilerSetModeParams out;

        {
            auto value = optionalField(params, "mode", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readProfilerModeDto(*value, "mode");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.mode = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<CaptureStartParams>) -> Result<CaptureStartParams>
    {
        CaptureStartParams out;

        {
            auto value = optionalField(params, "mode", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readCaptureModeDto(*value, "mode");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.mode = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "frames", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readI32(*value, "frames");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.frames = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "filter", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readString(*value, "filter");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.filter = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "includeCpu", 3, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "includeCpu");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.includeCpu = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "includePipelineStats", 4, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "includePipelineStats");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.includePipelineStats = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<FrameHistoryParams>) -> Result<FrameHistoryParams>
    {
        FrameHistoryParams out;

        {
            auto value = optionalField(params, "samples", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readI32(*value, "samples");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.samples = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetPerfConfigParams>) -> Result<SetPerfConfigParams>
    {
        SetPerfConfigParams out;

        {
            auto value = optionalField(params, "targetFps", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "targetFps");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.targetFps = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "greenBudgetFrac", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "greenBudgetFrac");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.greenBudgetFrac = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "greenMedianMul", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "greenMedianMul");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.greenMedianMul = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "amberMedianMul", 3, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "amberMedianMul");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.amberMedianMul = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "frozenMs", 4, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "frozenMs");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.frozenMs = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "vramWarnFrac", 5, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "vramWarnFrac");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.vramWarnFrac = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "vramCritFrac", 6, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "vramCritFrac");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.vramCritFrac = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<DrainAlarmsParams>) -> Result<DrainAlarmsParams>
    {
        DrainAlarmsParams out;

        {
            auto value = optionalField(params, "since", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readI64(*value, "since");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.since = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetAaParams>) -> Result<SetAaParams>
    {
        SetAaParams out;

        {
            auto value = optionalField(params, "mode", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readAaModeDto(*value, "mode");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.mode = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetViewModeParams>) -> Result<SetViewModeParams>
    {
        SetViewModeParams out;

        {
            auto value = optionalField(params, "mode", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readViewModeDto(*value, "mode");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.mode = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<ToggleParams>) -> Result<ToggleParams>
    {
        ToggleParams out;

        {
            auto value = optionalField(params, "enabled", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "enabled");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.enabled = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetGiParams>) -> Result<SetGiParams>
    {
        SetGiParams out;

        {
            auto value = requiredField(params, "mode", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readGiModeDto(**value, "mode");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.mode = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetViewportSizeParams>) -> Result<SetViewportSizeParams>
    {
        SetViewportSizeParams out;

        {
            auto value = optionalField(params, "view", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readString(*value, "view");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.view = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "width", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readI32(*value, "width");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.width = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "height", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readI32(*value, "height");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.height = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<CreateEntityParams>) -> Result<CreateEntityParams>
    {
        CreateEntityParams out;

        {
            auto value = requiredField(params, "name", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "name");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.name = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<EntityParams>) -> Result<EntityParams>
    {
        EntityParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetParentParams>) -> Result<SetParentParams>
    {
        SetParentParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "parent", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readEntitySelector(*value, "parent");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.parent = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<ComponentParams>) -> Result<ComponentParams>
    {
        ComponentParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "component", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "component");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.component = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetComponentParams>) -> Result<SetComponentParams>
    {
        SetComponentParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "component", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "component");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.component = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "json", 2, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readJson(**value, "json");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.json = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetTransformParams>) -> Result<SetTransformParams>
    {
        SetTransformParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "translation", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = parseDto(*value, DtoTag<Vec3>{});
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.translation = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "rotation", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = parseDto(*value, DtoTag<Vec3>{});
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.rotation = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "scale", 3, true);
            if (value && !value->is_null())
            {
                auto parsed = parseDto(*value, DtoTag<Vec3>{});
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.scale = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "smooth", 4, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "smooth");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.smooth = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<Vec3>) -> Result<Vec3>
    {
        Vec3 out;

        {
            auto value = requiredField(params, "x", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readF32(**value, "x");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.x = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "y", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readF32(**value, "y");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.y = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "z", 2, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readF32(**value, "z");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.z = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetMaterialParams>) -> Result<SetMaterialParams>
    {
        SetMaterialParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "baseColor", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = parseDto(*value, DtoTag<Vec4>{});
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.baseColor = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "albedoTexture", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readWireUuid(*value, "albedoTexture");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.albedoTexture = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "metallicRoughnessTexture", 3, true);
            if (value && !value->is_null())
            {
                auto parsed = readWireUuid(*value, "metallicRoughnessTexture");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.metallicRoughnessTexture = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "metallic", 4, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "metallic");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.metallic = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "roughness", 5, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "roughness");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.roughness = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "emissive", 6, true);
            if (value && !value->is_null())
            {
                auto parsed = parseDto(*value, DtoTag<Vec3>{});
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.emissive = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "emissiveStrength", 7, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "emissiveStrength");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.emissiveStrength = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "unlit", 8, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "unlit");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.unlit = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "slot", 9, true);
            if (value && !value->is_null())
            {
                auto parsed = readU32(*value, "slot");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.slot = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "smooth", 10, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "smooth");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.smooth = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<Vec4>) -> Result<Vec4>
    {
        Vec4 out;

        {
            auto value = requiredField(params, "x", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readF32(**value, "x");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.x = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "y", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readF32(**value, "y");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.y = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "z", 2, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readF32(**value, "z");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.z = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "w", 3, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readF32(**value, "w");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.w = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetLightParams>) -> Result<SetLightParams>
    {
        SetLightParams out;

        {
            auto value = optionalField(params, "entity", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readEntitySelector(*value, "entity");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.entity = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "direction", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = parseDto(*value, DtoTag<Vec3>{});
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.direction = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "color", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = parseDto(*value, DtoTag<Vec3>{});
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.color = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "intensity", 3, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "intensity");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.intensity = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "ambient", 4, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "ambient");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.ambient = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<PickParams>) -> Result<PickParams>
    {
        PickParams out;

        {
            auto value = optionalField(params, "u", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "u");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.u = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "v", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "v");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.v = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetEnvironmentParams>) -> Result<SetEnvironmentParams>
    {
        SetEnvironmentParams out;

        {
            auto value = optionalField(params, "json", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readJson(*value, "json");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.json = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "skyMode", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readString(*value, "skyMode");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.skyMode = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "clearColor", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = parseDto(*value, DtoTag<Vec3>{});
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.clearColor = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "skyTexture", 3, true);
            if (value && !value->is_null())
            {
                auto parsed = readWireUuid(*value, "skyTexture");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.skyTexture = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "skyIntensity", 4, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "skyIntensity");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.skyIntensity = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "skyRotation", 5, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "skyRotation");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.skyRotation = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "exposure", 6, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "exposure");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.exposure = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "visible", 7, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "visible");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.visible = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "useSkyForAmbient", 8, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "useSkyForAmbient");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.useSkyForAmbient = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "ambientColor", 9, true);
            if (value && !value->is_null())
            {
                auto parsed = parseDto(*value, DtoTag<Vec3>{});
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.ambientColor = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "ambientIntensity", 10, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "ambientIntensity");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.ambientIntensity = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetAtmosphereParams>) -> Result<SetAtmosphereParams>
    {
        SetAtmosphereParams out;

        {
            auto value = optionalField(params, "json", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readJson(*value, "json");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.json = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "enabled", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "enabled");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.enabled = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "planetRadius", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "planetRadius");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.planetRadius = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "atmosphereHeight", 3, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "atmosphereHeight");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.atmosphereHeight = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "rayleighScattering", 4, true);
            if (value && !value->is_null())
            {
                auto parsed = parseDto(*value, DtoTag<Vec3>{});
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.rayleighScattering = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "rayleighScaleHeight", 5, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "rayleighScaleHeight");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.rayleighScaleHeight = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "mieScattering", 6, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "mieScattering");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.mieScattering = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "mieScaleHeight", 7, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "mieScaleHeight");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.mieScaleHeight = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "mieAnisotropy", 8, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "mieAnisotropy");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.mieAnisotropy = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "ozoneAbsorption", 9, true);
            if (value && !value->is_null())
            {
                auto parsed = parseDto(*value, DtoTag<Vec3>{});
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.ozoneAbsorption = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "sunDiskAngularRadius", 10, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "sunDiskAngularRadius");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.sunDiskAngularRadius = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "sunDiskIntensity", 11, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "sunDiskIntensity");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.sunDiskIntensity = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<StepParams>) -> Result<StepParams>
    {
        StepParams out;

        {
            auto value = optionalField(params, "frames", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readI32(*value, "frames");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.frames = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<AnimationStateParams>) -> Result<AnimationStateParams>
    {
        AnimationStateParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<ListClipsParams>) -> Result<ListClipsParams>
    {
        ListClipsParams out;

        {
            auto value = optionalField(params, "asset", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readAssetSelector(*value, "asset");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.asset = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<PlayAnimationParams>) -> Result<PlayAnimationParams>
    {
        PlayAnimationParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "clip", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "clip");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.clip = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "speed", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "speed");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.speed = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "loop", 3, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "loop");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.loop = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "blend", 4, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "blend");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.blend = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "paused", 5, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "paused");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.paused = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetAnimationPlayingParams>) -> Result<SetAnimationPlayingParams>
    {
        SetAnimationPlayingParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "playing", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readBool(**value, "playing");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.playing = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SeekAnimationParams>) -> Result<SeekAnimationParams>
    {
        SeekAnimationParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "time", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readF32(**value, "time");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.time = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "seekBlend", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "seekBlend");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.seekBlend = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetAnimationLoopParams>) -> Result<SetAnimationLoopParams>
    {
        SetAnimationLoopParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "wrap", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "wrap");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.wrap = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetSkeletonOverlayParams>) -> Result<SetSkeletonOverlayParams>
    {
        SetSkeletonOverlayParams out;

        {
            auto value = optionalField(params, "show", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "show");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.show = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "axes", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "axes");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.axes = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "jointSize", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "jointSize");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.jointSize = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<DebugOverlaysParams>) -> Result<DebugOverlaysParams>
    {
        DebugOverlaysParams out;

        {
            auto value = optionalField(params, "bounds", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "bounds");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.bounds = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "sceneAabb", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "sceneAabb");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.sceneAabb = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "lightVolumes", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "lightVolumes");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.lightVolumes = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "grid", 3, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "grid");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.grid = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "colliders", 4, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "colliders");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.colliders = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetSkeletonHighlightParams>) -> Result<SetSkeletonHighlightParams>
    {
        SetSkeletonHighlightParams out;

        {
            auto value = requiredField(params, "joint", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readI32(**value, "joint");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.joint = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<PickSkeletonJointParams>) -> Result<PickSkeletonJointParams>
    {
        PickSkeletonJointParams out;

        {
            auto value = requiredField(params, "u", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readF32(**value, "u");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.u = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "v", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readF32(**value, "v");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.v = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "radiusPx", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "radiusPx");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.radiusPx = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetAssetPreviewOptionsParams>) -> Result<SetAssetPreviewOptionsParams>
    {
        SetAssetPreviewOptionsParams out;

        {
            auto value = optionalField(params, "floor", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "floor");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.floor = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<GetFootIkParams>) -> Result<GetFootIkParams>
    {
        GetFootIkParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetFootIkParams>) -> Result<SetFootIkParams>
    {
        SetFootIkParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "enabled", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "enabled");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.enabled = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "groundHeight", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "groundHeight");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.groundHeight = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<FitColliderParams>) -> Result<FitColliderParams>
    {
        FitColliderParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<ApplyImpulseParams>) -> Result<ApplyImpulseParams>
    {
        ApplyImpulseParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "impulse", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = parseDto(**value, DtoTag<Vec3>{});
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.impulse = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<DrainContactsParams>) -> Result<DrainContactsParams>
    {
        DrainContactsParams out;

        {
            auto value = optionalField(params, "since", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readI64(*value, "since");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.since = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetKinematicBonesParams>) -> Result<SetKinematicBonesParams>
    {
        SetKinematicBonesParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "enabled", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "enabled");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.enabled = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<MoveCharacterParams>) -> Result<MoveCharacterParams>
    {
        MoveCharacterParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "velocity", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = parseDto(**value, DtoTag<Vec3>{});
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.velocity = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "jump", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "jump");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.jump = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<RaycastParams>) -> Result<RaycastParams>
    {
        RaycastParams out;

        {
            auto value = requiredField(params, "origin", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = parseDto(**value, DtoTag<Vec3>{});
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.origin = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "dir", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = parseDto(**value, DtoTag<Vec3>{});
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.dir = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "maxDist", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "maxDist");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.maxDist = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<ShapecastParams>) -> Result<ShapecastParams>
    {
        ShapecastParams out;

        {
            auto value = requiredField(params, "origin", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = parseDto(**value, DtoTag<Vec3>{});
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.origin = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "dir", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = parseDto(**value, DtoTag<Vec3>{});
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.dir = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "radius", 2, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readF32(**value, "radius");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.radius = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "maxDist", 3, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "maxDist");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.maxDist = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<EnableRagdollParams>) -> Result<EnableRagdollParams>
    {
        EnableRagdollParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "enabled", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "enabled");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.enabled = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetRagdollParams>) -> Result<SetRagdollParams>
    {
        SetRagdollParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "active", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "active");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.active = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "bodyWeight", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "bodyWeight");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.bodyWeight = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "bone", 3, true);
            if (value && !value->is_null())
            {
                auto parsed = readI32(*value, "bone");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.bone = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "weight", 4, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "weight");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.weight = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<GetRagdollParams>) -> Result<GetRagdollParams>
    {
        GetRagdollParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<DrainScriptErrorsParams>) -> Result<DrainScriptErrorsParams>
    {
        DrainScriptErrorsParams out;

        {
            auto value = optionalField(params, "since", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readI64(*value, "since");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.since = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<DrainScriptLogsParams>) -> Result<DrainScriptLogsParams>
    {
        DrainScriptLogsParams out;

        {
            auto value = optionalField(params, "since", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readI64(*value, "since");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.since = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<GetScriptSchemaParams>) -> Result<GetScriptSchemaParams>
    {
        GetScriptSchemaParams out;

        {
            auto value = requiredField(params, "path", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "path");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.path = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetScriptOverrideParams>) -> Result<SetScriptOverrideParams>
    {
        SetScriptOverrideParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "slot", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readI32(**value, "slot");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.slot = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "name", 2, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "name");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.name = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "value", 3, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readJson(**value, "value");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.value = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<AddEntityParams>) -> Result<AddEntityParams>
    {
        AddEntityParams out;

        {
            auto value = optionalField(params, "preset", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readAddEntityPreset(*value, "preset");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.preset = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<RenameEntityParams>) -> Result<RenameEntityParams>
    {
        RenameEntityParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "name", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "name");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.name = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetComponentFieldParams>) -> Result<SetComponentFieldParams>
    {
        SetComponentFieldParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "component", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "component");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.component = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "field", 2, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "field");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.field = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "value", 3, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readJson(**value, "value");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.value = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "index", 4, true);
            if (value && !value->is_null())
            {
                auto parsed = readI32(*value, "index");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.index = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetCameraParams>) -> Result<SetCameraParams>
    {
        SetCameraParams out;

        {
            auto value = optionalField(params, "position", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = parseDto(*value, DtoTag<Vec3>{});
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.position = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "yaw", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "yaw");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.yaw = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "pitch", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "pitch");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.pitch = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "fov", 3, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "fov");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.fov = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "near", 4, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "near");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.near = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "far", 5, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "far");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.far = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "moveSpeed", 6, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "moveSpeed");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.moveSpeed = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "lookSpeed", 7, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "lookSpeed");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.lookSpeed = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetGizmoParams>) -> Result<SetGizmoParams>
    {
        SetGizmoParams out;

        {
            auto value = optionalField(params, "op", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readGizmoOpDto(*value, "op");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.op = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "space", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readGizmoSpaceDto(*value, "space");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.space = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "preserveChildren", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "preserveChildren");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.preserveChildren = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<GizmoPointerParams>) -> Result<GizmoPointerParams>
    {
        GizmoPointerParams out;

        {
            auto value = optionalField(params, "phase", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readGizmoPointerPhase(*value, "phase");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.phase = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "x", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "x");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.x = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "y", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "y");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.y = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<FlyInputParams>) -> Result<FlyInputParams>
    {
        FlyInputParams out;

        {
            auto value = optionalField(params, "active", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "active");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.active = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "lookDx", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "lookDx");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.lookDx = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "lookDy", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "lookDy");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.lookDy = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "forward", 3, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "forward");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.forward = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "back", 4, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "back");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.back = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "left", 5, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "left");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.left = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "right", 6, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "right");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.right = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "up", 7, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "up");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.up = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "down", 8, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "down");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.down = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<ScriptInputParams>) -> Result<ScriptInputParams>
    {
        ScriptInputParams out;

        {
            auto value = requiredField(params, "keys", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readVector<std::string>(**value, "keys");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.keys = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "mouseButtons", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readVector<std::string>(*value, "mouseButtons");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.mouseButtons = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "mouseX", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "mouseX");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.mouseX = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "mouseY", 3, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "mouseY");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.mouseY = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "scroll", 4, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "scroll");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.scroll = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetProbesParams>) -> Result<SetProbesParams>
    {
        SetProbesParams out;

        {
            auto value = optionalField(params, "enabled", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "enabled");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.enabled = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetExposureParams>) -> Result<SetExposureParams>
    {
        SetExposureParams out;

        {
            auto value = requiredField(params, "ev", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readF32(**value, "ev");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.ev = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<NewProjectParams>) -> Result<NewProjectParams>
    {
        NewProjectParams out;

        {
            auto value = optionalField(params, "name", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readString(*value, "name");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.name = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "displayName", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readString(*value, "displayName");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.displayName = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "root", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readString(*value, "root");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.root = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<CreateScriptParams>) -> Result<CreateScriptParams>
    {
        CreateScriptParams out;

        {
            auto value = requiredField(params, "name", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "name");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.name = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<PathParams>) -> Result<PathParams>
    {
        PathParams out;

        {
            auto value = requiredField(params, "path", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "path");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.path = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<InstantiateModelParams>) -> Result<InstantiateModelParams>
    {
        InstantiateModelParams out;

        {
            auto value = requiredField(params, "asset", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "asset");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.asset = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "name", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readString(*value, "name");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.name = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<ExtractSubAssetParams>) -> Result<ExtractSubAssetParams>
    {
        ExtractSubAssetParams out;

        {
            auto value = requiredField(params, "asset", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "asset");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.asset = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "subAsset", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readWireUuid(**value, "subAsset");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.subAsset = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "dest", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readString(*value, "dest");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.dest = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<ClearExtractionParams>) -> Result<ClearExtractionParams>
    {
        ClearExtractionParams out;

        {
            auto value = requiredField(params, "asset", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "asset");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.asset = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "subAsset", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readWireUuid(**value, "subAsset");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.subAsset = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<ReimportModelParams>) -> Result<ReimportModelParams>
    {
        ReimportModelParams out;

        {
            auto value = requiredField(params, "asset", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "asset");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.asset = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<ModelInfoParams>) -> Result<ModelInfoParams>
    {
        ModelInfoParams out;

        {
            auto value = requiredField(params, "asset", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "asset");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.asset = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<AssetReferencesParams>) -> Result<AssetReferencesParams>
    {
        AssetReferencesParams out;

        {
            auto value = requiredField(params, "asset", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "asset");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.asset = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<GetAssetModelParams>) -> Result<GetAssetModelParams>
    {
        GetAssetModelParams out;

        {
            auto value = requiredField(params, "asset", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "asset");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.asset = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<EnterAssetPreviewParams>) -> Result<EnterAssetPreviewParams>
    {
        EnterAssetPreviewParams out;

        {
            auto value = requiredField(params, "asset", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "asset");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.asset = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<SetActiveViewParams>) -> Result<SetActiveViewParams>
    {
        SetActiveViewParams out;

        {
            auto value = requiredField(params, "view", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "view");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.view = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<CleanAssetsParams>) -> Result<CleanAssetsParams>
    {
        CleanAssetsParams out;

        {
            auto value = optionalField(params, "dryRun", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "dryRun");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.dryRun = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "exclude", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readVector<std::string>(*value, "exclude");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.exclude = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<DeleteUnusedParams>) -> Result<DeleteUnusedParams>
    {
        DeleteUnusedParams out;

        {
            auto value = requiredField(params, "ids", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readVector<std::string>(**value, "ids");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.ids = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "confirm", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "confirm");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.confirm = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<RenameAssetParams>) -> Result<RenameAssetParams>
    {
        RenameAssetParams out;

        {
            auto value = requiredField(params, "asset", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "asset");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.asset = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "name", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "name");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.name = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<CreateAssetFolderParams>) -> Result<CreateAssetFolderParams>
    {
        CreateAssetFolderParams out;

        {
            auto value = requiredField(params, "folder", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "folder");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.folder = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<RenameAssetFolderParams>) -> Result<RenameAssetFolderParams>
    {
        RenameAssetFolderParams out;

        {
            auto value = requiredField(params, "folder", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "folder");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.folder = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "name", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "name");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.name = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<DeleteAssetFolderParams>) -> Result<DeleteAssetFolderParams>
    {
        DeleteAssetFolderParams out;

        {
            auto value = requiredField(params, "folder", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "folder");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.folder = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<MoveAssetParams>) -> Result<MoveAssetParams>
    {
        MoveAssetParams out;

        {
            auto value = requiredField(params, "asset", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "asset");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.asset = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "folder", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readString(*value, "folder");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.folder = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<AssetUsagesParams>) -> Result<AssetUsagesParams>
    {
        AssetUsagesParams out;

        {
            auto value = requiredField(params, "asset", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "asset");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.asset = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<AssetMetadataParams>) -> Result<AssetMetadataParams>
    {
        AssetMetadataParams out;

        {
            auto value = requiredField(params, "asset", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "asset");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.asset = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<DeleteAssetParams>) -> Result<DeleteAssetParams>
    {
        DeleteAssetParams out;

        {
            auto value = requiredField(params, "asset", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "asset");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.asset = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<AssignAssetParams>) -> Result<AssignAssetParams>
    {
        AssignAssetParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "slot", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSlotDto(**value, "slot");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.slot = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "asset", 2, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "asset");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.asset = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<MaterialCreateParams>) -> Result<MaterialCreateParams>
    {
        MaterialCreateParams out;

        {
            auto value = requiredField(params, "name", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "name");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.name = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<MaterialAssignParams>) -> Result<MaterialAssignParams>
    {
        MaterialAssignParams out;

        {
            auto value = requiredField(params, "entity", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readEntitySelector(**value, "entity");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.entity = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "material", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "material");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.material = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<MaterialImportParams>) -> Result<MaterialImportParams>
    {
        MaterialImportParams out;

        {
            auto value = requiredField(params, "path", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "path");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.path = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "name", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "name");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.name = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<MaterialGetParams>) -> Result<MaterialGetParams>
    {
        MaterialGetParams out;

        {
            auto value = requiredField(params, "material", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "material");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.material = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<MaterialUpdateParams>) -> Result<MaterialUpdateParams>
    {
        MaterialUpdateParams out;

        {
            auto value = requiredField(params, "material", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "material");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.material = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "baseColor", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = parseDto(*value, DtoTag<Vec4>{});
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.baseColor = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "metallic", 2, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "metallic");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.metallic = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "roughness", 3, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "roughness");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.roughness = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "emissive", 4, true);
            if (value && !value->is_null())
            {
                auto parsed = parseDto(*value, DtoTag<Vec3>{});
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.emissive = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "emissiveStrength", 5, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "emissiveStrength");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.emissiveStrength = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "normalStrength", 6, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "normalStrength");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.normalStrength = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "albedoTexture", 7, true);
            if (value && !value->is_null())
            {
                auto parsed = readWireUuid(*value, "albedoTexture");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.albedoTexture = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "ormTexture", 8, true);
            if (value && !value->is_null())
            {
                auto parsed = readWireUuid(*value, "ormTexture");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.ormTexture = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "normalTexture", 9, true);
            if (value && !value->is_null())
            {
                auto parsed = readWireUuid(*value, "normalTexture");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.normalTexture = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "emissiveTexture", 10, true);
            if (value && !value->is_null())
            {
                auto parsed = readWireUuid(*value, "emissiveTexture");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.emissiveTexture = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "heightTexture", 11, true);
            if (value && !value->is_null())
            {
                auto parsed = readWireUuid(*value, "heightTexture");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.heightTexture = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<PreviewRenderParams>) -> Result<PreviewRenderParams>
    {
        PreviewRenderParams out;

        {
            auto value = requiredField(params, "material", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "material");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.material = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "size", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readU32(*value, "size");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.size = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<MaterialSetGraphParams>) -> Result<MaterialSetGraphParams>
    {
        MaterialSetGraphParams out;

        {
            auto value = requiredField(params, "material", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "material");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.material = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "graph", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readJson(**value, "graph");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.graph = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<MaterialCreateInstanceParams>) -> Result<MaterialCreateInstanceParams>
    {
        MaterialCreateInstanceParams out;

        {
            auto value = requiredField(params, "parent", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "parent");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.parent = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "name", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "name");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.name = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<MaterialSetOverrideParams>) -> Result<MaterialSetOverrideParams>
    {
        MaterialSetOverrideParams out;

        {
            auto value = requiredField(params, "material", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "material");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.material = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "field", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "field");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.field = std::move(*parsed);
        }

        {
            auto value = requiredField(params, "value", 2, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readJson(**value, "value");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.value = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<MaterialCompileParams>) -> Result<MaterialCompileParams>
    {
        MaterialCompileParams out;

        {
            auto value = requiredField(params, "material", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "material");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.material = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<OptionalPathParams>) -> Result<OptionalPathParams>
    {
        OptionalPathParams out;

        {
            auto value = optionalField(params, "path", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readString(*value, "path");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.path = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<ScreenshotParams>) -> Result<ScreenshotParams>
    {
        ScreenshotParams out;

        {
            auto value = optionalField(params, "target", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readScreenshotTargetDto(*value, "target");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.target = std::move(*parsed);
            }
        }

        {
            auto value = requiredField(params, "path", 1, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "path");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.path = std::move(*parsed);
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<ThumbnailParams>) -> Result<ThumbnailParams>
    {
        ThumbnailParams out;

        {
            auto value = requiredField(params, "asset", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readAssetSelector(**value, "asset");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.asset = std::move(*parsed);
        }

        {
            auto value = optionalField(params, "size", 1, true);
            if (value && !value->is_null())
            {
                auto parsed = readI32(*value, "size");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.size = std::move(*parsed);
            }
        }
        return out;
    }

    auto parseDto(const Json& params, DtoTag<ThumbnailCacheParams>) -> Result<ThumbnailCacheParams>
    {
        ThumbnailCacheParams out;

        {
            auto value = requiredField(params, "action", 0, true);
            if (!value) { return Err(std::move(value.error())); }
            auto parsed = readString(**value, "action");
            if (!parsed) { return Err(std::move(parsed.error())); }
            out.action = std::move(*parsed);
        }
        return out;
    }

    auto dtoToJson(const PingResult& value) -> Json
    {
        Json out = Json::object();
        out["pong"] = value.pong;
        out["engine"] = value.engine;
        out["version"] = value.version;
        out["pid"] = value.pid;
        return out;
    }

    auto dtoToJson(const RenderStatsDto& value) -> Json
    {
        Json out = Json::object();
        out["drawCalls"] = value.drawCalls;
        out["batches"] = value.batches;
        out["instances"] = value.instances;
        out["frameMs"] = value.frameMs;
        out["fps"] = value.fps;
        out["gpuMs"] = value.gpuMs;
        out["cpuFrameMs"] = value.cpuFrameMs;
        out["gpuFrameMs"] = value.gpuFrameMs;
        out["cpuWaitMs"] = value.cpuWaitMs;
        out["triangles"] = value.triangles;
        out["descriptorBinds"] = value.descriptorBinds;
        out["commandBuffers"] = value.commandBuffers;
        out["queueSubmits"] = value.queueSubmits;
        out["pipelinesCreated"] = value.pipelinesCreated;
        out["vramUsageBytes"] = value.vramUsageBytes;
        out["vramBudgetBytes"] = value.vramBudgetBytes;
        out["softwareGpu"] = value.softwareGpu;
        out["profilerMode"] = dtoToJson(value.profilerMode);
        out["clustered"] = value.clustered;
        out["depthPrepass"] = value.depthPrepass;
        out["shadows"] = value.shadows;
        out["ibl"] = value.ibl;
        out["ssao"] = value.ssao;
        out["contactShadows"] = value.contactShadows;
        out["ssgi"] = value.ssgi;
        out["ddgi"] = value.ddgi;
        out["rtSupported"] = value.rtSupported;
        out["rtShadows"] = value.rtShadows;
        out["restir"] = value.restir;
        out["blasCount"] = value.blasCount;
        out["pipelines"] = value.pipelines;
        out["bindlessTextures"] = value.bindlessTextures;
        out["bindlessFree"] = value.bindlessFree;
        out["hdr"] = value.hdr;
        out["exposureEv"] = value.exposureEv;
        out["aa"] = dtoToJson(value.aa);
        out["viewMode"] = dtoToJson(value.viewMode);
        return out;
    }

    auto dtoToJson(const ProfilerModeResult& value) -> Json
    {
        Json out = Json::object();
        out["mode"] = dtoToJson(value.mode);
        out["timestampsSupported"] = value.timestampsSupported;
        out["pipelineStatsSupported"] = value.pipelineStatsSupported;
        out["softwareGpu"] = value.softwareGpu;
        return out;
    }

    auto dtoToJson(const RenderPassTimingsDto& value) -> Json
    {
        Json out = Json::object();
        out["passes"] = dtoVectorToJson(value.passes);
        out["gpuTotalMs"] = value.gpuTotalMs;
        out["softwareGpu"] = value.softwareGpu;
        out["profilerMode"] = dtoToJson(value.profilerMode);
        return out;
    }

    auto dtoToJson(const RenderPassTimingDto& value) -> Json
    {
        Json out = Json::object();
        out["name"] = value.name;
        out["gpuMs"] = value.gpuMs;
        return out;
    }

    auto dtoToJson(const CaptureStartResult& value) -> Json
    {
        Json out = Json::object();
        out["captureId"] = value.captureId;
        out["ack"] = value.ack;
        return out;
    }

    auto dtoToJson(const CaptureStopResult& value) -> Json
    {
        Json out = Json::object();
        out["ready"] = value.ready;
        out["mode"] = dtoToJson(value.mode);
        out["frameCount"] = value.frameCount;
        out["inlined"] = value.inlined;
        out["capture"] = dtoToJson(value.capture);
        out["chromeTrace"] = value.chromeTrace;
        out["path"] = value.path;
        out["pending"] = value.pending;
        return out;
    }

    auto dtoToJson(const ProfileCaptureDto& value) -> Json
    {
        Json out = Json::object();
        out["spans"] = dtoVectorToJson(value.spans);
        out["metadata"] = dtoToJson(value.metadata);
        return out;
    }

    auto dtoToJson(const ProfileSpanDto& value) -> Json
    {
        Json out = Json::object();
        out["name"] = value.name;
        out["lane"] = dtoToJson(value.lane);
        out["startNs"] = value.startNs;
        out["endNs"] = value.endNs;
        out["parentIndex"] = value.parentIndex;
        out["depth"] = value.depth;
        if (value.pipelineStats) { out["pipelineStats"] = dtoToJson(*value.pipelineStats); }
        return out;
    }

    auto dtoToJson(const PipelineStatsDto& value) -> Json
    {
        Json out = Json::object();
        out["inputVertices"] = value.inputVertices;
        out["vertexInvocations"] = value.vertexInvocations;
        out["clippingInvocations"] = value.clippingInvocations;
        out["clippingPrimitives"] = value.clippingPrimitives;
        out["fragmentInvocations"] = value.fragmentInvocations;
        out["computeInvocations"] = value.computeInvocations;
        out["pixels"] = value.pixels;
        return out;
    }

    auto dtoToJson(const ProfileCaptureMetadataDto& value) -> Json
    {
        Json out = Json::object();
        out["softwareGpu"] = value.softwareGpu;
        out["correlated"] = value.correlated;
        out["deviceName"] = value.deviceName;
        out["timestampPeriod"] = value.timestampPeriod;
        out["targetFps"] = value.targetFps;
        out["mode"] = dtoToJson(value.mode);
        out["filter"] = value.filter;
        out["frameCount"] = value.frameCount;
        return out;
    }

    auto dtoToJson(const CaptureStatusResult& value) -> Json
    {
        Json out = Json::object();
        out["state"] = dtoToJson(value.state);
        out["capturedFrames"] = value.capturedFrames;
        out["targetFrames"] = value.targetFrames;
        out["mode"] = dtoToJson(value.mode);
        out["pipelineStatsSupported"] = value.pipelineStatsSupported;
        return out;
    }

    auto dtoToJson(const FrameHistoryDto& value) -> Json
    {
        Json out = Json::object();
        out["p50Ms"] = value.p50Ms;
        out["p95Ms"] = value.p95Ms;
        out["p99Ms"] = value.p99Ms;
        out["p999Ms"] = value.p999Ms;
        out["maxMs"] = value.maxMs;
        out["meanMs"] = value.meanMs;
        out["stddevMs"] = value.stddevMs;
        out["stutterCount"] = value.stutterCount;
        out["sampleCount"] = value.sampleCount;
        out["budgetMs"] = value.budgetMs;
        out["samples"] = dtoVectorToJson(value.samples);
        return out;
    }

    auto dtoToJson(const FrameSampleDto& value) -> Json
    {
        Json out = Json::object();
        out["frameIndex"] = value.frameIndex;
        out["cpuMs"] = value.cpuMs;
        out["gpuMs"] = value.gpuMs;
        out["cpuWaitMs"] = value.cpuWaitMs;
        return out;
    }

    auto dtoToJson(const PerfConfigDto& value) -> Json
    {
        Json out = Json::object();
        out["targetFps"] = value.targetFps;
        out["budgetMs"] = value.budgetMs;
        out["greenBudgetFrac"] = value.greenBudgetFrac;
        out["greenMedianMul"] = value.greenMedianMul;
        out["amberMedianMul"] = value.amberMedianMul;
        out["frozenMs"] = value.frozenMs;
        out["vramWarnFrac"] = value.vramWarnFrac;
        out["vramCritFrac"] = value.vramCritFrac;
        return out;
    }

    auto dtoToJson(const DrainAlarmsResult& value) -> Json
    {
        Json out = Json::object();
        out["events"] = dtoVectorToJson(value.events);
        out["highWaterSeq"] = value.highWaterSeq;
        out["oldestSeq"] = value.oldestSeq;
        out["overflowed"] = value.overflowed;
        return out;
    }

    auto dtoToJson(const AlarmEventDto& value) -> Json
    {
        Json out = Json::object();
        out["seq"] = value.seq;
        out["fingerprint"] = value.fingerprint;
        out["metric"] = value.metric;
        out["pass"] = value.pass;
        out["severity"] = dtoToJson(value.severity);
        out["state"] = dtoToJson(value.state);
        out["value"] = value.value;
        out["threshold"] = value.threshold;
        out["sinceFrame"] = value.sinceFrame;
        out["count"] = value.count;
        out["durationMs"] = value.durationMs;
        return out;
    }

    auto dtoToJson(const ActiveAlarmsDto& value) -> Json
    {
        Json out = Json::object();
        out["alarms"] = dtoVectorToJson(value.alarms);
        return out;
    }

    auto dtoToJson(const ActiveAlarmDto& value) -> Json
    {
        Json out = Json::object();
        out["fingerprint"] = value.fingerprint;
        out["metric"] = value.metric;
        out["pass"] = value.pass;
        out["severity"] = dtoToJson(value.severity);
        out["value"] = value.value;
        out["threshold"] = value.threshold;
        out["sinceFrame"] = value.sinceFrame;
        out["count"] = value.count;
        return out;
    }

    auto dtoToJson(const SetAaResult& value) -> Json
    {
        Json out = Json::object();
        out["aa"] = dtoToJson(value.aa);
        return out;
    }

    auto dtoToJson(const SetViewModeResult& value) -> Json
    {
        Json out = Json::object();
        out["viewMode"] = dtoToJson(value.viewMode);
        return out;
    }

    auto dtoToJson(const SetClusteredResult& value) -> Json
    {
        Json out = Json::object();
        out["clustered"] = value.clustered;
        return out;
    }

    auto dtoToJson(const SetIblResult& value) -> Json
    {
        Json out = Json::object();
        out["ibl"] = value.ibl;
        return out;
    }

    auto dtoToJson(const SetSsaoResult& value) -> Json
    {
        Json out = Json::object();
        out["ssao"] = value.ssao;
        return out;
    }

    auto dtoToJson(const SetContactShadowsResult& value) -> Json
    {
        Json out = Json::object();
        out["contactShadows"] = value.contactShadows;
        return out;
    }

    auto dtoToJson(const SetSsgiResult& value) -> Json
    {
        Json out = Json::object();
        out["ssgi"] = value.ssgi;
        return out;
    }

    auto dtoToJson(const SetRtShadowsResult& value) -> Json
    {
        Json out = Json::object();
        out["rtShadows"] = value.rtShadows;
        return out;
    }

    auto dtoToJson(const SetRestirResult& value) -> Json
    {
        Json out = Json::object();
        out["restir"] = value.restir;
        return out;
    }

    auto dtoToJson(const SetGiResult& value) -> Json
    {
        Json out = Json::object();
        out["ddgi"] = value.ddgi;
        return out;
    }

    auto dtoToJson(const SetShadowsResult& value) -> Json
    {
        Json out = Json::object();
        out["shadows"] = value.shadows;
        return out;
    }

    auto dtoToJson(const SetSkinningResult& value) -> Json
    {
        Json out = Json::object();
        out["skinning"] = value.skinning;
        return out;
    }

    auto dtoToJson(const SetDepthPrepassResult& value) -> Json
    {
        Json out = Json::object();
        out["depthPrepass"] = value.depthPrepass;
        return out;
    }

    auto dtoToJson(const ViewportNativeInfoResult& value) -> Json
    {
        Json out = Json::object();
        out["platform"] = value.platform;
        out["transport"] = value.transport;
        out["status"] = value.status;
        out["controlSocket"] = value.controlSocket;
        out["width"] = value.width;
        out["height"] = value.height;
        out["message"] = value.message;
        return out;
    }

    auto dtoToJson(const SetViewportSizeResult& value) -> Json
    {
        Json out = Json::object();
        out["width"] = value.width;
        out["height"] = value.height;
        return out;
    }

    auto dtoToJson(const EntityList& value) -> Json
    {
        Json out = Json::object();
        out["entities"] = dtoVectorToJson(value.entities);
        return out;
    }

    auto dtoToJson(const EntityListEntry& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["name"] = value.name;
        if (value.parentId) { out["parentId"] = dtoToJson(*value.parentId); }
        if (value.bone) { out["bone"] = *value.bone; }
        return out;
    }

    auto dtoToJson(const ComponentList& value) -> Json
    {
        Json out = Json::object();
        out["components"] = dtoVectorToJson(value.components);
        return out;
    }

    auto dtoToJson(const EntityRef& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["name"] = value.name;
        return out;
    }

    auto dtoToJson(const DestroyEntityResult& value) -> Json
    {
        Json out = Json::object();
        out["destroyed"] = dtoToJson(value.destroyed);
        return out;
    }

    auto dtoToJson(const AddComponentResult& value) -> Json
    {
        Json out = Json::object();
        out["added"] = value.added;
        return out;
    }

    auto dtoToJson(const RemoveComponentResult& value) -> Json
    {
        Json out = Json::object();
        out["removed"] = value.removed;
        return out;
    }

    auto dtoToJson(const SetComponentResult& value) -> Json
    {
        Json out = Json::object();
        out["set"] = value.set;
        return out;
    }

    auto dtoToJson(const PickResult& value) -> Json
    {
        Json out = Json::object();
        out["hit"] = value.hit;
        if (value.id) { out["id"] = dtoToJson(*value.id); }
        if (value.name) { out["name"] = *value.name; }
        if (value.kind) { out["kind"] = dtoToJson(*value.kind); }
        return out;
    }

    auto dtoToJson(const InspectResult& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["name"] = value.name;
        out["components"] = value.components;
        return out;
    }

    auto dtoToJson(const WorldTransformResult& value) -> Json
    {
        Json out = Json::object();
        out["translation"] = dtoToJson(value.translation);
        out["scale"] = dtoToJson(value.scale);
        return out;
    }

    auto dtoToJson(const Vec3& value) -> Json
    {
        Json out = Json::object();
        out["x"] = value.x;
        out["y"] = value.y;
        out["z"] = value.z;
        return out;
    }

    auto dtoToJson(const EnvironmentDto& value) -> Json
    {
        return value.value;
    }

    auto dtoToJson(const SelectionResult& value) -> Json
    {
        Json out = Json::object();
        out["selectionVersion"] = value.selectionVersion;
        out["sceneVersion"] = value.sceneVersion;
        if (value.entity) { out["entity"] = dtoToJson(*value.entity); }
        else { out["entity"] = nullptr; }
        out["playState"] = value.playState;
        out["playVersion"] = value.playVersion;
        out["animationVersion"] = value.animationVersion;
        return out;
    }

    auto dtoToJson(const DeselectResult& value) -> Json
    {
        Json out = Json::object();
        out["selectionVersion"] = value.selectionVersion;
        return out;
    }

    auto dtoToJson(const PlayStateResult& value) -> Json
    {
        Json out = Json::object();
        out["state"] = value.state;
        out["playVersion"] = value.playVersion;
        out["sceneVersion"] = value.sceneVersion;
        out["hasPrimaryCamera"] = value.hasPrimaryCamera;
        out["animationVersion"] = value.animationVersion;
        out["previewAsset"] = dtoToJson(value.previewAsset);
        return out;
    }

    auto dtoToJson(const AnimationStateResult& value) -> Json
    {
        Json out = Json::object();
        out["clip"] = dtoToJson(value.clip);
        out["clipName"] = value.clipName;
        out["duration"] = value.duration;
        out["time"] = value.time;
        out["playing"] = value.playing;
        out["wrap"] = value.wrap;
        out["speed"] = value.speed;
        out["animationVersion"] = value.animationVersion;
        return out;
    }

    auto dtoToJson(const ListClipsResult& value) -> Json
    {
        Json out = Json::object();
        out["clips"] = dtoVectorToJson(value.clips);
        return out;
    }

    auto dtoToJson(const AnimationClipDto& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["name"] = value.name;
        out["duration"] = value.duration;
        out["tracks"] = value.tracks;
        return out;
    }

    auto dtoToJson(const SkeletonOverlayResult& value) -> Json
    {
        Json out = Json::object();
        out["show"] = value.show;
        out["axes"] = value.axes;
        out["jointSize"] = value.jointSize;
        out["highlightJoint"] = value.highlightJoint;
        return out;
    }

    auto dtoToJson(const DebugOverlaysResult& value) -> Json
    {
        Json out = Json::object();
        out["bounds"] = value.bounds;
        out["sceneAabb"] = value.sceneAabb;
        out["lightVolumes"] = value.lightVolumes;
        out["grid"] = value.grid;
        out["colliders"] = value.colliders;
        return out;
    }

    auto dtoToJson(const PickSkeletonJointResult& value) -> Json
    {
        Json out = Json::object();
        out["found"] = value.found;
        out["nodeIndex"] = value.nodeIndex;
        return out;
    }

    auto dtoToJson(const AssetPreviewOptionsResult& value) -> Json
    {
        Json out = Json::object();
        out["floor"] = value.floor;
        return out;
    }

    auto dtoToJson(const FootIkResult& value) -> Json
    {
        Json out = Json::object();
        out["enabled"] = value.enabled;
        out["groundHeight"] = value.groundHeight;
        out["chains"] = value.chains;
        return out;
    }

    auto dtoToJson(const ScriptStatusResult& value) -> Json
    {
        Json out = Json::object();
        out["state"] = value.state;
        out["instances"] = value.instances;
        out["errorHighWater"] = value.errorHighWater;
        return out;
    }

    auto dtoToJson(const PhysicsStateResult& value) -> Json
    {
        Json out = Json::object();
        out["active"] = value.active;
        out["bodyCount"] = value.bodyCount;
        out["dynamicCount"] = value.dynamicCount;
        return out;
    }

    auto dtoToJson(const PhysicsBodiesResult& value) -> Json
    {
        Json out = Json::object();
        out["bodies"] = dtoVectorToJson(value.bodies);
        return out;
    }

    auto dtoToJson(const PhysicsBodyDto& value) -> Json
    {
        Json out = Json::object();
        out["entity"] = dtoToJson(value.entity);
        out["motion"] = value.motion;
        out["active"] = value.active;
        out["position"] = dtoToJson(value.position);
        return out;
    }

    auto dtoToJson(const FitColliderResult& value) -> Json
    {
        Json out = Json::object();
        out["entity"] = dtoToJson(value.entity);
        out["shape"] = value.shape;
        out["halfExtents"] = dtoToJson(value.halfExtents);
        out["offset"] = dtoToJson(value.offset);
        return out;
    }

    auto dtoToJson(const ApplyImpulseResult& value) -> Json
    {
        Json out = Json::object();
        out["velocity"] = dtoToJson(value.velocity);
        return out;
    }

    auto dtoToJson(const DrainContactsResult& value) -> Json
    {
        Json out = Json::object();
        out["events"] = dtoVectorToJson(value.events);
        out["highWaterSeq"] = value.highWaterSeq;
        out["oldestSeq"] = value.oldestSeq;
        out["overflowed"] = value.overflowed;
        return out;
    }

    auto dtoToJson(const ContactEventDto& value) -> Json
    {
        Json out = Json::object();
        out["seq"] = value.seq;
        out["kind"] = value.kind;
        out["entityA"] = dtoToJson(value.entityA);
        out["entityB"] = dtoToJson(value.entityB);
        out["sensor"] = value.sensor;
        out["point"] = dtoToJson(value.point);
        out["normal"] = dtoToJson(value.normal);
        out["tick"] = value.tick;
        return out;
    }

    auto dtoToJson(const KinematicBonesResult& value) -> Json
    {
        Json out = Json::object();
        out["entity"] = dtoToJson(value.entity);
        out["enabled"] = value.enabled;
        out["boneCount"] = value.boneCount;
        return out;
    }

    auto dtoToJson(const MoveCharacterResult& value) -> Json
    {
        Json out = Json::object();
        out["position"] = dtoToJson(value.position);
        out["onGround"] = value.onGround;
        return out;
    }

    auto dtoToJson(const RaycastResult& value) -> Json
    {
        Json out = Json::object();
        out["hit"] = value.hit;
        out["entity"] = dtoToJson(value.entity);
        out["point"] = dtoToJson(value.point);
        out["normal"] = dtoToJson(value.normal);
        out["distance"] = value.distance;
        return out;
    }

    auto dtoToJson(const RagdollResult& value) -> Json
    {
        Json out = Json::object();
        out["present"] = value.present;
        out["active"] = value.active;
        out["bodyWeight"] = value.bodyWeight;
        out["bones"] = value.bones;
        return out;
    }

    auto dtoToJson(const DrainScriptErrorsResult& value) -> Json
    {
        Json out = Json::object();
        out["events"] = dtoVectorToJson(value.events);
        out["highWaterSeq"] = value.highWaterSeq;
        out["oldestSeq"] = value.oldestSeq;
        out["overflowed"] = value.overflowed;
        return out;
    }

    auto dtoToJson(const ScriptErrorDto& value) -> Json
    {
        Json out = Json::object();
        out["seq"] = value.seq;
        out["entity"] = dtoToJson(value.entity);
        out["script"] = value.script;
        out["message"] = value.message;
        out["tick"] = value.tick;
        return out;
    }

    auto dtoToJson(const DrainScriptLogsResult& value) -> Json
    {
        Json out = Json::object();
        out["events"] = dtoVectorToJson(value.events);
        out["highWaterSeq"] = value.highWaterSeq;
        out["oldestSeq"] = value.oldestSeq;
        out["overflowed"] = value.overflowed;
        return out;
    }

    auto dtoToJson(const ScriptLogDto& value) -> Json
    {
        Json out = Json::object();
        out["seq"] = value.seq;
        out["entity"] = dtoToJson(value.entity);
        out["message"] = value.message;
        out["epochMs"] = value.epochMs;
        out["tick"] = value.tick;
        return out;
    }

    auto dtoToJson(const GetScriptSchemaResult& value) -> Json
    {
        Json out = Json::object();
        out["fields"] = dtoVectorToJson(value.fields);
        return out;
    }

    auto dtoToJson(const ScriptFieldDto& value) -> Json
    {
        Json out = Json::object();
        out["name"] = value.name;
        out["type"] = value.type;
        out["defaultValue"] = value.defaultValue;
        return out;
    }

    auto dtoToJson(const SetScriptOverrideResult& value) -> Json
    {
        Json out = Json::object();
        out["scriptPath"] = value.scriptPath;
        out["overrides"] = value.overrides;
        return out;
    }

    auto dtoToJson(const SetComponentFieldResult& value) -> Json
    {
        Json out = Json::object();
        out["set"] = value.set;
        out["field"] = value.field;
        return out;
    }

    auto dtoToJson(const EditorCamera& value) -> Json
    {
        Json out = Json::object();
        out["position"] = dtoToJson(value.position);
        out["yaw"] = value.yaw;
        out["pitch"] = value.pitch;
        out["fov"] = value.fov;
        out["near"] = value.near;
        out["far"] = value.far;
        out["moveSpeed"] = value.moveSpeed;
        out["lookSpeed"] = value.lookSpeed;
        return out;
    }

    auto dtoToJson(const GizmoState& value) -> Json
    {
        Json out = Json::object();
        out["op"] = dtoToJson(value.op);
        out["space"] = dtoToJson(value.space);
        out["preserveChildren"] = value.preserveChildren;
        return out;
    }

    auto dtoToJson(const GizmoPointerResult& value) -> Json
    {
        Json out = Json::object();
        out["hovered"] = value.hovered;
        out["dragging"] = value.dragging;
        return out;
    }

    auto dtoToJson(const FlyInputResult& value) -> Json
    {
        Json out = Json::object();
        out["active"] = value.active;
        return out;
    }

    auto dtoToJson(const ScriptInputResult& value) -> Json
    {
        Json out = Json::object();
        out["keys"] = dtoVectorToJson(value.keys);
        return out;
    }

    auto dtoToJson(const SetProbesResult& value) -> Json
    {
        Json out = Json::object();
        out["probes"] = value.probes;
        return out;
    }

    auto dtoToJson(const RecaptureProbesResult& value) -> Json
    {
        Json out = Json::object();
        out["marked"] = value.marked;
        return out;
    }

    auto dtoToJson(const ListProbesResult& value) -> Json
    {
        Json out = Json::object();
        out["enabled"] = value.enabled;
        out["count"] = value.count;
        out["probes"] = dtoVectorToJson(value.probes);
        return out;
    }

    auto dtoToJson(const ProbeRef& value) -> Json
    {
        Json out = Json::object();
        out["slot"] = value.slot;
        out["entity"] = dtoToJson(value.entity);
        out["origin"] = dtoToJson(value.origin);
        out["influenceRadius"] = value.influenceRadius;
        out["intensity"] = value.intensity;
        out["boxProjection"] = value.boxProjection;
        out["valid"] = value.valid;
        out["dirty"] = value.dirty;
        return out;
    }

    auto dtoToJson(const SetExposureResult& value) -> Json
    {
        Json out = Json::object();
        out["exposureEv"] = value.exposureEv;
        return out;
    }

    auto dtoToJson(const ProjectInfoDto& value) -> Json
    {
        Json out = Json::object();
        out["loaded"] = value.loaded;
        out["root"] = value.root;
        out["path"] = value.path;
        out["name"] = value.name;
        out["displayName"] = value.displayName;
        return out;
    }

    auto dtoToJson(const CreateScriptResult& value) -> Json
    {
        Json out = Json::object();
        out["path"] = value.path;
        return out;
    }

    auto dtoToJson(const ImportModelResult& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["name"] = value.name;
        out["type"] = value.type;
        return out;
    }

    auto dtoToJson(const ImportTextureResult& value) -> Json
    {
        Json out = Json::object();
        out["texture"] = dtoToJson(value.texture);
        return out;
    }

    auto dtoToJson(const AssetList& value) -> Json
    {
        Json out = Json::object();
        out["assets"] = dtoVectorToJson(value.assets);
        out["folders"] = dtoVectorToJson(value.folders);
        return out;
    }

    auto dtoToJson(const AssetEntryDto& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["name"] = value.name;
        out["type"] = dtoToJson(value.type);
        out["path"] = value.path;
        if (value.folder) { out["folder"] = *value.folder; }
        if (value.container) { out["container"] = dtoToJson(*value.container); }
        if (value.duration) { out["duration"] = *value.duration; }
        if (value.rigged) { out["rigged"] = *value.rigged; }
        return out;
    }

    auto dtoToJson(const ScanAssetsResult& value) -> Json
    {
        Json out = Json::object();
        out["added"] = value.added;
        out["removed"] = value.removed;
        return out;
    }

    auto dtoToJson(const AssetRef& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["name"] = value.name;
        if (value.folder) { out["folder"] = *value.folder; }
        return out;
    }

    auto dtoToJson(const ReimportModelResult& value) -> Json
    {
        Json out = Json::object();
        out["updated"] = value.updated;
        out["added"] = value.added;
        out["removedFromSource"] = value.removedFromSource;
        out["skipped"] = value.skipped;
        return out;
    }

    auto dtoToJson(const ModelInfoResult& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["name"] = value.name;
        out["sourcePath"] = value.sourcePath;
        out["sourceHash"] = value.sourceHash;
        out["materialCount"] = value.materialCount;
        out["hasSkin"] = value.hasSkin;
        out["nodeCount"] = value.nodeCount;
        out["totalBytes"] = value.totalBytes;
        out["subAssets"] = dtoVectorToJson(value.subAssets);
        return out;
    }

    auto dtoToJson(const ModelSubAssetDto& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["name"] = value.name;
        out["type"] = value.type;
        out["bytes"] = value.bytes;
        return out;
    }

    auto dtoToJson(const AssetReferencesResult& value) -> Json
    {
        Json out = Json::object();
        out["referencedBy"] = dtoVectorToJson(value.referencedBy);
        out["references"] = dtoVectorToJson(value.references);
        out["footprint"] = value.footprint;
        return out;
    }

    auto dtoToJson(const AssetModelResult& value) -> Json
    {
        Json out = Json::object();
        out["mesh"] = dtoToJson(value.mesh);
        out["name"] = value.name;
        out["capabilities"] = dtoToJson(value.capabilities);
        out["bones"] = dtoVectorToJson(value.bones);
        out["clips"] = dtoVectorToJson(value.clips);
        return out;
    }

    auto dtoToJson(const AssetCapabilitiesDto& value) -> Json
    {
        Json out = Json::object();
        out["meshCount"] = value.meshCount;
        out["materialCount"] = value.materialCount;
        out["nodeCount"] = value.nodeCount;
        out["hasRig"] = value.hasRig;
        out["boneCount"] = value.boneCount;
        out["clipCount"] = value.clipCount;
        return out;
    }

    auto dtoToJson(const BoneDto& value) -> Json
    {
        Json out = Json::object();
        out["index"] = value.index;
        out["name"] = value.name;
        out["parent"] = value.parent;
        out["joint"] = value.joint;
        return out;
    }

    auto dtoToJson(const AssetPreviewResult& value) -> Json
    {
        Json out = Json::object();
        out["rootEntity"] = dtoToJson(value.rootEntity);
        out["bones"] = dtoVectorToJson(value.bones);
        out["target"] = dtoToJson(value.target);
        out["distance"] = value.distance;
        return out;
    }

    auto dtoToJson(const BoneEntityDto& value) -> Json
    {
        Json out = Json::object();
        out["index"] = value.index;
        out["entity"] = dtoToJson(value.entity);
        return out;
    }

    auto dtoToJson(const SetActiveViewResult& value) -> Json
    {
        Json out = Json::object();
        out["view"] = value.view;
        return out;
    }

    auto dtoToJson(const CleanReport& value) -> Json
    {
        Json out = Json::object();
        out["candidates"] = dtoVectorToJson(value.candidates);
        out["reclaimableBytes"] = value.reclaimableBytes;
        return out;
    }

    auto dtoToJson(const CleanCandidateDto& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["path"] = value.path;
        out["category"] = value.category;
        out["bytes"] = value.bytes;
        out["reason"] = value.reason;
        return out;
    }

    auto dtoToJson(const DeleteUnusedResult& value) -> Json
    {
        Json out = Json::object();
        out["deleted"] = value.deleted;
        out["reclaimedBytes"] = value.reclaimedBytes;
        return out;
    }

    auto dtoToJson(const AssetUsagesResult& value) -> Json
    {
        Json out = Json::object();
        out["usages"] = dtoVectorToJson(value.usages);
        return out;
    }

    auto dtoToJson(const AssetUsageDto& value) -> Json
    {
        Json out = Json::object();
        if (value.entity) { out["entity"] = dtoToJson(*value.entity); }
        if (value.entityName) { out["entityName"] = *value.entityName; }
        out["slot"] = value.slot;
        return out;
    }

    auto dtoToJson(const AssetMetadataDto& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["name"] = value.name;
        out["type"] = dtoToJson(value.type);
        out["path"] = value.path;
        if (value.folder) { out["folder"] = *value.folder; }
        out["sizeBytes"] = value.sizeBytes;
        if (value.vertexCount) { out["vertexCount"] = *value.vertexCount; }
        if (value.triangleCount) { out["triangleCount"] = *value.triangleCount; }
        out["createdAt"] = value.createdAt;
        return out;
    }

    auto dtoToJson(const DeleteAssetResult& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["name"] = value.name;
        out["cleared"] = dtoVectorToJson(value.cleared);
        out["fileDeleted"] = value.fileDeleted;
        return out;
    }

    auto dtoToJson(const AssignAssetResult& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["name"] = value.name;
        out["slot"] = dtoToJson(value.slot);
        return out;
    }

    auto dtoToJson(const MaterialCreateResult& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["name"] = value.name;
        return out;
    }

    auto dtoToJson(const MaterialAssignResult& value) -> Json
    {
        Json out = Json::object();
        out["material"] = dtoToJson(value.material);
        return out;
    }

    auto dtoToJson(const MaterialImportResultDto& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["roles"] = value.roles;
        return out;
    }

    auto dtoToJson(const MaterialListResult& value) -> Json
    {
        Json out = Json::object();
        out["materials"] = dtoVectorToJson(value.materials);
        return out;
    }

    auto dtoToJson(const MaterialRefDto& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["name"] = value.name;
        out["folder"] = value.folder;
        return out;
    }

    auto dtoToJson(const MaterialGetResult& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["blend"] = value.blend;
        out["unlit"] = value.unlit;
        out["baseColor"] = dtoToJson(value.baseColor);
        out["metallic"] = value.metallic;
        out["roughness"] = value.roughness;
        out["emissive"] = dtoToJson(value.emissive);
        out["emissiveStrength"] = value.emissiveStrength;
        out["albedoTexture"] = dtoToJson(value.albedoTexture);
        out["ormTexture"] = dtoToJson(value.ormTexture);
        out["normalTexture"] = dtoToJson(value.normalTexture);
        out["emissiveTexture"] = dtoToJson(value.emissiveTexture);
        out["heightTexture"] = dtoToJson(value.heightTexture);
        out["graph"] = value.graph;
        return out;
    }

    auto dtoToJson(const Vec4& value) -> Json
    {
        Json out = Json::object();
        out["x"] = value.x;
        out["y"] = value.y;
        out["z"] = value.z;
        out["w"] = value.w;
        return out;
    }

    auto dtoToJson(const MaterialUpdateResult& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        return out;
    }

    auto dtoToJson(const PreviewRenderResult& value) -> Json
    {
        Json out = Json::object();
        out["png"] = value.png;
        return out;
    }

    auto dtoToJson(const MaterialSetGraphResult& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["foldable"] = value.foldable;
        return out;
    }

    auto dtoToJson(const MaterialSetOverrideResult& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        return out;
    }

    auto dtoToJson(const MaterialCompileResult& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["ok"] = value.ok;
        return out;
    }

    auto dtoToJson(const MaterialCookResult& value) -> Json
    {
        Json out = Json::object();
        out["compiled"] = value.compiled;
        out["failed"] = value.failed;
        return out;
    }

    auto dtoToJson(const PathResult& value) -> Json
    {
        Json out = Json::object();
        out["path"] = value.path;
        return out;
    }

    auto dtoToJson(const ScreenshotResult& value) -> Json
    {
        Json out = Json::object();
        out["target"] = dtoToJson(value.target);
        out["path"] = value.path;
        out["pending"] = value.pending;
        return out;
    }

    auto dtoToJson(const ThumbnailResult& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["format"] = value.format;
        out["width"] = value.width;
        out["height"] = value.height;
        out["base64"] = value.base64;
        out["pending"] = value.pending;
        return out;
    }

    auto dtoToJson(const ThumbnailCacheResult& value) -> Json
    {
        Json out = Json::object();
        out["entries"] = value.entries;
        out["bytes"] = value.bytes;
        return out;
    }

    auto dtoToJson(const QuitResult& value) -> Json
    {
        Json out = Json::object();
        out["quitting"] = value.quitting;
        return out;
    }
}
