module;

// Editor panels: ImGui + entt + glm + json are header-heavy, so this module uses
// classic includes (no `import std`), like the rendering/ui/scene modules.
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <nlohmann/json.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <ImGuizmo.h>

#include <cmath>
#include <expected>
#include <format>
#include <functional>
#include <string>

export module Saffron.Editor;

import Saffron.Core;
import Saffron.Signal;
import Saffron.Scene;
import Saffron.Json;
import Saffron.Ui;

export namespace se
{
    // The viewport's own fly-camera (the scene-view eye, distinct from any ECS
    // CameraComponent / game camera). Hold RMB over the viewport to look + WASD to move,
    // Shift up / Ctrl down. yaw/pitch in degrees; at yaw 0 the camera looks down -Z.
    struct EditorCamera
    {
        glm::vec3 position{ 3.0f, 2.5f, 4.0f };
        f32 yaw = -37.0f;
        f32 pitch = -29.0f;
        f32 fov = 45.0f;
        f32 nearPlane = 0.1f;
        f32 farPlane = 100.0f;
        f32 moveSpeed = 6.0f;   // units/second
        f32 lookSpeed = 0.12f;  // degrees/pixel
        bool controlling = false;  // latched while RMB is held (so a drag can leave the rect)
    };

    // The editor's mutable state: the scene being edited, the component registry
    // that drives every panel, and the current selection (broadcast as a signal).
    struct EditorContext
    {
        Scene scene;
        ComponentRegistry registry;
        Entity selected{ entt::null };
        SubscriberList<Entity> onSelectionChanged;
        std::string scenePath;
        EditorCamera camera;

        // Imports a file into the asset catalog (File > Import, drag-and-drop, the asset
        // panel). The editor has no renderer/assets, so the client routes by extension.
        std::function<void(const std::string&)> onImport;
        // Save/load the whole project (asset catalog + scene); delegated for the same reason.
        std::function<void(const std::string&)> onSaveProject;
        std::function<void(const std::string&)> onLoadProject;
        std::string importPath;  // the Import dialog's text buffer

        // Spawns the bundled cube mesh (Create > Cube); delegated because the editor has
        // no AssetServer to resolve/upload the mesh itself.
        std::function<void()> onCreateCube;
        ImGuizmo::OPERATION gizmoOp = ImGuizmo::TRANSLATE;  // W/E/R cycle translate/rotate/scale
    };

    // The payload dragged from an asset tile onto a component picker field.
    struct AssetDragPayload
    {
        u64 id = 0;
        AssetType type = AssetType::Mesh;
    };

    void setSelection(EditorContext& ctx, Entity entity)
    {
        ctx.selected = entity;
        ctx.onSelectionChanged.publish(entity);
    }

    // A combo that picks a catalog asset of `type` into `target` (the component's Uuid),
    // showing each asset's thumbnail + name. Reads the catalog through scene.catalog
    // (borrowed; tolerates null). `thumbnailFor` maps an asset to an ImGui texture (0 = none).
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
        if (ImGui::BeginCombo(label, current.c_str()))
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

    // Concrete built-in component registration — the imgui draw lambdas + json serde
    // live here (the one place with both imgui and json). `thumbnailFor` is captured by
    // the Mesh/Material draws so their pickers can show asset thumbnails.
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

