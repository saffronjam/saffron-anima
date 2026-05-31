module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <functional>
#include <string>

module Saffron.Editor;

import Saffron.Core;
import Saffron.Scene;
import Saffron.Json;
import Saffron.Ui;

namespace se
{
    void drawAssetPicker(Scene& scene, AssetType type, const char* label, Uuid& target,
                         const std::function<ImTextureID(const AssetEntry&)>& thumbnailFor)
    {
        const AssetCatalog* catalog = scene.catalog;
        std::string current = "(none)";
        if (catalog != nullptr)
        {
            const AssetEntry* entry = findAsset(*catalog, target);
            if (entry != nullptr)
            {
                current = entry->name;
            }
        }
        ImGui::TextUnformatted(label);
        const std::string comboId = std::string("##") + label;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo(comboId.c_str(), current.c_str()))
        {
            if (ImGui::Selectable("(none)", target.value == 0))
            {
                target = Uuid{ 0 };
            }
            if (catalog != nullptr)
            {
                for (const AssetEntry& entry : catalog->entries)
                {
                    if (entry.type != type)
                    {
                        continue;
                    }
                    ImGui::PushID(static_cast<int>(entry.id.value));
                    ImTextureID thumb = 0;
                    if (thumbnailFor)
                    {
                        thumb = thumbnailFor(entry);
                    }
                    if (thumb != 0)
                    {
                        ImGui::Image(thumb, ImVec2{ 16.0f, 16.0f });
                        ImGui::SameLine();
                    }
                    if (ImGui::Selectable(entry.name.c_str(), entry.id.value == target.value))
                    {
                        target = entry.id;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndCombo();
        }
        // Accept an asset tile dragged from the catalog panel (matching type only).
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SE_ASSET"))
            {
                const AssetDragPayload* drag = static_cast<const AssetDragPayload*>(payload->Data);
                if (drag != nullptr && drag->type == type)
                {
                    target = Uuid{ drag->id };
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    void registerBuiltinComponents(ComponentRegistry& reg,
                                   std::function<ImTextureID(const AssetEntry&)> thumbnailFor)
    {
        registerComponent<NameComponent>(reg, "Name",
            [](Scene& s, Entity e)
        {
                ImGui::InputText("##name", &getComponent<NameComponent>(s, e).name);
            },
            [](const NameComponent& c) -> nlohmann::json { return nlohmann::json{ { "name", c.name } }; },
            [](NameComponent& c, const nlohmann::json& j) -> Result<void>
            {
                c.name = jsonStringOr(j, "name", std::string{});
                return {};
            },
            false);

        registerComponent<TransformComponent>(reg, "Transform",
            [](Scene& s, Entity e)
        {
                TransformComponent& t = getComponent<TransformComponent>(s, e);
                vec3Control("Translation", &t.translation.x);
                glm::vec3 degrees = glm::degrees(t.rotation);
                if (vec3Control("Rotation", &degrees.x))
                {
                    t.rotation = glm::radians(degrees);
                }
                vec3Control("Scale", &t.scale.x, 1.0f);
            },
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
            [thumbnailFor](Scene& s, Entity e)
        {
                MeshComponent& mesh = getComponent<MeshComponent>(s, e);
                drawAssetPicker(s, AssetType::Mesh, "Mesh", mesh.mesh, thumbnailFor);
            },
            [](const MeshComponent& c) -> nlohmann::json { return nlohmann::json{ { "mesh", c.mesh.value } }; },
            [](MeshComponent& c, const nlohmann::json& j) -> Result<void>
            {
                c.mesh = Uuid{ jsonU64Or(j, "mesh", 0) };
                return {};
            },
            true);

        registerComponent<CameraComponent>(reg, "Camera",
            [](Scene& s, Entity e)
        {
                CameraComponent& camera = getComponent<CameraComponent>(s, e);
                ImGui::DragFloat("FOV", &camera.fov, 0.5f, 1.0f, 179.0f);
                ImGui::DragFloat("Near", &camera.nearPlane, 0.01f, 0.001f, camera.farPlane);
                ImGui::DragFloat("Far", &camera.farPlane, 1.0f, camera.nearPlane, 10000.0f);
                ImGui::Checkbox("Primary", &camera.primary);
            },
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
            [thumbnailFor](Scene& s, Entity e)
        {
                MaterialComponent& material = getComponent<MaterialComponent>(s, e);
                ImGui::ColorEdit4("Base Color", &material.baseColor.x);
                drawAssetPicker(s, AssetType::Texture, "Albedo", material.albedoTexture, thumbnailFor);
                ImGui::Checkbox("Unlit", &material.unlit);
            },
            [](const MaterialComponent& c)
            -> nlohmann::json {
                return nlohmann::json{ { "baseColor", vec4ToJson(c.baseColor) },
                                       { "albedoTexture", c.albedoTexture.value },
                                       { "unlit", c.unlit } };
            },
            [](MaterialComponent& c, const nlohmann::json& j) -> Result<void>
            {
                c.baseColor = vec4FromJson(j.value("baseColor", nlohmann::json::object()));
                c.albedoTexture = Uuid{ jsonU64Or(j, "albedoTexture", 0) };
                c.unlit = jsonBoolOr(j, "unlit", false);
                return {};
            },
            true);

        registerComponent<DirectionalLightComponent>(reg, "DirectionalLight",
            [](Scene& s, Entity e)
        {
                DirectionalLightComponent& light = getComponent<DirectionalLightComponent>(s, e);
                ImGui::DragFloat3("Direction", &light.direction.x, 0.01f);
                ImGui::ColorEdit3("Color", &light.color.x);
                ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0.0f, 50.0f);
                ImGui::DragFloat("Ambient", &light.ambient, 0.01f, 0.0f, 1.0f);
            },
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
            [](Scene& s, Entity e)
        {
                PointLightComponent& light = getComponent<PointLightComponent>(s, e);
                ImGui::ColorEdit3("Color", &light.color.x);
                ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0.0f, 100.0f);
                ImGui::DragFloat("Range", &light.range, 0.05f, 0.0f, 200.0f);
            },
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
            [](Scene& s, Entity e)
        {
                SpotLightComponent& light = getComponent<SpotLightComponent>(s, e);
                ImGui::DragFloat3("Direction", &light.direction.x, 0.01f);
                ImGui::ColorEdit3("Color", &light.color.x);
                ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0.0f, 100.0f);
                ImGui::DragFloat("Range", &light.range, 0.05f, 0.0f, 200.0f);
                ImGui::DragFloat("Inner Angle", &light.innerAngle, 0.1f, 0.0f, 89.0f);
                ImGui::DragFloat("Outer Angle", &light.outerAngle, 0.1f, 0.0f, 89.0f);
            },
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
    }
}
