module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

module Saffron.SceneEdit;

import Saffron.Core;
import Saffron.Scene;

namespace se
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

    void updateSceneEditCamera(SceneEditCamera& camera, const SceneEditCameraInput& input, f32 dt)
    {
        if (!input.active)
        {
            camera.controlling = false;
            return;
        }
        camera.controlling = true;  // latch so the drag keeps control if it leaves the rect

        camera.yaw += input.lookDelta.x * camera.lookSpeed;
        camera.pitch -= input.lookDelta.y * camera.lookSpeed;
        camera.pitch = glm::clamp(camera.pitch, -89.0f, 89.0f);

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
