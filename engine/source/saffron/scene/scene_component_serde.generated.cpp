// GENERATED - do not edit.
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
                if (end != text.c_str() && *end == '\0')
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

    auto animationPlayerComponentToJson(const AnimationPlayerComponent& c) -> nlohmann::json
    {
        const char* wrap = c.wrap == AnimationPlayerComponent::Wrap::Once       ? "once"
                           : c.wrap == AnimationPlayerComponent::Wrap::PingPong ? "pingpong"
                                                                                : "loop";
        return nlohmann::json{ { "clip", uuidToJson(c.clip.value) }, { "time", c.time }, { "speed", c.speed },
                               { "wrap", wrap }, { "playing", c.playing } };
    }

    auto animationPlayerComponentFromJson(AnimationPlayerComponent& c, const nlohmann::json& j) -> Result<void>
    {
        c.clip = Uuid{ jsonU64Or(j, "clip", 0) };
        c.time = jsonF32Or(j, "time", 0.0f);
        c.speed = jsonF32Or(j, "speed", 1.0f);
        const std::string wrap = jsonStringOr(j, "wrap", std::string{ "loop" });
        c.wrap = wrap == "once"       ? AnimationPlayerComponent::Wrap::Once
                 : wrap == "pingpong" ? AnimationPlayerComponent::Wrap::PingPong
                                      : AnimationPlayerComponent::Wrap::Loop;
        c.playing = jsonBoolOr(j, "playing", false);
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
