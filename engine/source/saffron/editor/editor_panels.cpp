module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

#include <functional>
#include <string>

module Saffron.Editor;

import Saffron.Core;
import Saffron.Scene;
import Saffron.Ui;

namespace se
{
    void hierarchyPanel(EditorContext& ctx)
    {
        ImGui::Begin("Hierarchy");

        if (ImGui::Button("Add +")) { ImGui::OpenPopup("AddEntityPreset"); }
        if (ImGui::BeginPopup("AddEntityPreset"))
        {
            if (ImGui::MenuItem("Empty"))
            {
                setSelection(ctx, createEntity(ctx.scene, "Entity"));
            }
            if (ImGui::MenuItem("Model") && ctx.onCreateCube)
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
            ImGui::EndPopup();
        }

        Entity toDelete{ entt::null };
        Entity toCopy{ entt::null };
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
                    if (ImGui::MenuItem("Copy"))  { toCopy  = entity; }
                    if (ImGui::MenuItem("Delete")) { toDelete = entity; }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            });

        // Structural changes deferred until after the iteration above.
        if (toCopy.handle != entt::null)
        {
            const std::string copyName = getComponent<NameComponent>(ctx.scene, toCopy).name + " (copy)";
            Entity fresh = createEntity(ctx.scene, copyName);
            for (const ComponentTraits& t : ctx.registry.rows)
            {
                if (t.has(ctx.scene, toCopy))
                {
                    t.addDefault(ctx.scene, fresh);
                    static_cast<void>(t.deserialize(ctx.scene, fresh, t.serialize(ctx.scene, toCopy)));
                }
            }
            setSelection(ctx, fresh);
        }
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

    void assetCatalogPanel(EditorContext& ctx, AssetCatalog* catalog,
                           const std::function<ImTextureID(const AssetEntry&)>& thumbnailFor,
                           const std::function<void(const AssetEntry&)>& onView,
                           ImTextureID eyeIcon)
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
            if (onView && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                onView(entry);
            }
            if (onView && ImGui::BeginPopupContextItem("tile_ctx"))
            {
                if (eyeIcon != 0)
                {
                    ImGui::Image(eyeIcon, ImVec2{ 14.0f, 14.0f });
                    ImGui::SameLine();
                }
                if (ImGui::MenuItem("View"))
                {
                    onView(entry);
                }
                ImGui::EndPopup();
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

    void viewerPanel(bool& open, const char* title, ImTextureID previewId)
    {
        if (!open)
        {
            return;
        }
        ImGui::SetNextWindowSize(ImVec2{ 480.0f, 520.0f }, ImGuiCond_FirstUseEver);
        if (ImGui::Begin(title && *title ? title : "Asset Viewer", &open))
        {
            if (previewId != 0)
            {
                const ImVec2 avail = ImGui::GetContentRegionAvail();
                const float side = avail.x < avail.y ? avail.x : avail.y;
                if (side > 0.0f)
                {
                    ImGui::Image(previewId, ImVec2{ side, side });
                }
            }
            else
            {
                ImGui::TextUnformatted("No preview available.");
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
}
