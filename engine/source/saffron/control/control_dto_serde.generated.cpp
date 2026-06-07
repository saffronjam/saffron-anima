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

        auto readAssetSlotDto(const Json& value, std::string_view key) -> Result<AssetSlotDto>
        {
            auto text = readString(value, key);
            if (!text) { return Err(std::move(text.error())); }
            if (text == "mesh") { return AssetSlotDto::Mesh; }
            if (text == "albedo") { return AssetSlotDto::Albedo; }
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto AssetSlotDtoName(AssetSlotDto value) -> const char*
        {
            switch (value)
            {
            case AssetSlotDto::Mesh: return "mesh";
            case AssetSlotDto::Albedo: return "albedo";
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
            return Err(std::format("key '{}' has unknown value '{}'", key, *text));
        }

        auto AssetTypeDtoName(AssetTypeDto value) -> const char*
        {
            switch (value)
            {
            case AssetTypeDto::Mesh: return "mesh";
            case AssetTypeDto::Texture: return "texture";
            case AssetTypeDto::Other: return "other";
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
            auto value = optionalField(params, "width", 0, true);
            if (value && !value->is_null())
            {
                auto parsed = readI32(*value, "width");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.width = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "height", 1, true);
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
            auto value = optionalField(params, "metallic", 3, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "metallic");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.metallic = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "roughness", 4, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "roughness");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.roughness = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "emissive", 5, true);
            if (value && !value->is_null())
            {
                auto parsed = parseDto(*value, DtoTag<Vec3>{});
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.emissive = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "emissiveStrength", 6, true);
            if (value && !value->is_null())
            {
                auto parsed = readF32(*value, "emissiveStrength");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.emissiveStrength = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "unlit", 7, true);
            if (value && !value->is_null())
            {
                auto parsed = readBool(*value, "unlit");
                if (!parsed) { return Err(std::move(parsed.error())); }
                out.unlit = std::move(*parsed);
            }
        }

        {
            auto value = optionalField(params, "smooth", 8, true);
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
        out["hdr"] = value.hdr;
        out["exposureEv"] = value.exposureEv;
        out["aa"] = dtoToJson(value.aa);
        return out;
    }

    auto dtoToJson(const SetAaResult& value) -> Json
    {
        Json out = Json::object();
        out["aa"] = dtoToJson(value.aa);
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

    auto dtoToJson(const Vec3& value) -> Json
    {
        Json out = Json::object();
        out["x"] = value.x;
        out["y"] = value.y;
        out["z"] = value.z;
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

    auto dtoToJson(const ImportModelResult& value) -> Json
    {
        Json out = Json::object();
        out["id"] = dtoToJson(value.id);
        out["name"] = value.name;
        out["mesh"] = dtoToJson(value.mesh);
        out["albedoTexture"] = dtoToJson(value.albedoTexture);
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
        return out;
    }

    auto dtoToJson(const QuitResult& value) -> Json
    {
        Json out = Json::object();
        out["quitting"] = value.quitting;
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
}
