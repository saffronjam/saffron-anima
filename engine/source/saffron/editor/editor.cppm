module;

// Editor panels: ImGui + entt + glm + json are header-heavy, so this module uses
// classic includes (no `import std`), like the rendering/ui/scene modules.
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <nlohmann/json.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <ImGuizmo.h>

#include <expected>
#include <format>
#include <functional>
#include <string>

export module Saffron.Editor;

import Saffron.Core;
import Saffron.Signal;
import Saffron.Scene;

export namespace se
{
    // The editor's mutable state: the scene being edited, the component registry
    // that drives every panel, and the current selection (broadcast as a signal).
    struct EditorContext
    {
        Scene scene;
        ComponentRegistry registry;
        Entity selected{ entt::null };
        SubscriberList<Entity> onSelectionChanged;
        std::string scenePath;

        // Set by the client to import a model path (File > Import / drag-and-drop).
        // The editor has no renderer/assets, so importing is delegated to this hook.
        std::function<void(const std::string&)> onImportModel;
        std::string importPath;  // the Import dialog's text buffer

        // Spawns the bundled cube mesh (Create > Cube); delegated like onImportModel
        // because the editor has no AssetServer to resolve/upload the mesh itself.
        std::function<void()> onCreateCube;
        ImGuizmo::OPERATION gizmoOp = ImGuizmo::TRANSLATE;  // W/E/R cycle translate/rotate/scale
    };

    void setSelection(EditorContext& ctx, Entity entity)
    {
        ctx.selected = entity;
        ctx.onSelectionChanged.publish(entity);
    }

