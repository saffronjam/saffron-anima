module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <string>

module Saffron.SceneEdit;

import Saffron.Core;
import Saffron.Scene;
import Saffron.Json;

namespace se
{
    void registerBuiltinComponents(ComponentRegistry& reg)
    {
        registerComponent<NameComponent>(reg, "Name",
            [](Scene&, Entity) {},
            [](const NameComponent& c) -> nlohmann::json { return nlohmann::json{ { "name", c.name } }; },
            [](NameComponent& c, const nlohmann::json& j) -> Result<void>
            {
                c.name = jsonStringOr(j, "name", std::string{});
                return {};
            },
            false);

        registerComponent<TransformComponent>(reg, "Transform",
            [](Scene&, Entity) {},
            [](const TransformComponent& t)
            -> nlohmann::json {
                return nlohmann::json{ { "translation", vec3ToJson(t.translation) },
                                       { "scale", vec3ToJson(t.scale) },
                                       { "rotation", vec3ToJson(t.rotation) } };
            },
            [](TransformComponent& t, const nlohmann::json& j) -> Result<void>
            {
                t.translation = vec3FromJson(j.value("translation", nlohmann::json::object()));
                t.scale = vec3FromJson(j.value("scale", nlohmann::json::object()));
                t.rotation = vec3FromJson(j.value("rotation", nlohmann::json::object()));
                return {};
            },
            false);

        registerComponent<MeshComponent>(reg, "Mesh",
            [](Scene&, Entity) {},
            [](const MeshComponent& c) -> nlohmann::json { return nlohmann::json{ { "mesh", uuidToJson(c.mesh.value) } }; },
            [](MeshComponent& c, const nlohmann::json& j) -> Result<void>
            {
                c.mesh = Uuid{ jsonU64Or(j, "mesh", 0) };
                return {};
            },
            true);

        registerComponent<CameraComponent>(reg, "Camera",
            [](Scene&, Entity) {},
            [](const CameraComponent& c)
            -> nlohmann::json {
                return nlohmann::json{ { "fov", c.fov }, { "near", c.nearPlane },
                                       { "far", c.farPlane }, { "primary", c.primary } };
            },
            [](CameraComponent& c, const nlohmann::json& j) -> Result<void>
            {
                c.fov = jsonF32Or(j, "fov", 45.0f);
                c.nearPlane = jsonF32Or(j, "near", 0.1f);
                c.farPlane = jsonF32Or(j, "far", 100.0f);
                c.primary = jsonBoolOr(j, "primary", true);
                return {};
            },
            true);

        registerComponent<MaterialComponent>(reg, "Material",
            [](Scene&, Entity) {},
            [](const MaterialComponent& c)
            -> nlohmann::json {
                return nlohmann::json{ { "baseColor", vec4ToJson(c.baseColor) },
                                       { "albedoTexture", uuidToJson(c.albedoTexture.value) },
                                       { "metallic", c.metallic },
                                       { "roughness", c.roughness },
                                       { "emissive", vec3ToJson(c.emissive) },
                                       { "emissiveStrength", c.emissiveStrength },
                                       { "unlit", c.unlit } };
            },
            [](MaterialComponent& c, const nlohmann::json& j) -> Result<void>
            {
                c.baseColor = vec4FromJson(j.value("baseColor", nlohmann::json::object()));
                c.albedoTexture = Uuid{ jsonU64Or(j, "albedoTexture", 0) };
                c.metallic = jsonF32Or(j, "metallic", 0.0f);
                c.roughness = jsonF32Or(j, "roughness", 1.0f);
                c.emissive = vec3FromJson(j.value("emissive", nlohmann::json::object()));
                c.emissiveStrength = jsonF32Or(j, "emissiveStrength", 1.0f);
                c.unlit = jsonBoolOr(j, "unlit", false);
                return {};
            },
            true);

        registerComponent<DirectionalLightComponent>(reg, "DirectionalLight",
            [](Scene&, Entity) {},
            [](const DirectionalLightComponent& c)
            -> nlohmann::json {
                return nlohmann::json{ { "direction", vec3ToJson(c.direction) },
                                       { "color", vec3ToJson(c.color) },
                                       { "intensity", c.intensity }, { "ambient", c.ambient } };
            },
            [](DirectionalLightComponent& c, const nlohmann::json& j) -> Result<void>
            {
                c.direction = vec3FromJson(j.value("direction", nlohmann::json::object()));
                c.color = vec3FromJson(j.value("color", nlohmann::json::object()));
                c.intensity = jsonF32Or(j, "intensity", 1.0f);
                c.ambient = jsonF32Or(j, "ambient", 0.15f);
                return {};
            },
            true);

        registerComponent<PointLightComponent>(reg, "PointLight",
            [](Scene&, Entity) {},
            [](const PointLightComponent& c)
            -> nlohmann::json {
                return nlohmann::json{ { "color", vec3ToJson(c.color) },
                                       { "intensity", c.intensity }, { "range", c.range } };
            },
            [](PointLightComponent& c, const nlohmann::json& j) -> Result<void>
            {
                c.color = vec3FromJson(j.value("color", nlohmann::json::object()));
                c.intensity = jsonF32Or(j, "intensity", 5.0f);
                c.range = jsonF32Or(j, "range", 10.0f);
                return {};
            },
            true);

        registerComponent<SpotLightComponent>(reg, "SpotLight",
            [](Scene&, Entity) {},
            [](const SpotLightComponent& c)
            -> nlohmann::json {
                return nlohmann::json{ { "direction", vec3ToJson(c.direction) },
                                       { "color", vec3ToJson(c.color) }, { "intensity", c.intensity },
                                       { "range", c.range }, { "innerAngle", c.innerAngle },
                                       { "outerAngle", c.outerAngle } };
            },
            [](SpotLightComponent& c, const nlohmann::json& j) -> Result<void>
            {
                c.direction = vec3FromJson(j.value("direction", nlohmann::json::object()));
                c.color = vec3FromJson(j.value("color", nlohmann::json::object()));
                c.intensity = jsonF32Or(j, "intensity", 5.0f);
                c.range = jsonF32Or(j, "range", 10.0f);
                c.innerAngle = jsonF32Or(j, "innerAngle", 20.0f);
                c.outerAngle = jsonF32Or(j, "outerAngle", 30.0f);
                return {};
            },
            true);

        registerComponent<ReflectionProbeComponent>(reg, "ReflectionProbe",
            [](Scene&, Entity) {},
            [](const ReflectionProbeComponent& c)
            -> nlohmann::json {
                return nlohmann::json{ { "influenceRadius", c.influenceRadius },
                                       { "intensity", c.intensity },
                                       { "boxProjection", c.boxProjection },
                                       { "boxExtent", vec3ToJson(c.boxExtent) } };
            },
            [](ReflectionProbeComponent& c, const nlohmann::json& j) -> Result<void>
            {
                c.influenceRadius = jsonF32Or(j, "influenceRadius", 10.0f);
                c.intensity = jsonF32Or(j, "intensity", 1.0f);
                c.boxProjection = jsonBoolOr(j, "boxProjection", false);
                c.boxExtent = vec3FromJson(j.value("boxExtent", nlohmann::json::object()));
                c.dirty = true;  // loaded probes start dirty -> captured on the first frame
                return {};
            },
            true);
    }
}