    // Heap-owned so EditorContext's heavy destructor (entt/json) is instantiated
    // here, not in the client TU. The editor holds only the pointer.
    auto newEditorContext() -> EditorContext*
    {
        EditorContext* ctx = new EditorContext();
        // Components are registered by the client via registerBuiltinComponents(reg,
        // thumbnailFor) once the thumbnail provider exists. Seeding entities below uses
        // entt directly, so it does not need the ComponentRegistry populated yet.

        // Seed a camera looking at the origin so a freshly spawned mesh is visible.
        Entity camera = createEntity(ctx->scene, "Camera");
        addComponent<CameraComponent>(ctx->scene, camera);
        TransformComponent& cameraTransform = getComponent<TransformComponent>(ctx->scene, camera);
        cameraTransform.translation = glm::vec3(3.0f, 2.5f, 4.0f);
        cameraTransform.rotation = glm::eulerAngles(
            glm::quatLookAt(glm::normalize(-cameraTransform.translation), glm::vec3(0.0f, 1.0f, 0.0f)));

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
            const bool open = propertyGridHeader(traits.name);
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
                ImGui::TreePop();
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

    // The shared "Import Asset" modal body (a path field → ctx.onImport). Drawn by the
    // asset panel; opened from there or from File ▸ Import via OpenPopup("Import Asset").
    void drawImportModal(EditorContext& ctx)
    {
        if (ImGui::BeginPopupModal("Import Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::InputText("Path", &ctx.importPath);
            if (ImGui::Button("Import") && ctx.onImport)
            {
                ctx.onImport(ctx.importPath);
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

    // The project asset catalog: a tile grid of imported assets (thumbnail + editable
    // name). Import via the button/modal or drag-and-drop; drag a tile onto a component
    // picker to assign it. `catalog` is mutable so names can be edited in place.
    void assetCatalogPanel(EditorContext& ctx, AssetCatalog* catalog,
                           const std::function<ImTextureID(const AssetEntry&)>& thumbnailFor)
    {
        ImGui::Begin("Assets");
        if (ImGui::Button("Import..."))
        {
            ImGui::OpenPopup("Import Asset");
        }
        drawImportModal(ctx);
        ImGui::Separator();

        if (catalog == nullptr || catalog->entries.empty())
        {
            ImGui::TextUnformatted("No assets yet — import or drag-and-drop a model or texture.");
            ImGui::End();
            return;
        }

        const float tileSize = 72.0f;
        const float cellWidth = tileSize + ImGui::GetStyle().ItemSpacing.x;
        int columns = static_cast<int>(ImGui::GetContentRegionAvail().x / cellWidth);
        if (columns < 1)
        {
            columns = 1;
        }

        int column = 0;
        for (AssetEntry& entry : catalog->entries)
        {
            ImGui::PushID(static_cast<int>(entry.id.value));
            ImGui::BeginGroup();

            ImTextureID thumb = 0;
            if (thumbnailFor)
            {
                thumb = thumbnailFor(entry);
            }
            if (thumb != 0)
            {
                ImGui::Image(thumb, ImVec2{ tileSize, tileSize });
            }
            else
            {
                const char* glyph = "?";
                if (entry.type == AssetType::Mesh)
                {
                    glyph = "MESH";
                }
                else if (entry.type == AssetType::Texture)
                {
                    glyph = "TEX";
                }
                ImGui::Button(glyph, ImVec2{ tileSize, tileSize });  // placeholder until thumbnails land
            }
            // The tile's last item is an Image (no ImGui ID) when it has a thumbnail;
            // SourceAllowNullID lets such an item be a drag source (id from its position).
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
            {
                AssetDragPayload payload{ entry.id.value, entry.type };
                ImGui::SetDragDropPayload("SE_ASSET", &payload, sizeof(payload));
                ImGui::TextUnformatted(entry.name.c_str());
                ImGui::EndDragDropSource();
            }

            ImGui::SetNextItemWidth(tileSize);
            ImGui::InputText("##name", &entry.name);  // rename in place (UTF-8)
            ImGui::EndGroup();
            ImGui::PopID();

            column = column + 1;
            if (column < columns)
            {
                ImGui::SameLine();
            }
            else
            {
                column = 0;
            }
        }
        ImGui::End();
    }

    void drawEditorMenuBar(EditorContext& ctx)
    {
        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                std::string path = ctx.scenePath;
                if (path.empty())
                {
                    path = "project.json";
                }

                // Project save/load is delegated (the editor has no AssetServer/Renderer
                // to write the asset catalog); the client wires saveProject/loadProject.
                if (ImGui::MenuItem("Save Project") && ctx.onSaveProject)
                {
                    ctx.onSaveProject(path);
                    ctx.scenePath = path;
                }
                if (ImGui::MenuItem("Load Project") && ctx.onLoadProject)
                {
                    ctx.onLoadProject(path);
                    ctx.scenePath = path;
                    setSelection(ctx, Entity{ entt::null });
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Import..."))
                {
                    ImGui::OpenPopup("Import Asset");  // body drawn by the asset panel
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
    }

    // The editor camera's forward (world space) from its yaw/pitch.
    auto editorCameraForward(const EditorCamera& camera) -> glm::vec3
    {
        const f32 yaw = glm::radians(camera.yaw);
        const f32 pitch = glm::radians(camera.pitch);
        return glm::normalize(glm::vec3(std::cos(pitch) * std::sin(yaw),
                                        std::sin(pitch),
                                        -std::cos(pitch) * std::cos(yaw)));
    }

    // The editor camera as a Scene CameraView (view + projection params), so renderScene
    // and the gizmo draw from the same eye.
    auto editorCameraView(const EditorCamera& camera) -> CameraView
    {
        CameraView result;
        const glm::vec3 forward = editorCameraForward(camera);
        result.view = glm::lookAt(camera.position, camera.position + forward, glm::vec3(0.0f, 1.0f, 0.0f));
        result.fov = camera.fov;
        result.nearPlane = camera.nearPlane;
        result.farPlane = camera.farPlane;
        result.valid = true;
        return result;
    }

    // Fly the editor camera while RMB is held over the viewport: mouse look + WASD move,
    // Shift up / Ctrl down (world Y). Reads ImGui input, so call from onUi each frame.
    void updateEditorCamera(EditorCamera& camera, bool viewportHovered, f32 dt)
    {
        ImGuiIO& io = ImGui::GetIO();
        const bool rmb = ImGui::IsMouseDown(ImGuiMouseButton_Right);
        if (!rmb || !(viewportHovered || camera.controlling))
        {
            camera.controlling = false;
            return;
        }
        camera.controlling = true;  // latch so the drag keeps control if it leaves the rect

        camera.yaw += io.MouseDelta.x * camera.lookSpeed;
        camera.pitch -= io.MouseDelta.y * camera.lookSpeed;
        camera.pitch = glm::clamp(camera.pitch, -89.0f, 89.0f);

        const glm::vec3 forward = editorCameraForward(camera);
        const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
        const glm::vec3 worldUp{ 0.0f, 1.0f, 0.0f };
        const f32 speed = camera.moveSpeed * dt;
        if (ImGui::IsKeyDown(ImGuiKey_W)) { camera.position += forward * speed; }
        if (ImGui::IsKeyDown(ImGuiKey_S)) { camera.position -= forward * speed; }
        if (ImGui::IsKeyDown(ImGuiKey_D)) { camera.position += right * speed; }
        if (ImGui::IsKeyDown(ImGuiKey_A)) { camera.position -= right * speed; }
        if (io.KeyShift) { camera.position += worldUp * speed; }
        if (io.KeyCtrl) { camera.position -= worldUp * speed; }
    }

    // In-viewport translate/rotate/scale gizmo for the selected entity. `proj` MUST be
    // the un-flipped projection (the Vulkan Y-flip stays local to the renderer) or the
    // gizmo mirrors vertically. Drawn into the "Viewport" window's draw list so it clips
    // to the panel and takes mouse input there. W/E/R cycle the op (not while flying the
    // camera with RMB). `imagePos`/`imageSize` are the viewport image's screen rect.
    void drawGizmo(EditorContext& ctx, const glm::mat4& view, const glm::mat4& proj,
                   ImVec2 imagePos, ImVec2 imageSize, bool hovered)
    {
        if (hovered && !ImGuizmo::IsUsing() && !ImGui::IsAnyItemActive() &&
            !ImGui::IsMouseDown(ImGuiMouseButton_Right))
        {
            if (ImGui::IsKeyPressed(ImGuiKey_W)) { ctx.gizmoOp = ImGuizmo::TRANSLATE; }
            if (ImGui::IsKeyPressed(ImGuiKey_E)) { ctx.gizmoOp = ImGuizmo::ROTATE; }
            if (ImGui::IsKeyPressed(ImGuiKey_R)) { ctx.gizmoOp = ImGuizmo::SCALE; }
        }

        Entity selected = ctx.selected;
        if (selected.handle == entt::null || !valid(ctx.scene, selected) ||
            !hasComponent<TransformComponent>(ctx.scene, selected) ||
            imageSize.x <= 0.0f || imageSize.y <= 0.0f)
        {
            return;
        }

        // Draw into the viewport window so the gizmo clips to the panel and takes its
        // mouse input.
        ImGui::Begin("Viewport");
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(imagePos.x, imagePos.y, imageSize.x, imageSize.y);

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
                // Apply rotation as a delta on the stored Euler so a pure translate/scale
                // drag doesn't rewrite (and snap) the rotation.
                const glm::vec3 deltaEuler = glm::eulerAngles(rotation) - transform.rotation;
                transform.translation = translation;
                transform.rotation += deltaEuler;
                transform.scale = scale;
            }
        }
        ImGui::End();
    }
}
