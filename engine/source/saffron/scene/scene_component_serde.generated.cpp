// GENERATED - do not edit.
// Produced by tools/gen-control-dto/gen.ts from the scene component DTO catalog.

module;

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

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
                               { "far", c.farPlane }, { "primary", c.primary } };
    }

    auto cameraComponentFromJson(CameraComponent& c, const nlohmann::json& j) -> Result<void>
    {
        c.fov = jsonF32Or(j, "fov", 45.0f);
        c.nearPlane = jsonF32Or(j, "near", 0.1f);
        c.farPlane = jsonF32Or(j, "far", 100.0f);
        c.primary = jsonBoolOr(j, "primary", true);
        return {};
    }

    auto materialComponentToJson(const MaterialComponent& c) -> nlohmann::json
    {
        return nlohmann::json{ { "baseColor", vec4ToJson(c.baseColor) },
                               { "albedoTexture", uuidToJson(c.albedoTexture.value) },
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
        c.metallic = jsonF32Or(j, "metallic", 0.0f);
        c.roughness = jsonF32Or(j, "roughness", 1.0f);
        c.emissive = vec3FromJson(j.value("emissive", nlohmann::json::object()));
        c.emissiveStrength = jsonF32Or(j, "emissiveStrength", 1.0f);
        c.unlit = jsonBoolOr(j, "unlit", false);
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
