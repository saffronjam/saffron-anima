module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <cmath>

module Saffron.Editor;

import Saffron.Core;
import Saffron.Scene;

namespace se
{
    auto editorCameraForward(const EditorCamera& camera) -> glm::vec3
    {
        const f32 yaw = glm::radians(camera.yaw);
        const f32 pitch = glm::radians(camera.pitch);
        return glm::normalize(glm::vec3(std::cos(pitch) * std::sin(yaw),
                                        std::sin(pitch),
                                        -std::cos(pitch) * std::cos(yaw)));
    }

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
}
