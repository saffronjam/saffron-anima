module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

module Saffron.Editor;

import Saffron.Core;
import Saffron.Scene;

namespace se
{
    void syncNativeGizmo(EditorContext& ctx)
    {
        ctx.nativeGizmo.mode = ctx.gizmoOp == GizmoOp::Rotate ? NativeGizmoMode::Rotate
                             : ctx.gizmoOp == GizmoOp::Scale  ? NativeGizmoMode::Scale
                                                              : NativeGizmoMode::Translate;
        ctx.nativeGizmo.space = ctx.gizmoSpace == GizmoSpace::Local ? NativeGizmoSpace::Local
                                                                    : NativeGizmoSpace::World;
    }

    auto viewportProject(const CameraView& cam, u32 width, u32 height, glm::vec3 world) -> GizmoProjection
    {
        if (width == 0 || height == 0)
        {
            return {};
        }
        const glm::mat4 proj = cameraProjection(cam, static_cast<f32>(width) / static_cast<f32>(height));
        const glm::vec4 clip = proj * cam.view * glm::vec4(world, 1.0f);
        if (std::abs(clip.w) < 0.0001f)
        {
            return {};
        }
        const glm::vec3 ndc3 = glm::vec3(clip) / clip.w;
        if (ndc3.z < -1.0f || ndc3.z > 1.0f)
        {
            return {};
        }
        return GizmoProjection{
            .pixel = glm::vec2{ (ndc3.x * 0.5f + 0.5f) * static_cast<f32>(width),
                                (ndc3.y * 0.5f + 0.5f) * static_cast<f32>(height) },
            .ndc = glm::vec2{ ndc3.x, ndc3.y },
            .visible = true
        };
    }

    auto pixelToNdc(glm::vec2 p, u32 width, u32 height) -> glm::vec2
    {
        return glm::vec2{ p.x / static_cast<f32>(width) * 2.0f - 1.0f,
                          p.y / static_cast<f32>(height) * 2.0f - 1.0f };
    }

    auto cameraPosition(const CameraView& cam) -> glm::vec3
    {
        const glm::mat4 inverseView = glm::inverse(cam.view);
        return glm::vec3(inverseView[3]);
    }

    auto pointSegmentDistance(glm::vec2 p, glm::vec2 a, glm::vec2 b) -> f32
    {
        const glm::vec2 ab = b - a;
        const f32 denom = glm::dot(ab, ab);
        if (denom < 0.0001f)
        {
            return glm::length(p - a);
        }
        const f32 t = std::clamp(glm::dot(p - a, ab) / denom, 0.0f, 1.0f);
        return glm::length(p - (a + ab * t));
    }

    auto axisColor(NativeGizmoHandle handle, const NativeGizmoState& gizmo) -> glm::vec4
    {
        if (gizmo.active == handle || (gizmo.active == NativeGizmoHandle::None && gizmo.hovered == handle))
        {
            return glm::vec4{ 1.0f, 0.82f, 0.18f, 1.0f };
        }
        if (handle == NativeGizmoHandle::X) { return glm::vec4{ 0.93f, 0.18f, 0.20f, 1.0f }; }
        if (handle == NativeGizmoHandle::Y) { return glm::vec4{ 0.20f, 0.82f, 0.25f, 1.0f }; }
        if (handle == NativeGizmoHandle::Z) { return glm::vec4{ 0.22f, 0.42f, 0.98f, 1.0f }; }
        return glm::vec4{ 0.93f, 0.93f, 0.95f, 0.75f };
    }

    auto gizmoAxes(const TransformComponent& transform, NativeGizmoSpace space) -> std::array<glm::vec3, 3>
    {
        if (space == NativeGizmoSpace::World)
        {
            return { glm::vec3{ 1.0f, 0.0f, 0.0f }, glm::vec3{ 0.0f, 1.0f, 0.0f }, glm::vec3{ 0.0f, 0.0f, 1.0f } };
        }
        const glm::quat q = glm::quat(transform.rotation);
        return { q * glm::vec3{ 1.0f, 0.0f, 0.0f }, q * glm::vec3{ 0.0f, 1.0f, 0.0f }, q * glm::vec3{ 0.0f, 0.0f, 1.0f } };
    }

    auto handleAxis(NativeGizmoHandle handle, const std::array<glm::vec3, 3>& axes) -> glm::vec3
    {
        if (handle == NativeGizmoHandle::X) { return axes[0]; }
        if (handle == NativeGizmoHandle::Y) { return axes[1]; }
        if (handle == NativeGizmoHandle::Z) { return axes[2]; }
        return glm::vec3{ 0.0f };
    }

