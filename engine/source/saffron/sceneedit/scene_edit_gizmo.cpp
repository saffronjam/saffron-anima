module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

module Saffron.SceneEdit;

import Saffron.Core;
import Saffron.Scene;

namespace se
{
    void syncNativeGizmo(SceneEditContext& ctx)
    {
        ctx.nativeGizmo.mode = ctx.gizmoOp == GizmoOp::Rotate  ? NativeGizmoMode::Rotate
                               : ctx.gizmoOp == GizmoOp::Scale ? NativeGizmoMode::Scale
                                                               : NativeGizmoMode::Translate;
        ctx.nativeGizmo.space = ctx.gizmoSpace == GizmoSpace::Local ? NativeGizmoSpace::Local : NativeGizmoSpace::World;
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
        if (ndc3.z < 0.0f || ndc3.z > 1.0f)
        {
            return {};
        }
        return GizmoProjection{ .pixel = glm::vec2{ (ndc3.x * 0.5f + 0.5f) * static_cast<f32>(width),
                                                    (1.0f - (ndc3.y * 0.5f + 0.5f)) * static_cast<f32>(height) },
                                .ndc = glm::vec2{ ndc3.x, ndc3.y },
                                .visible = true };
    }

    auto pixelToNdc(glm::vec2 p, u32 width, u32 height) -> glm::vec2
    {
        return glm::vec2{ p.x / static_cast<f32>(width) * 2.0f - 1.0f, p.y / static_cast<f32>(height) * 2.0f - 1.0f };
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

    auto pointInConvexQuad(glm::vec2 p, const std::array<GizmoProjection, 4>& quad) -> bool
    {
        for (const GizmoProjection& q : quad)
        {
            if (!q.visible)
            {
                return false;
            }
        }
        auto edge = [](glm::vec2 a, glm::vec2 b, glm::vec2 c) -> f32
        {
            const glm::vec2 ab = b - a;
            const glm::vec2 ac = c - a;
            return ab.x * ac.y - ab.y * ac.x;
        };
        bool hasNeg = false;
        bool hasPos = false;
        for (u32 i = 0; i < 4; i = i + 1)
        {
            const f32 e = edge(quad[i].pixel, quad[(i + 1) % 4].pixel, p);
            hasNeg = hasNeg || e < 0.0f;
            hasPos = hasPos || e > 0.0f;
        }
        return !(hasNeg && hasPos);
    }

    auto hitRotateRing(const CameraView& cam, u32 width, u32 height, glm::vec2 mouse, glm::vec3 origin,
                       const std::array<glm::vec3, 3>& axes, f32 radius) -> NativeGizmoHandle
    {
        constexpr u32 segments = 96;
        const std::array<NativeGizmoHandle, 3> handles{ NativeGizmoHandle::X, NativeGizmoHandle::Y,
                                                        NativeGizmoHandle::Z };
        NativeGizmoHandle hit = NativeGizmoHandle::None;
        f32 best = 9.0f;
        for (u32 axis = 0; axis < 3; axis = axis + 1)
        {
            const glm::vec3 n = axes[axis];
            glm::vec3 a = glm::normalize(glm::cross(n, glm::vec3{ 0.0f, 1.0f, 0.0f }));
            if (glm::length(a) < 0.001f)
            {
                a = glm::normalize(glm::cross(n, glm::vec3{ 1.0f, 0.0f, 0.0f }));
            }
            const glm::vec3 b = glm::normalize(glm::cross(n, a));
            GizmoProjection prev{};
            for (u32 i = 0; i <= segments; i = i + 1)
            {
                const f32 t = static_cast<f32>(i) / static_cast<f32>(segments) * glm::two_pi<f32>();
                const GizmoProjection cur =
                    viewportProject(cam, width, height, origin + (a * std::cos(t) + b * std::sin(t)) * radius);
                if (i > 0 && prev.visible && cur.visible)
                {
                    const f32 d = pointSegmentDistance(mouse, prev.pixel, cur.pixel);
                    if (d < best)
                    {
                        best = d;
                        hit = handles[axis];
                    }
                }
                prev = cur;
            }
        }
        return hit;
    }

    auto axisColor(NativeGizmoHandle handle, const NativeGizmoState& gizmo) -> glm::vec4
    {
        if (gizmo.active == handle || (gizmo.active == NativeGizmoHandle::None && gizmo.hovered == handle))
        {
            return glm::vec4{ 1.0f, 0.82f, 0.18f, 1.0f };
        }
        if (handle == NativeGizmoHandle::X)
        {
            return glm::vec4{ 0.93f, 0.18f, 0.20f, 1.0f };
        }
        if (handle == NativeGizmoHandle::Y)
        {
            return glm::vec4{ 0.20f, 0.82f, 0.25f, 1.0f };
        }
        if (handle == NativeGizmoHandle::Z)
        {
            return glm::vec4{ 0.22f, 0.42f, 0.98f, 1.0f };
        }
        return glm::vec4{ 0.93f, 0.93f, 0.95f, 0.75f };
    }

    auto gizmoAxes(const glm::quat& worldRotation, NativeGizmoSpace space) -> std::array<glm::vec3, 3>
    {
        if (space == NativeGizmoSpace::World)
        {
            return { glm::vec3{ 1.0f, 0.0f, 0.0f }, glm::vec3{ 0.0f, 1.0f, 0.0f }, glm::vec3{ 0.0f, 0.0f, 1.0f } };
        }
        return { worldRotation * glm::vec3{ 1.0f, 0.0f, 0.0f }, worldRotation * glm::vec3{ 0.0f, 1.0f, 0.0f },
                 worldRotation * glm::vec3{ 0.0f, 0.0f, 1.0f } };
    }

    auto handleAxis(NativeGizmoHandle handle, const std::array<glm::vec3, 3>& axes) -> glm::vec3
    {
        if (handle == NativeGizmoHandle::X)
        {
            return axes[0];
        }
        if (handle == NativeGizmoHandle::Y)
        {
            return axes[1];
        }
        if (handle == NativeGizmoHandle::Z)
        {
            return axes[2];
        }
        return glm::vec3{ 0.0f };
    }

    auto hitNativeGizmo(SceneEditContext& editor, const CameraView& cam, u32 width, u32 height, glm::vec2 mouse)
        -> NativeGizmoHandle
    {
        if (editor.selected.handle == entt::null || !hasComponent<TransformComponent>(editor.scene, editor.selected))
        {
            return NativeGizmoHandle::None;
        }
        const glm::vec3 position = worldTranslation(editor.scene, editor.selected);
        const GizmoProjection origin = viewportProject(cam, width, height, position);
        if (!origin.visible)
        {
            return NativeGizmoHandle::None;
        }
        const auto axes = gizmoAxes(worldRotation(editor.scene, editor.selected), editor.nativeGizmo.space);
        const f32 distance = glm::length(cameraPosition(cam) - position);
        const f32 axisLen = std::max(0.75f, distance * 0.22f);
        const std::array<NativeGizmoHandle, 3> handles{ NativeGizmoHandle::X, NativeGizmoHandle::Y,
                                                        NativeGizmoHandle::Z };

        if (editor.nativeGizmo.mode == NativeGizmoMode::Rotate)
        {
            return hitRotateRing(cam, width, height, mouse, position, axes, axisLen * 0.72f);
        }

        if (editor.nativeGizmo.mode == NativeGizmoMode::Scale && glm::length(mouse - origin.pixel) < 12.0f)
        {
            return NativeGizmoHandle::Uniform;
        }

        for (u32 i = 0; i < 3; i = i + 1)
        {
            const GizmoProjection end = viewportProject(cam, width, height, position + axes[i] * axisLen);
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
            constexpr f32 quadMin = 0.5f;
            constexpr f32 quadMax = 0.8f;
            const std::array<std::pair<NativeGizmoHandle, std::pair<u32, u32>>, 3> planes{
                std::pair{ NativeGizmoHandle::YZ, std::pair{ 1u, 2u } },
                std::pair{ NativeGizmoHandle::XZ, std::pair{ 0u, 2u } },
                std::pair{ NativeGizmoHandle::XY, std::pair{ 0u, 1u } }
            };
            for (const auto& [handle, axisPair] : planes)
            {
                const std::array<GizmoProjection, 4> corners{
                    viewportProject(cam, width, height,
                                    position + axes[axisPair.first] * axisLen * quadMin +
                                        axes[axisPair.second] * axisLen * quadMin),
                    viewportProject(cam, width, height,
                                    position + axes[axisPair.first] * axisLen * quadMin +
                                        axes[axisPair.second] * axisLen * quadMax),
                    viewportProject(cam, width, height,
                                    position + axes[axisPair.first] * axisLen * quadMax +
                                        axes[axisPair.second] * axisLen * quadMax),
                    viewportProject(cam, width, height,
                                    position + axes[axisPair.first] * axisLen * quadMax +
                                        axes[axisPair.second] * axisLen * quadMin)
                };
                if (pointInConvexQuad(mouse, corners))
                {
                    return handle;
                }
            }
        }
        return NativeGizmoHandle::None;
    }

    namespace
    {
        auto parentOf(Scene& scene, Entity entity) -> entt::entity
        {
            if (!hasComponent<RelationshipComponent>(scene, entity))
            {
                return entt::null;
            }
            return getComponent<RelationshipComponent>(scene, entity).parentHandle;
        }

        auto rotationOf(const glm::mat4& m) -> glm::quat
        {
            glm::vec3 scale{ glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])) };
            scale = glm::max(scale, glm::vec3(1e-8f));
            const glm::mat3 rotation{ glm::vec3(m[0]) / scale.x, glm::vec3(m[1]) / scale.y, glm::vec3(m[2]) / scale.z };
            return glm::quat_cast(rotation);
        }
    }

    void snapshotNativeGizmoStart(SceneEditContext& editor, Entity target)
    {
        NativeGizmoState& gizmo = editor.nativeGizmo;
        const TransformComponent& transform = getComponent<TransformComponent>(editor.scene, target);
        gizmo.startScale = transform.scale;  // scale never rebases (TRS-only model)
        gizmo.startParentWorld = glm::mat4{ 1.0f };
        const entt::entity parent = parentOf(editor.scene, target);
        if (parent == entt::null)
        {
            // Root: world == local. Keeping the raw Euler preserves rotate-drag continuity
            // for angles a matrix extraction would wrap.
            gizmo.startTranslation = transform.translation;
            gizmo.startRotation = transform.rotation;
            return;
        }
        gizmo.startParentWorld = composeWorldMatrix(editor.scene, Entity{ parent });
        gizmo.startTranslation = worldTranslation(editor.scene, target);
        glm::vec3 euler;
        glm::extractEulerAngleZYX(glm::mat4_cast(worldRotation(editor.scene, target)), euler.z, euler.y, euler.x);
        gizmo.startRotation = euler;
    }

    void applyNativeGizmoDrag(SceneEditContext& editor, const CameraView& cam, u32 width, u32 height, glm::vec2 mouse)
    {
        NativeGizmoState& gizmo = editor.nativeGizmo;
        if (!gizmo.dragging || gizmo.target.handle == entt::null ||
            !hasComponent<TransformComponent>(editor.scene, gizmo.target))
        {
            return;
        }
        // Past the guard the drag will write the transform; bump the scene stamp so the
        // control-plane poll re-inspects and the editor's Inspector tracks the drag live.
        editor.sceneVersion += 1;
        TransformComponent& transform = getComponent<TransformComponent>(editor.scene, gizmo.target);
        const auto axes = gizmoAxes(worldRotation(editor.scene, gizmo.target), gizmo.space);
        const glm::vec2 delta = mouse - gizmo.startMouse;
        const f32 distance = glm::length(cameraPosition(cam) - gizmo.startTranslation);
        const f32 unitsPerPixel = std::max(0.001f, 2.0f * distance * std::tan(glm::radians(cam.fov) * 0.5f) /
                                                       std::max(1.0f, static_cast<f32>(height)));
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
                if (useX)
                {
                    move += axes[0] * glm::dot(delta, projectedAxis(axes[0])) * unitsPerPixel;
                }
                if (useY)
                {
                    move += axes[1] * glm::dot(delta, projectedAxis(axes[1])) * unitsPerPixel;
                }
                if (useZ)
                {
                    move += axes[2] * glm::dot(delta, projectedAxis(axes[2])) * unitsPerPixel;
                }
            }
            else
            {
                const glm::vec3 axis = handleAxis(gizmo.active, axes);
                move = axis * glm::dot(delta, projectedAxis(axis)) * unitsPerPixel;
            }
            // The drag math runs in world space; rebase the result into the parent frame
            // before writing the local transform (identity parent for a root).
            transform.translation =
                glm::vec3(glm::inverse(gizmo.startParentWorld) * glm::vec4(gizmo.startTranslation + move, 1.0f));
            return;
        }

        if (gizmo.mode == NativeGizmoMode::Rotate)
        {
            const f32 radians = (delta.x + delta.y) * 0.01f;
            glm::vec3 worldEuler = gizmo.startRotation;
            if (gizmo.active == NativeGizmoHandle::X)
            {
                worldEuler += glm::vec3{ radians, 0.0f, 0.0f };
            }
            if (gizmo.active == NativeGizmoHandle::Y)
            {
                worldEuler += glm::vec3{ 0.0f, radians, 0.0f };
            }
            if (gizmo.active == NativeGizmoHandle::Z)
            {
                worldEuler += glm::vec3{ 0.0f, 0.0f, radians };
            }
            const entt::entity parent = parentOf(editor.scene, gizmo.target);
            if (parent == entt::null)
            {
                // Root: the world Euler IS the local Euler; writing it raw keeps the stored
                // angles continuous instead of wrapping through a matrix extraction.
                transform.rotation = worldEuler;
                return;
            }
            // Peel the frozen parent rotation off the world result, then extract a stable
            // Euler (extractEulerAngleZYX matches the engine's Rz*Ry*Rx convention).
            const glm::quat localRot = glm::inverse(rotationOf(gizmo.startParentWorld)) * glm::quat(worldEuler);
            glm::vec3 euler;
            glm::extractEulerAngleZYX(glm::mat4_cast(localRot), euler.z, euler.y, euler.x);
            transform.rotation = euler;
            return;
        }

        const f32 scaleDelta = glm::dot(delta, projectedAxis(handleAxis(gizmo.active, axes))) * 0.01f;
        const f32 factor = std::max(0.05f, 1.0f + scaleDelta);
        if (gizmo.active == NativeGizmoHandle::Uniform)
        {
            const f32 uniform = std::max(0.05f, 1.0f + (delta.x - delta.y) * 0.01f);
            transform.scale = gizmo.startScale * uniform;
        }
        if (gizmo.active == NativeGizmoHandle::X)
        {
            transform.scale = gizmo.startScale * glm::vec3{ factor, 1.0f, 1.0f };
        }
        if (gizmo.active == NativeGizmoHandle::Y)
        {
            transform.scale = gizmo.startScale * glm::vec3{ 1.0f, factor, 1.0f };
        }
        if (gizmo.active == NativeGizmoHandle::Z)
        {
            transform.scale = gizmo.startScale * glm::vec3{ 1.0f, 1.0f, factor };
        }
    }
}
