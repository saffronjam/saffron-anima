// GENERATED - do not edit.
// The scene-component (de)serialization below is hand-maintained in emitSceneSerde
// (tools/gen-control-dto/gen.ts), kept in step with the component structs in Saffron.Scene.

module;

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <format>
#include <string>

module Saffron.Scene;

import Saffron.Core;
import Saffron.Json;

namespace sa
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
        c.frustumMaxDistance = jsonF32Or(j, "frustumMaxDistance", 10.0f);
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
                               { "unlit", c.unlit },
                               { "normalTexture", uuidToJson(c.normalTexture.value) },
                               { "occlusionTexture", uuidToJson(c.occlusionTexture.value) },
                               { "emissiveTexture", uuidToJson(c.emissiveTexture.value) },
                               { "heightTexture", uuidToJson(c.heightTexture.value) },
                               { "normalStrength", c.normalStrength },
                               { "heightScale", c.heightScale },
                               { "alphaClip", c.alphaClip },
                               { "alphaCutoff", c.alphaCutoff } };
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
        c.normalTexture = Uuid{ jsonU64Or(j, "normalTexture", 0) };
        c.occlusionTexture = Uuid{ jsonU64Or(j, "occlusionTexture", 0) };
        c.emissiveTexture = Uuid{ jsonU64Or(j, "emissiveTexture", 0) };
        c.heightTexture = Uuid{ jsonU64Or(j, "heightTexture", 0) };
        c.normalStrength = jsonF32Or(j, "normalStrength", 1.0f);
        c.heightScale = jsonF32Or(j, "heightScale", 0.05f);
        c.alphaClip = jsonBoolOr(j, "alphaClip", false);
        c.alphaCutoff = jsonF32Or(j, "alphaCutoff", 0.5f);
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
                                            { "unlit", s.unlit },
                                            { "normalTexture", uuidToJson(s.normalTexture.value) },
                                            { "occlusionTexture", uuidToJson(s.occlusionTexture.value) },
                                            { "emissiveTexture", uuidToJson(s.emissiveTexture.value) },
                                            { "heightTexture", uuidToJson(s.heightTexture.value) },
                                            { "normalStrength", s.normalStrength },
                                            { "heightScale", s.heightScale },
                                            { "alphaClip", s.alphaClip },
                                            { "alphaCutoff", s.alphaCutoff } });
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
                s.normalTexture = Uuid{ jsonU64Or(sj, "normalTexture", 0) };
                s.occlusionTexture = Uuid{ jsonU64Or(sj, "occlusionTexture", 0) };
                s.emissiveTexture = Uuid{ jsonU64Or(sj, "emissiveTexture", 0) };
                s.heightTexture = Uuid{ jsonU64Or(sj, "heightTexture", 0) };
                s.normalStrength = jsonF32Or(sj, "normalStrength", 1.0f);
                s.heightScale = jsonF32Or(sj, "heightScale", 0.05f);
                s.alphaClip = jsonBoolOr(sj, "alphaClip", false);
                s.alphaCutoff = jsonF32Or(sj, "alphaCutoff", 0.5f);
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
        const char* transition =
            c.transitionMode == AnimationPlayerComponent::Transition::CrossFade ? "crossfade" : "inertialize";
        return nlohmann::json{ { "clip", uuidToJson(c.clip.value) }, { "time", c.time },
                               { "speed", c.speed },                 { "wrap", wrap },
                               { "playing", c.playing },             { "transitionMode", transition },
                               { "loopBlend", c.loopBlend } };
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
        const std::string transition = jsonStringOr(j, "transitionMode", std::string{ "inertialize" });
        c.transitionMode = transition == "crossfade" ? AnimationPlayerComponent::Transition::CrossFade
                                                      : AnimationPlayerComponent::Transition::Inertialize;
        c.loopBlend = jsonF32Or(j, "loopBlend", 0.0f);
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

    auto footIkComponentToJson(const FootIkComponent& c) -> nlohmann::json
    {
        nlohmann::json chains = nlohmann::json::array();
        for (const FootChain& chain : c.chains)
        {
            chains.push_back(nlohmann::json{ { "upper", chain.upper },
                                             { "mid", chain.mid },
                                             { "end", chain.end },
                                             { "poleVector", vec3ToJson(chain.poleVector) } });
        }
        return nlohmann::json{
            { "enabled", c.enabled }, { "groundHeight", c.groundHeight }, { "chains", std::move(chains) }
        };
    }

    auto footIkComponentFromJson(FootIkComponent& c, const nlohmann::json& j) -> Result<void>
    {
        c.enabled = jsonBoolOr(j, "enabled", false);
        c.groundHeight = jsonF32Or(j, "groundHeight", 0.0f);
        c.chains.clear();
        if (j.contains("chains") && j["chains"].is_array())
        {
            for (const nlohmann::json& entry : j["chains"])
            {
                FootChain chain;
                chain.upper = static_cast<i32>(entry.value("upper", -1));
                chain.mid = static_cast<i32>(entry.value("mid", -1));
                chain.end = static_cast<i32>(entry.value("end", -1));
                chain.poleVector = vec3FromJson(entry.value("poleVector", nlohmann::json::object()));
                c.chains.push_back(chain);
            }
        }
        return {};
    }

    auto bonePhysicsComponentToJson(const BonePhysicsComponent& c) -> nlohmann::json
    {
        auto jointName = [](BonePhysics::Joint joint) -> const char*
        {
            switch (joint)
            {
                case BonePhysics::Joint::Fixed: return "fixed";
                case BonePhysics::Joint::Hinge: return "hinge";
                case BonePhysics::Joint::SwingTwist: return "swingtwist";
                case BonePhysics::Joint::Free: return "free";
            }
            return "swingtwist";
        };
        nlohmann::json bones = nlohmann::json::array();
        for (const BonePhysics& b : c.bones)
        {
            bones.push_back(nlohmann::json{ { "shapeHalfExtents", vec3ToJson(b.shapeHalfExtents) },
                                            { "mass", b.mass },
                                            { "joint", jointName(b.joint) },
                                            { "swingTwistLimits", vec3ToJson(b.swingTwistLimits) },
                                            { "driveStiffness", b.driveStiffness },
                                            { "driveDamping", b.driveDamping },
                                            { "driveMaxForce", b.driveMaxForce } });
        }
        return nlohmann::json{ { "bones", std::move(bones) } };
    }

    auto bonePhysicsComponentFromJson(BonePhysicsComponent& c, const nlohmann::json& j) -> Result<void>
    {
        auto jointFromName = [](const std::string& name) -> BonePhysics::Joint
        {
            if (name == "fixed") { return BonePhysics::Joint::Fixed; }
            if (name == "hinge") { return BonePhysics::Joint::Hinge; }
            if (name == "free") { return BonePhysics::Joint::Free; }
            return BonePhysics::Joint::SwingTwist;
        };
        c.bones.clear();
        if (j.contains("bones") && j["bones"].is_array())
        {
            for (const nlohmann::json& entry : j["bones"])
            {
                BonePhysics b;
                b.shapeHalfExtents = vec3FromJson(entry.value("shapeHalfExtents", nlohmann::json::object()));
                b.mass = jsonF32Or(entry, "mass", 1.0f);
                b.joint = jointFromName(jsonStringOr(entry, "joint", std::string{ "swingtwist" }));
                b.swingTwistLimits = vec3FromJson(entry.value("swingTwistLimits", nlohmann::json::object()));
                b.driveStiffness = jsonF32Or(entry, "driveStiffness", 0.0f);
                b.driveDamping = jsonF32Or(entry, "driveDamping", 0.0f);
                b.driveMaxForce = jsonF32Or(entry, "driveMaxForce", 0.0f);
                c.bones.push_back(b);
            }
        }
        return {};
    }

    auto rigidbodyComponentToJson(const RigidbodyComponent& c) -> nlohmann::json
    {
        auto motionName = [](RigidbodyComponent::Motion m) -> const char*
        {
            switch (m)
            {
                case RigidbodyComponent::Motion::Static: return "static";
                case RigidbodyComponent::Motion::Kinematic: return "kinematic";
                case RigidbodyComponent::Motion::Dynamic: return "dynamic";
            }
            return "dynamic";
        };
        return nlohmann::json{ { "motion", motionName(c.motion) },
                               { "mass", c.mass },
                               { "linearDamping", c.linearDamping },
                               { "angularDamping", c.angularDamping },
                               { "gravityFactor", c.gravityFactor },
                               { "lockPosition", bvec3ToJson(c.lockPosition) },
                               { "lockRotation", bvec3ToJson(c.lockRotation) },
                               { "collisionLayer", c.collisionLayer } };
    }

    auto rigidbodyComponentFromJson(RigidbodyComponent& c, const nlohmann::json& j) -> Result<void>
    {
        auto motionFromName = [](const std::string& name) -> RigidbodyComponent::Motion
        {
            if (name == "static") { return RigidbodyComponent::Motion::Static; }
            if (name == "kinematic") { return RigidbodyComponent::Motion::Kinematic; }
            return RigidbodyComponent::Motion::Dynamic;
        };
        c.motion = motionFromName(jsonStringOr(j, "motion", std::string{ "dynamic" }));
        c.mass = jsonF32Or(j, "mass", 1.0f);
        c.linearDamping = jsonF32Or(j, "linearDamping", 0.05f);
        c.angularDamping = jsonF32Or(j, "angularDamping", 0.05f);
        c.gravityFactor = jsonF32Or(j, "gravityFactor", 1.0f);
        c.lockPosition = bvec3FromJson(j.value("lockPosition", nlohmann::json::object()));
        c.lockRotation = bvec3FromJson(j.value("lockRotation", nlohmann::json::object()));
        c.collisionLayer = static_cast<i32>(j.value("collisionLayer", 0));
        return {};
    }

    auto colliderComponentToJson(const ColliderComponent& c) -> nlohmann::json
    {
        auto shapeName = [](ColliderComponent::Shape s) -> const char*
        {
            switch (s)
            {
                case ColliderComponent::Shape::Box: return "box";
                case ColliderComponent::Shape::Sphere: return "sphere";
                case ColliderComponent::Shape::Capsule: return "capsule";
                case ColliderComponent::Shape::ConvexHull: return "convexhull";
                case ColliderComponent::Shape::Mesh: return "mesh";
            }
            return "box";
        };
        return nlohmann::json{ { "shape", shapeName(c.shape) },
                               { "halfExtents", vec3ToJson(c.halfExtents) },
                               { "sourceMesh", std::to_string(c.sourceMesh.value) },
                               { "offset", vec3ToJson(c.offset) },
                               { "material",
                                 nlohmann::json{ { "friction", c.material.friction },
                                                 { "restitution", c.material.restitution } } },
                               { "isSensor", c.isSensor } };
    }

    auto colliderComponentFromJson(ColliderComponent& c, const nlohmann::json& j) -> Result<void>
    {
        auto shapeFromName = [](const std::string& name) -> ColliderComponent::Shape
        {
            if (name == "sphere") { return ColliderComponent::Shape::Sphere; }
            if (name == "capsule") { return ColliderComponent::Shape::Capsule; }
            if (name == "convexhull") { return ColliderComponent::Shape::ConvexHull; }
            if (name == "mesh") { return ColliderComponent::Shape::Mesh; }
            return ColliderComponent::Shape::Box;
        };
        c.shape = shapeFromName(jsonStringOr(j, "shape", std::string{ "box" }));
        c.halfExtents = vec3FromJson(j.value("halfExtents", nlohmann::json::object()));
        c.sourceMesh = Uuid{ u64FromJson(j.value("sourceMesh", nlohmann::json{})) };
        c.offset = vec3FromJson(j.value("offset", nlohmann::json::object()));
        const nlohmann::json material = j.value("material", nlohmann::json::object());
        c.material.friction = jsonF32Or(material, "friction", 0.5f);
        c.material.restitution = jsonF32Or(material, "restitution", 0.0f);
        c.isSensor = jsonBoolOr(j, "isSensor", false);
        return {};
    }

    auto kinematicBonesComponentToJson(const KinematicBonesComponent& c) -> nlohmann::json
    {
        nlohmann::json driven = nlohmann::json::array();
        for (i32 index : c.driven)
        {
            driven.push_back(index);
        }
        return nlohmann::json{ { "enabled", c.enabled }, { "driven", std::move(driven) } };
    }

    auto kinematicBonesComponentFromJson(KinematicBonesComponent& c, const nlohmann::json& j) -> Result<void>
    {
        c.enabled = jsonBoolOr(j, "enabled", true);
        c.driven.clear();
        if (j.contains("driven") && j["driven"].is_array())
        {
            for (const nlohmann::json& entry : j["driven"])
            {
                if (entry.is_number())
                {
                    c.driven.push_back(static_cast<i32>(entry.get<i64>()));
                }
            }
        }
        return {};
    }

    auto characterControllerComponentToJson(const CharacterControllerComponent& c) -> nlohmann::json
    {
        // Only the authored movement params round-trip; the runtime velocity/ground state serialize
        // as their defaults (move-character writes them at play time).
        return nlohmann::json{ { "maxSpeed", c.maxSpeed },
                               { "maxSlopeAngle", c.maxSlopeAngle },
                               { "maxStepHeight", c.maxStepHeight },
                               { "gravityFactor", c.gravityFactor } };
    }

    auto characterControllerComponentFromJson(CharacterControllerComponent& c, const nlohmann::json& j)
        -> Result<void>
    {
        c.maxSpeed = jsonF32Or(j, "maxSpeed", 4.0f);
        c.maxSlopeAngle = jsonF32Or(j, "maxSlopeAngle", 0.785398f);
        c.maxStepHeight = jsonF32Or(j, "maxStepHeight", 0.3f);
        c.gravityFactor = jsonF32Or(j, "gravityFactor", 1.0f);
        c.desiredVelocity = glm::vec3(0.0f);
        c.verticalVelocity = 0.0f;
        c.onGround = false;
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