    // Concrete built-in component registration — the imgui draw lambdas + json
    // serde live here (the one place with both imgui and json). A future physics
    // or render module registers ITS components the same way; nothing else changes.
    void registerBuiltinComponents(ComponentRegistry& reg)
    {
        registerComponent<NameComponent>(reg, "Name",
            [](Scene& s, Entity e)
            {
                ImGui::InputText("Name", &getComponent<NameComponent>(s, e).name);
            },
            [](const NameComponent& c) { return nlohmann::json{ { "name", c.name } }; },
            [](NameComponent& c, const nlohmann::json& j) -> std::expected<void, std::string>
            {
                c.name = j.value("name", std::string{});
                return {};
            },
            false);

        registerComponent<TransformComponent>(reg, "Transform",
            [](Scene& s, Entity e)
            {
                TransformComponent& t = getComponent<TransformComponent>(s, e);
                ImGui::DragFloat3("Translation", &t.translation.x, 0.1f);
                glm::vec3 euler = glm::degrees(glm::eulerAngles(t.rotation));
                if (ImGui::DragFloat3("Rotation", &euler.x, 0.5f))
                {
                    t.rotation = glm::quat(glm::radians(euler));
                }
                ImGui::DragFloat3("Scale", &t.scale.x, 0.1f);
            },
            [](const TransformComponent& t)
            {
                return nlohmann::json{ { "translation", vec3ToJson(t.translation) },
                                       { "scale", vec3ToJson(t.scale) },
                                       { "rotation", quatToJson(t.rotation) } };
            },
            [](TransformComponent& t, const nlohmann::json& j) -> std::expected<void, std::string>
            {
                t.translation = vec3FromJson(j.value("translation", nlohmann::json::object()));
                t.scale = vec3FromJson(j.value("scale", nlohmann::json::object()));
                t.rotation = quatFromJson(j.value("rotation", nlohmann::json::object()));
                return {};
            },
            false);

        registerComponent<MeshComponent>(reg, "Mesh",
            [](Scene& s, Entity e)
            {
                MeshComponent& mesh = getComponent<MeshComponent>(s, e);
                ImGui::Text("Asset: %llu", static_cast<unsigned long long>(mesh.mesh.value));
            },
            [](const MeshComponent& c) { return nlohmann::json{ { "mesh", c.mesh.value } }; },
            [](MeshComponent& c, const nlohmann::json& j) -> std::expected<void, std::string>
            {
                c.mesh = Uuid{ j.value("mesh", u64{ 0 }) };
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
            {
                return nlohmann::json{ { "fov", c.fov }, { "near", c.nearPlane },
                                       { "far", c.farPlane }, { "primary", c.primary } };
            },
            [](CameraComponent& c, const nlohmann::json& j) -> std::expected<void, std::string>
            {
                c.fov = j.value("fov", 45.0f);
                c.nearPlane = j.value("near", 0.1f);
                c.farPlane = j.value("far", 100.0f);
                c.primary = j.value("primary", true);
                return {};
            },
            true);

        registerComponent<MaterialComponent>(reg, "Material",
            [](Scene& s, Entity e)
            {
                MaterialComponent& material = getComponent<MaterialComponent>(s, e);
                ImGui::ColorEdit4("Base Color", &material.baseColor.x);
                ImGui::Text("Albedo: %llu", static_cast<unsigned long long>(material.albedoTexture.value));
            },
            [](const MaterialComponent& c)
            {
                return nlohmann::json{ { "baseColor", vec4ToJson(c.baseColor) },
                                       { "albedoTexture", c.albedoTexture.value } };
            },
            [](MaterialComponent& c, const nlohmann::json& j) -> std::expected<void, std::string>
            {
                c.baseColor = vec4FromJson(j.value("baseColor", nlohmann::json::object()));
                c.albedoTexture = Uuid{ j.value("albedoTexture", u64{ 0 }) };
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
            {
                return nlohmann::json{ { "direction", vec3ToJson(c.direction) },
                                       { "color", vec3ToJson(c.color) },
                                       { "intensity", c.intensity }, { "ambient", c.ambient } };
            },
            [](DirectionalLightComponent& c, const nlohmann::json& j) -> std::expected<void, std::string>
            {
                c.direction = vec3FromJson(j.value("direction", nlohmann::json::object()));
                c.color = vec3FromJson(j.value("color", nlohmann::json::object()));
                c.intensity = j.value("intensity", 1.0f);
                c.ambient = j.value("ambient", 0.15f);
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
            {
                return nlohmann::json{ { "color", vec3ToJson(c.color) },
                                       { "intensity", c.intensity }, { "range", c.range } };
            },
            [](PointLightComponent& c, const nlohmann::json& j) -> std::expected<void, std::string>
            {
                c.color = vec3FromJson(j.value("color", nlohmann::json::object()));
                c.intensity = j.value("intensity", 5.0f);
                c.range = j.value("range", 10.0f);
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
            {
                return nlohmann::json{ { "direction", vec3ToJson(c.direction) },
                                       { "color", vec3ToJson(c.color) }, { "intensity", c.intensity },
                                       { "range", c.range }, { "innerAngle", c.innerAngle },
                                       { "outerAngle", c.outerAngle } };
            },
            [](SpotLightComponent& c, const nlohmann::json& j) -> std::expected<void, std::string>
            {
                c.direction = vec3FromJson(j.value("direction", nlohmann::json::object()));
                c.color = vec3FromJson(j.value("color", nlohmann::json::object()));
                c.intensity = j.value("intensity", 5.0f);
                c.range = j.value("range", 10.0f);
                c.innerAngle = j.value("innerAngle", 20.0f);
                c.outerAngle = j.value("outerAngle", 30.0f);
                return {};
            },
            true);
    }

    // Heap-owned so EditorContext's heavy destructor (entt/json) is instantiated
    // here, not in the client TU. The editor holds only the pointer.
    EditorContext* newEditorContext()
    {
        EditorContext* ctx = new EditorContext();
        registerBuiltinComponents(ctx->registry);

        // Seed a camera looking at the origin so a freshly spawned mesh is visible.
        Entity camera = createEntity(ctx->scene, "Camera");
        addComponent<CameraComponent>(ctx->scene, camera);
        TransformComponent& cameraTransform = getComponent<TransformComponent>(ctx->scene, camera);
        cameraTransform.translation = glm::vec3(3.0f, 2.5f, 4.0f);
        cameraTransform.rotation =
            glm::quatLookAt(glm::normalize(-cameraTransform.translation), glm::vec3(0.0f, 1.0f, 0.0f));

        Entity sun = createEntity(ctx->scene, "Sun");
        addComponent<DirectionalLightComponent>(ctx->scene, sun);

        setSelection(*ctx, camera);
        return ctx;
    }

    void destroyEditorContext(EditorContext* ctx)
    {
        delete ctx;
    }

    void hierarchyPanel(EditorContext& ctx)
    {
        ImGui::Begin("Hierarchy");

        if (ImGui::Button("Add Entity"))
        {
            setSelection(ctx, createEntity(ctx.scene, "Entity"));
        }

        Entity toDelete{ entt::null };
        forEach<IdComponent, NameComponent>(ctx.scene,
            [&](Entity entity, IdComponent& id, NameComponent& name)
            {
                ImGui::PushID(static_cast<int>(id.id.value));
                const bool isSelected = entity.handle == ctx.selected.handle;
                if (ImGui::Selectable(name.name.c_str(), isSelected))
                {
                    setSelection(ctx, entity);
                }
                if (ImGui::BeginPopupContextItem())
                {
                    if (ImGui::MenuItem("Delete"))
                    {
                        toDelete = entity;
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            });

        // Structural change deferred until after the iteration above.
        if (toDelete.handle != entt::null)
        {
            if (ctx.selected.handle == toDelete.handle)
            {
                setSelection(ctx, Entity{ entt::null });
            }
            destroyEntity(ctx.scene, toDelete);
        }

        ImGui::End();
    }

    // Registry-driven: iterates ComponentTraits rows, no per-component switch.
    void inspectorPanel(EditorContext& ctx)
    {
        ImGui::Begin("Inspector");

        Entity selected = ctx.selected;
        if (selected.handle == entt::null || !valid(ctx.scene, selected))
        {
            ImGui::TextUnformatted("No entity selected");
            ImGui::End();
            return;
        }

        const ComponentTraits* toRemove = nullptr;
        for (const ComponentTraits& traits : ctx.registry.rows)
        {
            if (!traits.has(ctx.scene, selected))
            {
                continue;
            }
            ImGui::PushID(static_cast<int>(traits.id));
            const bool open = ImGui::CollapsingHeader(traits.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
            if (traits.removable && ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Remove component"))
                {
                    toRemove = &traits;
                }
                ImGui::EndPopup();
            }
            if (open)
            {
                traits.drawInspector(ctx.scene, selected);
            }
            ImGui::PopID();
        }
        if (toRemove != nullptr)
        {
            toRemove->remove(ctx.scene, selected);
        }

        ImGui::Separator();
        if (ImGui::Button("Add Component"))
        {
            ImGui::OpenPopup("AddComponent");
        }
        if (ImGui::BeginPopup("AddComponent"))
        {
            for (const ComponentTraits& traits : ctx.registry.rows)
            {
                if (!traits.has(ctx.scene, selected) && ImGui::MenuItem(traits.name.c_str()))
                {
                    traits.addDefault(ctx.scene, selected);
                }
            }
            ImGui::EndPopup();
        }

        ImGui::End();
    }

    void drawEditorMenuBar(EditorContext& ctx)
    {
        bool openImport = false;
        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                std::string path = ctx.scenePath;
                if (path.empty())
                {
                    path = "scene.json";
                }

                if (ImGui::MenuItem("Save Scene"))
                {
                    std::expected<void, std::string> result = writeScene(ctx.registry, ctx.scene, path);
                    if (!result)
                    {
                        logError(result.error());
                    }
                    else
                    {
                        ctx.scenePath = path;
                        logInfo(std::format("saved scene to {}", path));
                    }
                }
                if (ImGui::MenuItem("Load Scene"))
                {
                    std::expected<void, std::string> result = readScene(ctx.registry, ctx.scene, path);
                    if (!result)
                    {
                        logError(result.error());
                    }
                    else
                    {
                        ctx.scenePath = path;
                        setSelection(ctx, Entity{ entt::null });
                        logInfo(std::format("loaded scene from {}", path));
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Import Model..."))
                {
                    openImport = true;
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Create"))
            {
                if (ImGui::MenuItem("Empty"))
                {
                    setSelection(ctx, createEntity(ctx.scene, "Entity"));
                }
                if (ImGui::MenuItem("Cube") && ctx.onCreateCube)
                {
                    ctx.onCreateCube();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Point Light"))
                {
                    Entity e = createEntity(ctx.scene, "Point Light");
                    addComponent<PointLightComponent>(ctx.scene, e);
                    getComponent<TransformComponent>(ctx.scene, e).translation = glm::vec3(0.0f, 2.0f, 0.0f);
                    setSelection(ctx, e);
                }
                if (ImGui::MenuItem("Spot Light"))
                {
                    Entity e = createEntity(ctx.scene, "Spot Light");
                    addComponent<SpotLightComponent>(ctx.scene, e);
                    getComponent<TransformComponent>(ctx.scene, e).translation = glm::vec3(0.0f, 4.0f, 0.0f);
                    setSelection(ctx, e);
                }
                if (ImGui::MenuItem("Directional Light"))
                {
                    Entity e = createEntity(ctx.scene, "Directional Light");
                    addComponent<DirectionalLightComponent>(ctx.scene, e);
                    setSelection(ctx, e);
                }
                if (ImGui::MenuItem("Camera"))
                {
                    Entity e = createEntity(ctx.scene, "Camera");
                    addComponent<CameraComponent>(ctx.scene, e);
                    setSelection(ctx, e);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        if (openImport)
        {
            ImGui::OpenPopup("Import Model");
        }
        if (ImGui::BeginPopupModal("Import Model", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::InputText("Path", &ctx.importPath);
            if (ImGui::Button("Import") && ctx.onImportModel)
            {
                ctx.onImportModel(ctx.importPath);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // In-viewport translate/rotate/scale gizmo for the selected entity. `proj` MUST be
    // the un-flipped projection (the Vulkan Y-flip stays local to the renderer) or the
    // gizmo mirrors vertically. Call from onUi after viewportPanel, passing the viewport
    // image's screen rect. W/E/R cycle the operation while hovering the viewport.
    void drawGizmo(EditorContext& ctx, const glm::mat4& view, const glm::mat4& proj,
                   ImVec2 panelPos, ImVec2 panelSize, bool hovered)
    {
        if (hovered && !ImGuizmo::IsUsing() && !ImGui::IsAnyItemActive())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_W)) { ctx.gizmoOp = ImGuizmo::TRANSLATE; }
            if (ImGui::IsKeyPressed(ImGuiKey_E)) { ctx.gizmoOp = ImGuizmo::ROTATE; }
            if (ImGui::IsKeyPressed(ImGuiKey_R)) { ctx.gizmoOp = ImGuizmo::SCALE; }
        }

        Entity selected = ctx.selected;
        if (selected.handle == entt::null || !valid(ctx.scene, selected) ||
            !hasComponent<TransformComponent>(ctx.scene, selected) ||
            panelSize.x <= 0.0f || panelSize.y <= 0.0f)
        {
            return;
        }

        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
        ImGuizmo::SetRect(panelPos.x, panelPos.y, panelSize.x, panelSize.y);

        TransformComponent& transform = getComponent<TransformComponent>(ctx.scene, selected);
        glm::mat4 model = transformMatrix(transform);
        ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj),
                             ctx.gizmoOp, ImGuizmo::WORLD, glm::value_ptr(model));

        if (ImGuizmo::IsUsing())
        {
            glm::vec3 translation{ 0.0f };
            glm::vec3 scale{ 1.0f };
            glm::vec3 skew{ 0.0f };
            glm::vec4 perspective{ 0.0f };
            glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
            if (glm::decompose(model, scale, rotation, translation, skew, perspective))
            {
                transform.translation = translation;
                transform.rotation = rotation;
                transform.scale = scale;
            }
        }
    }
}
