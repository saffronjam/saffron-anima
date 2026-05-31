module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <imgui.h>
#include <ImGuizmo.h>

module Saffron.Editor;

import Saffron.Core;
import Saffron.Scene;

namespace se
{
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

    auto drawEditorBillboards(EditorContext& ctx, const CameraView& cam, float aspect,
                              ImVec2 vpPos, ImVec2 vpSize,
                              ImTextureID pointLightIcon, ImTextureID spotLightIcon,
                              ImTextureID cameraIcon) -> Entity
    {
        if (vpSize.x <= 0.0f || vpSize.y <= 0.0f) { return Entity{ entt::null }; }

        const glm::mat4 view = cam.view;
        glm::mat4 proj = cameraProjection(cam, aspect);
        const glm::vec4 vp{ 0.0f, 0.0f, vpSize.x, vpSize.y };

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float half = 12.0f;  // icon half-size in pixels
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        Entity hit{ entt::null };

        auto drawIcon = [&](Entity entity, ImTextureID icon)
        {
            if (icon == 0) { return; }
            if (!hasComponent<TransformComponent>(ctx.scene, entity)) { return; }
            const glm::vec3 worldPos = getComponent<TransformComponent>(ctx.scene, entity).translation;
            const glm::vec3 screen = glm::project(worldPos, view, proj, vp);
            if (screen.z < 0.0f || screen.z > 1.0f) { return; }
            const ImVec2 center{ vpPos.x + screen.x, vpPos.y + (vpSize.y - screen.y) };
            if (center.x < vpPos.x || center.x > vpPos.x + vpSize.x) { return; }
            if (center.y < vpPos.y || center.y > vpPos.y + vpSize.y) { return; }
            const ImVec2 mn{ center.x - half, center.y - half };
            const ImVec2 mx{ center.x + half, center.y + half };
            const bool sel = (ctx.selected.handle == entity.handle);
            dl->AddImage(icon, mn, mx, ImVec2{0,0}, ImVec2{1,1},
                         sel ? IM_COL32(255,200,80,255) : IM_COL32(200,200,200,200));
            if (clicked && mouse.x >= mn.x && mouse.x <= mx.x &&
                           mouse.y >= mn.y && mouse.y <= mx.y)
            {
                hit = entity;
            }
        };

        forEach<PointLightComponent>(ctx.scene, [&](Entity e, PointLightComponent&)
            { drawIcon(e, pointLightIcon); });
        forEach<SpotLightComponent>(ctx.scene, [&](Entity e, SpotLightComponent&)
            { drawIcon(e, spotLightIcon); });
        forEach<CameraComponent>(ctx.scene, [&](Entity e, CameraComponent&)
            { drawIcon(e, cameraIcon); });

        return hit;
    }
}