    auto hitNativeGizmo(EditorContext& editor, const CameraView& cam, u32 width, u32 height, glm::vec2 mouse)
        -> NativeGizmoHandle
    {
        if (editor.selected.handle == entt::null || !hasComponent<TransformComponent>(editor.scene, editor.selected))
        {
            return NativeGizmoHandle::None;
        }
        TransformComponent& transform = getComponent<TransformComponent>(editor.scene, editor.selected);
        const GizmoProjection origin = viewportProject(cam, width, height, transform.translation);
        if (!origin.visible)
        {
            return NativeGizmoHandle::None;
        }
        const auto axes = gizmoAxes(transform, editor.nativeGizmo.space);
        const f32 distance = glm::length(cameraPosition(cam) - transform.translation);
        const f32 axisLen = std::max(0.75f, distance * 0.22f);
        const std::array<NativeGizmoHandle, 3> handles{ NativeGizmoHandle::X, NativeGizmoHandle::Y, NativeGizmoHandle::Z };

        if (editor.nativeGizmo.mode == NativeGizmoMode::Scale && glm::length(mouse - origin.pixel) < 12.0f)
        {
            return NativeGizmoHandle::Uniform;
        }

        for (u32 i = 0; i < 3; i = i + 1)
        {
            const GizmoProjection end = viewportProject(cam, width, height, transform.translation + axes[i] * axisLen);
            if (!end.visible)
            {
                continue;
            }
            if (pointSegmentDistance(mouse, origin.pixel, end.pixel) < 9.0f)
            {
                return handles[i];
            }
        }

        if (editor.nativeGizmo.mode == NativeGizmoMode::Translate)
        {
            const std::array<std::pair<NativeGizmoHandle, glm::vec2>, 3> planeCenters{
                std::pair{ NativeGizmoHandle::XY, glm::vec2{ 0.24f, 0.24f } },
                std::pair{ NativeGizmoHandle::YZ, glm::vec2{ 0.48f, 0.24f } },
                std::pair{ NativeGizmoHandle::XZ, glm::vec2{ 0.24f, 0.48f } }
            };
            for (const auto& [handle, uv] : planeCenters)
            {
                const glm::vec3 p = transform.translation +
                                    axes[handle == NativeGizmoHandle::YZ ? 1 : 0] * axisLen * uv.x +
                                    axes[handle == NativeGizmoHandle::XY ? 1 : 2] * axisLen * uv.y;
                const GizmoProjection center = viewportProject(cam, width, height, p);
                if (center.visible && glm::all(glm::lessThan(glm::abs(mouse - center.pixel), glm::vec2{ 12.0f })))
                {
                    return handle;
                }
            }
        }
        return NativeGizmoHandle::None;
    }

    void applyNativeGizmoDrag(EditorContext& editor, const CameraView& cam, u32 width, u32 height, glm::vec2 mouse)
    {
        NativeGizmoState& gizmo = editor.nativeGizmo;
        if (!gizmo.dragging || gizmo.target.handle == entt::null ||
            !hasComponent<TransformComponent>(editor.scene, gizmo.target))
        {
            return;
        }
        TransformComponent& transform = getComponent<TransformComponent>(editor.scene, gizmo.target);
        const auto axes = gizmoAxes(transform, gizmo.space);
        const glm::vec2 delta = mouse - gizmo.startMouse;
        const f32 distance = glm::length(cameraPosition(cam) - gizmo.startTranslation);
        const f32 unitsPerPixel = std::max(0.001f,
            2.0f * distance * std::tan(glm::radians(cam.fov) * 0.5f) / std::max(1.0f, static_cast<f32>(height)));
        auto projectedAxis = [&](glm::vec3 axis) -> glm::vec2
        {
            const GizmoProjection a = viewportProject(cam, width, height, gizmo.startTranslation);
            const GizmoProjection b = viewportProject(cam, width, height, gizmo.startTranslation + axis);
            const glm::vec2 d = b.pixel - a.pixel;
            const f32 len = glm::length(d);
            if (len < 0.001f)
            {
                return glm::vec2{ 1.0f, 0.0f };
            }
            return d / len;
        };

        if (gizmo.mode == NativeGizmoMode::Translate)
        {
            glm::vec3 move{ 0.0f };
            if (gizmo.active == NativeGizmoHandle::XY || gizmo.active == NativeGizmoHandle::XZ ||
                gizmo.active == NativeGizmoHandle::YZ)
            {
                const bool useX = gizmo.active != NativeGizmoHandle::YZ;
                const bool useY = gizmo.active != NativeGizmoHandle::XZ;
                const bool useZ = gizmo.active != NativeGizmoHandle::XY;
                if (useX) { move += axes[0] * glm::dot(delta, projectedAxis(axes[0])) * unitsPerPixel; }
                if (useY) { move += axes[1] * glm::dot(delta, projectedAxis(axes[1])) * unitsPerPixel; }
                if (useZ) { move += axes[2] * glm::dot(delta, projectedAxis(axes[2])) * unitsPerPixel; }
            }
            else
            {
                const glm::vec3 axis = handleAxis(gizmo.active, axes);
                move = axis * glm::dot(delta, projectedAxis(axis)) * unitsPerPixel;
            }
            transform.translation = gizmo.startTranslation + move;
            return;
        }

        if (gizmo.mode == NativeGizmoMode::Rotate)
        {
            const f32 radians = (delta.x + delta.y) * 0.01f;
            if (gizmo.active == NativeGizmoHandle::X) { transform.rotation = gizmo.startRotation + glm::vec3{ radians, 0.0f, 0.0f }; }
            if (gizmo.active == NativeGizmoHandle::Y) { transform.rotation = gizmo.startRotation + glm::vec3{ 0.0f, radians, 0.0f }; }
            if (gizmo.active == NativeGizmoHandle::Z) { transform.rotation = gizmo.startRotation + glm::vec3{ 0.0f, 0.0f, radians }; }
            return;
        }

        const f32 scaleDelta = glm::dot(delta, projectedAxis(handleAxis(gizmo.active, axes))) * 0.01f;
        const f32 factor = std::max(0.05f, 1.0f + scaleDelta);
        if (gizmo.active == NativeGizmoHandle::Uniform)
        {
            const f32 uniform = std::max(0.05f, 1.0f + (delta.x - delta.y) * 0.01f);
            transform.scale = gizmo.startScale * uniform;
        }
        if (gizmo.active == NativeGizmoHandle::X) { transform.scale = gizmo.startScale * glm::vec3{ factor, 1.0f, 1.0f }; }
        if (gizmo.active == NativeGizmoHandle::Y) { transform.scale = gizmo.startScale * glm::vec3{ 1.0f, factor, 1.0f }; }
        if (gizmo.active == NativeGizmoHandle::Z) { transform.scale = gizmo.startScale * glm::vec3{ 1.0f, 1.0f, factor }; }
    }
}
