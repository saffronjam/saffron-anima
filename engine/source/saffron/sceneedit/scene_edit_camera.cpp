module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

module Saffron.SceneEdit;

import Saffron.Core;
import Saffron.Scene;
import Saffron.Json;

namespace sa
{
    auto sceneEditCameraForward(const SceneEditCamera& camera) -> glm::vec3
    {
        const f32 yaw = glm::radians(camera.yaw);
        const f32 pitch = glm::radians(camera.pitch);
        return glm::normalize(
            glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch), -std::cos(pitch) * std::cos(yaw)));
    }

    auto sceneEditCameraView(const SceneEditCamera& camera) -> CameraView
    {
        CameraView result;
        const glm::vec3 forward = sceneEditCameraForward(camera);
        result.view = glm::lookAt(camera.position, camera.position + forward, glm::vec3(0.0f, 1.0f, 0.0f));
        result.fov = camera.fov;
        result.nearPlane = camera.nearPlane;
        result.farPlane = camera.farPlane;
        result.valid = true;
        return result;
    }

    auto sceneEditCameraToJson(const SceneEditCamera& camera) -> nlohmann::json
    {
        return nlohmann::json{ { "position", vec3ToJson(camera.position) },
                               { "yaw", camera.yaw },
                               { "pitch", camera.pitch },
                               { "fov", camera.fov } };
    }

    void sceneEditCameraFromJson(SceneEditCamera& camera, const nlohmann::json& j)
    {
        if (!j.is_object())
        {
            return;
        }
        if (j.contains("position"))
        {
            camera.position = vec3FromJson(j["position"]);
        }
        camera.yaw = jsonF32Or(j, "yaw", camera.yaw);
        camera.pitch = jsonF32Or(j, "pitch", camera.pitch);
        camera.fov = jsonF32Or(j, "fov", camera.fov);
    }

    void updateSceneEditCamera(SceneEditCamera& camera, const SceneEditCameraInput& input, f32 dt)
    {
        // Look samples stream over the control plane at ~60Hz; drain the pending delta
        // exponentially each rendered frame (same time constant as the gizmo drag) so
        // the look doesn't staircase. Runs while inactive too, easing the tail out.
        camera.lookPending += input.lookDelta;
        constexpr f32 tau = 0.025f;
        const f32 alpha = 1.0f - std::exp(-std::max(0.0f, dt) / tau);
        const glm::vec2 step = camera.lookPending * alpha;
        camera.lookPending -= step;
        camera.yaw += step.x * camera.lookSpeed;
        camera.pitch -= step.y * camera.lookSpeed;
        camera.pitch = glm::clamp(camera.pitch, -89.0f, 89.0f);

        if (!input.active)
        {
            camera.controlling = false;
            return;
        }
        camera.controlling = true;  // latch so the drag keeps control if it leaves the rect

        const glm::vec3 forward = sceneEditCameraForward(camera);
        const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
        const glm::vec3 worldUp{ 0.0f, 1.0f, 0.0f };
        const f32 speed = camera.moveSpeed * dt;
        if (input.forward)
        {
            camera.position += forward * speed;
        }
        if (input.back)
        {
            camera.position -= forward * speed;
        }
        if (input.right)
        {
            camera.position += right * speed;
        }
        if (input.left)
        {
            camera.position -= right * speed;
        }
        if (input.up)
        {
            camera.position += worldUp * speed;
        }
        if (input.down)
        {
            camera.position -= worldUp * speed;
        }
    }
}
