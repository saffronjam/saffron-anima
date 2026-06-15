module;

// SDL3 + glm are header-heavy C++ headers, so this TU uses classic includes (no
// `import std`) — consistent with the engine's rendering/scene modules.
#include <entt/entt.hpp>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

export module Saffron.Host;

import Saffron.Core;
import Saffron.Signal;
import Saffron.App;
import Saffron.Window;
import Saffron.Rendering;
import Saffron.SceneEdit;
import Saffron.Control;
import Saffron.Scene;
import Saffron.Geometry;
import Saffron.Animation;
import Saffron.Physics;
import Saffron.Script;
import Saffron.Assets;

namespace se
{
    constexpr se::i32 KeyEscape = 27;  // SDLK_ESCAPE

    // State shared across the app lifecycle closures. The SceneEditContext is owned
    // by the engine (heap) so its heavy entt/json destructor stays out of this TU.
    struct HostState
    {
        se::SceneEditContext* editor = nullptr;
        se::ControlContext* control = nullptr;
        se::AssetServer assets;
        se::AnimationRuntime animation;           // per-session clip cache for the evaluator
        se::ScriptHost script;                    // the Lua runtime; the Host is its only owner
        se::SubscriptionId scriptSubscription;    // the onPlayStateChanged lifecycle hook
        se::SubscriptionId physicsSubscription;   // the onPlayStateChanged physics lifecycle hook
        std::optional<se::PhysicsWorld> physics;  // a Jolt world exists exactly while play is active
        bool physicsInit = false;                 // initPhysics() has installed the Jolt globals
        se::u64 contactCursor = 0;                // seq high-water for per-tick contact -> script dispatch
        bool scriptVmActive = false;              // a VM exists (Playing/Paused); stop destroys it
        bool scriptErrorPending = false;          // set inside simTick; drives the deferred pause
        bool shmPublish = false;                  // frames publish to shared memory; the editor owns the render size
        bool previewActive = false;               // tracks asset-preview transitions to prune the anim runtime
    };

    enum class BillboardKind
    {
        None,
        PointLight,
        SpotLight,
        Camera,
    };

    // The overlay-gizmo + billboard geometry builders. These touch Rendering
    // (OverlayVertex / submitOverlay / Renderer), so they stay in this TU; the
    // pure-math hit-test/drag live in Saffron.SceneEdit.

    void addTriangle(std::vector<se::OverlayVertex>& vertices, glm::vec2 a, glm::vec2 b, glm::vec2 c, glm::vec4 color)
    {
        vertices.push_back(se::OverlayVertex{ a, color });
        vertices.push_back(se::OverlayVertex{ b, color });
        vertices.push_back(se::OverlayVertex{ c, color });
    }

    void addLine(std::vector<se::OverlayVertex>& vertices, glm::vec2 aPx, glm::vec2 bPx, se::f32 thickness,
                 glm::vec4 color, se::u32 width, se::u32 height, se::f32 aDepth = 0.0f, se::f32 bDepth = 0.0f)
    {
        const glm::vec2 delta = bPx - aPx;
        const se::f32 len = glm::length(delta);
        if (len < 0.001f)
        {
            return;
        }
        // The quad is widened 1px per side for the shader's analytic feather: edge.x
        // interpolates the signed cross-edge coordinate (±1 at the nominal width, ±rim
        // at the expanded rim where coverage reaches zero), edge.z carries the
        // half-thickness so the falloff stays ~1px at any line width.
        const se::f32 half = thickness * 0.5f;
        const se::f32 ext = half + 1.0f;
        const glm::vec2 n = glm::vec2{ -delta.y, delta.x } / len * ext;
        const glm::vec4 edgePos{ ext / half, 0.0f, half, 0.0f };
        const glm::vec4 edgeNeg{ -ext / half, 0.0f, half, 0.0f };
        const glm::vec2 a0 = se::pixelToNdc(aPx + n, width, height);
        const glm::vec2 a1 = se::pixelToNdc(aPx - n, width, height);
        const glm::vec2 b0 = se::pixelToNdc(bPx + n, width, height);
        const glm::vec2 b1 = se::pixelToNdc(bPx - n, width, height);
        vertices.push_back(se::OverlayVertex{ a0, color, edgePos, aDepth });
        vertices.push_back(se::OverlayVertex{ b0, color, edgePos, bDepth });
        vertices.push_back(se::OverlayVertex{ b1, color, edgeNeg, bDepth });
        vertices.push_back(se::OverlayVertex{ a0, color, edgePos, aDepth });
        vertices.push_back(se::OverlayVertex{ b1, color, edgeNeg, bDepth });
        vertices.push_back(se::OverlayVertex{ a1, color, edgeNeg, aDepth });
    }

    // A filled quad from 4 pixel-space corners (a convex loop), feathered analytically in
    // both directions: each corner is pushed 1px outward along the quad's edge directions,
    // and the vertices carry signed coords + half-extents for the shader's coverage alpha.
    void addQuad(std::vector<se::OverlayVertex>& vertices, const std::array<glm::vec2, 4>& cornersPx, glm::vec4 color,
                 se::u32 width, se::u32 height)
    {
        const glm::vec2 u = (cornersPx[3] - cornersPx[0] + cornersPx[2] - cornersPx[1]) * 0.5f;
        const glm::vec2 v = (cornersPx[1] - cornersPx[0] + cornersPx[2] - cornersPx[3]) * 0.5f;
        const se::f32 hu = glm::length(u) * 0.5f;
        const se::f32 hv = glm::length(v) * 0.5f;
        if (hu < 0.5f || hv < 0.5f)
        {
            return;
        }
        const glm::vec2 du = u / (hu * 2.0f);
        const glm::vec2 dv = v / (hv * 2.0f);
        const se::f32 eu = (hu + 1.0f) / hu;
        const se::f32 ev = (hv + 1.0f) / hv;
        // Corner order mirrors gizmoPlaneCorners: (min,min), (min,max), (max,max), (max,min).
        const std::array<se::OverlayVertex, 4> quad{
            se::OverlayVertex{ se::pixelToNdc(cornersPx[0] - du - dv, width, height), color,
                               glm::vec4{ -eu, -ev, hu, hv } },
            se::OverlayVertex{ se::pixelToNdc(cornersPx[1] - du + dv, width, height), color,
                               glm::vec4{ -eu, ev, hu, hv } },
            se::OverlayVertex{ se::pixelToNdc(cornersPx[2] + du + dv, width, height), color,
                               glm::vec4{ eu, ev, hu, hv } },
            se::OverlayVertex{ se::pixelToNdc(cornersPx[3] + du - dv, width, height), color,
                               glm::vec4{ eu, -ev, hu, hv } }
        };
        vertices.push_back(quad[0]);
        vertices.push_back(quad[1]);
        vertices.push_back(quad[2]);
        vertices.push_back(quad[0]);
        vertices.push_back(quad[2]);
        vertices.push_back(quad[3]);
    }

    void addBox(std::vector<se::OverlayVertex>& vertices, glm::vec2 centerPx, se::f32 size, glm::vec4 color,
                se::u32 width, se::u32 height)
    {
        const se::f32 h = size * 0.5f;
        const glm::vec2 a = se::pixelToNdc(centerPx + glm::vec2{ -h, -h }, width, height);
        const glm::vec2 b = se::pixelToNdc(centerPx + glm::vec2{ h, -h }, width, height);
        const glm::vec2 c = se::pixelToNdc(centerPx + glm::vec2{ h, h }, width, height);
        const glm::vec2 d = se::pixelToNdc(centerPx + glm::vec2{ -h, h }, width, height);
        addTriangle(vertices, a, b, c, color);
        addTriangle(vertices, a, c, d, color);
    }

    void addRectOutline(std::vector<se::OverlayVertex>& vertices, glm::vec2 centerPx, glm::vec2 sizePx, glm::vec4 color,
                        se::u32 width, se::u32 height)
    {
        const glm::vec2 h = sizePx * 0.5f;
        const glm::vec2 tl = centerPx + glm::vec2{ -h.x, -h.y };
        const glm::vec2 tr = centerPx + glm::vec2{ h.x, -h.y };
        const glm::vec2 br = centerPx + glm::vec2{ h.x, h.y };
        const glm::vec2 bl = centerPx + glm::vec2{ -h.x, h.y };
        addLine(vertices, tl, tr, 2.0f, color, width, height);
        addLine(vertices, tr, br, 2.0f, color, width, height);
        addLine(vertices, br, bl, 2.0f, color, width, height);
        addLine(vertices, bl, tl, 2.0f, color, width, height);
    }

    void addCircleFill(std::vector<se::OverlayVertex>& vertices, glm::vec2 centerPx, se::f32 radius, glm::vec4 color,
                       se::u32 width, se::u32 height)
    {
        constexpr se::u32 segments = 24;
        const glm::vec2 center = se::pixelToNdc(centerPx, width, height);
        for (se::u32 i = 0; i < segments; i = i + 1)
        {
            const se::f32 a0 = static_cast<se::f32>(i) / static_cast<se::f32>(segments) * glm::two_pi<se::f32>();
            const se::f32 a1 = static_cast<se::f32>(i + 1) / static_cast<se::f32>(segments) * glm::two_pi<se::f32>();
            const glm::vec2 p0 =
                se::pixelToNdc(centerPx + glm::vec2{ std::cos(a0), std::sin(a0) } * radius, width, height);
            const glm::vec2 p1 =
                se::pixelToNdc(centerPx + glm::vec2{ std::cos(a1), std::sin(a1) } * radius, width, height);
            addTriangle(vertices, center, p0, p1, color);
        }
    }

    void addCircleOutline(std::vector<se::OverlayVertex>& vertices, glm::vec2 centerPx, se::f32 radius, glm::vec4 color,
                          se::u32 width, se::u32 height)
    {
        constexpr se::u32 segments = 32;
        glm::vec2 prev = centerPx + glm::vec2{ radius, 0.0f };
        for (se::u32 i = 1; i <= segments; i = i + 1)
        {
            const se::f32 a = static_cast<se::f32>(i) / static_cast<se::f32>(segments) * glm::two_pi<se::f32>();
            const glm::vec2 cur = centerPx + glm::vec2{ std::cos(a), std::sin(a) } * radius;
            addLine(vertices, prev, cur, 2.0f, color, width, height);
            prev = cur;
        }
    }

    void addBulbIcon(std::vector<se::OverlayVertex>& vertices, glm::vec2 centerPx, glm::vec4 color, se::u32 width,
                     se::u32 height)
    {
        addCircleFill(vertices, centerPx + glm::vec2{ 0.0f, -3.0f }, 7.5f, color, width, height);
        addLine(vertices, centerPx + glm::vec2{ -4.5f, 5.0f }, centerPx + glm::vec2{ 4.5f, 5.0f }, 3.0f, color, width,
                height);
        addLine(vertices, centerPx + glm::vec2{ -3.5f, 9.0f }, centerPx + glm::vec2{ 3.5f, 9.0f }, 3.0f, color, width,
                height);
    }

    void addCameraIcon(std::vector<se::OverlayVertex>& vertices, glm::vec2 centerPx, glm::vec4 color, se::u32 width,
                       se::u32 height)
    {
        addRectOutline(vertices, centerPx + glm::vec2{ -2.0f, 1.0f }, glm::vec2{ 20.0f, 14.0f }, color, width, height);
        addCircleOutline(vertices, centerPx + glm::vec2{ -2.0f, 1.0f }, 4.0f, color, width, height);
        const glm::vec2 a = centerPx + glm::vec2{ 8.0f, -4.0f };
        const glm::vec2 b = centerPx + glm::vec2{ 14.0f, -8.0f };
        const glm::vec2 c = centerPx + glm::vec2{ 14.0f, 6.0f };
        const glm::vec2 d = centerPx + glm::vec2{ 8.0f, 2.0f };
        addLine(vertices, a, b, 2.0f, color, width, height);
        addLine(vertices, b, c, 2.0f, color, width, height);
        addLine(vertices, c, d, 2.0f, color, width, height);
    }

    auto billboardKind(se::Scene& scene, se::Entity entity) -> BillboardKind
    {
        if (se::hasComponent<se::MeshComponent>(scene, entity))
        {
            return BillboardKind::None;
        }
        if (se::hasComponent<se::PointLightComponent>(scene, entity))
        {
            return BillboardKind::PointLight;
        }
        if (se::hasComponent<se::SpotLightComponent>(scene, entity))
        {
            return BillboardKind::SpotLight;
        }
        if (se::hasComponent<se::CameraComponent>(scene, entity))
        {
            return BillboardKind::Camera;
        }
        return BillboardKind::None;
    }

    // Builds the active-mode gizmo geometry for the selected entity into `vertices`.
    void buildNativeGizmo(se::SceneEditContext& editor, const se::CameraView& cam, se::u32 width, se::u32 height,
                          std::vector<se::OverlayVertex>& vertices)
    {
        if (editor.selected.handle == entt::null ||
            !se::hasComponent<se::TransformComponent>(editor.scene, editor.selected))
        {
            return;
        }
        const glm::vec3 position = se::worldTranslation(editor.scene, editor.selected);
        const auto axes = se::gizmoAxes(se::worldRotation(editor.scene, editor.selected), editor.nativeGizmo.space);
        const se::GizmoProjection origin = se::viewportProject(cam, width, height, position);
        if (!origin.visible)
        {
            return;
        }
        const se::f32 distance = glm::length(se::cameraPosition(cam) - position);
        const se::f32 axisLen = std::max(0.75f, distance * 0.22f);
        const std::array<se::NativeGizmoHandle, 3> handles{ se::NativeGizmoHandle::X, se::NativeGizmoHandle::Y,
                                                            se::NativeGizmoHandle::Z };
        // Rotate mode shows only the rings; the straight axis lines belong to translate/scale.
        if (editor.nativeGizmo.mode != se::NativeGizmoMode::Rotate)
        {
            for (se::u32 i = 0; i < 3; i = i + 1)
            {
                const se::GizmoProjection end = se::viewportProject(cam, width, height, position + axes[i] * axisLen);
                if (!end.visible)
                {
                    continue;
                }
                addLine(vertices, origin.pixel, end.pixel, 5.0f, se::axisColor(handles[i], editor.nativeGizmo), width,
                        height);
                se::f32 boxSize = 8.0f;
                if (editor.nativeGizmo.mode == se::NativeGizmoMode::Scale)
                {
                    boxSize = 12.0f;
                }
                addBox(vertices, end.pixel, boxSize, se::axisColor(handles[i], editor.nativeGizmo), width, height);
            }
        }
        if (editor.nativeGizmo.mode == se::NativeGizmoMode::Translate)
        {
            // The drawn quads are the exact hit-test geometry (gizmoPlaneCorners), so the
            // plane handles always sit under the cursor that activates them.
            const std::array<std::pair<se::NativeGizmoHandle, std::pair<se::u32, se::u32>>, 3> planes{
                std::pair{ se::NativeGizmoHandle::XY, std::pair{ 0u, 1u } },
                std::pair{ se::NativeGizmoHandle::YZ, std::pair{ 1u, 2u } },
                std::pair{ se::NativeGizmoHandle::XZ, std::pair{ 0u, 2u } }
            };
            for (const auto& [handle, axisPair] : planes)
            {
                const std::array<se::GizmoProjection, 4> corners =
                    se::gizmoPlaneCorners(cam, width, height, position, axes, axisLen, axisPair.first, axisPair.second);
                if (!corners[0].visible || !corners[1].visible || !corners[2].visible || !corners[3].visible)
                {
                    continue;
                }
                addQuad(vertices, { corners[0].pixel, corners[1].pixel, corners[2].pixel, corners[3].pixel },
                        se::axisColor(handle, editor.nativeGizmo), width, height);
            }
        }
        else if (editor.nativeGizmo.mode == se::NativeGizmoMode::Rotate)
        {
            constexpr se::u32 segments = 96;
            const se::f32 radius = axisLen * 0.72f;
            for (se::u32 axis = 0; axis < 3; axis = axis + 1)
            {
                const auto [a, b] = se::ringBasis(axes[axis]);
                se::GizmoProjection prev{};
                for (se::u32 i = 0; i <= segments; i = i + 1)
                {
                    const se::f32 t = static_cast<se::f32>(i) / static_cast<se::f32>(segments) * glm::two_pi<se::f32>();
                    const se::GizmoProjection cur = se::viewportProject(
                        cam, width, height, position + (a * std::cos(t) + b * std::sin(t)) * radius);
                    if (i > 0 && prev.visible && cur.visible)
                    {
                        addLine(vertices, prev.pixel, cur.pixel, 3.0f, se::axisColor(handles[axis], editor.nativeGizmo),
                                width, height);
                    }
                    prev = cur;
                }
            }
        }
        else
        {
            addBox(vertices, origin.pixel, 13.0f, se::axisColor(se::NativeGizmoHandle::Uniform, editor.nativeGizmo),
                   width, height);
        }
    }

    // Draws a line skeleton over the selected rig: a bone segment to each joint's parent,
    // a screen-constant joint dot, and (when enabled) three short RGB axis lines per joint.
    // Always on top (the overlay PSO has no depth test). Renders in Edit and Play, so a
    // playing clip shows its bones move; scoped to the selected entity to bound vertex count.
    void buildSkeletonOverlay(se::SceneEditContext& editor, const se::CameraView& cam, se::u32 width, se::u32 height,
                              std::vector<se::OverlayVertex>& vertices)
    {
        if (!editor.skeletonOverlay.show || width == 0 || height == 0)
        {
            return;
        }
        se::Scene& scene = se::activeScene(editor);
        // The model the overlay draws bones for: the previewed model's root while previewing (so
        // highlighting a bone via the dedicated channel never blanks the overlay, and a bone has no
        // SkinnedMesh of its own), else the selected entity in the normal scene-edit view. A static
        // model's root has no SkinnedMeshComponent, so the overlay self-gates to nothing just below.
        se::Entity target = editor.selected;
        if (editor.previewScene)
        {
            target = editor.previewRootEntity;
        }
        if (target.handle == entt::null)
        {
            return;
        }
        if (!se::valid(scene, target) || !se::hasComponent<se::SkinnedMeshComponent>(scene, target))
        {
            return;
        }
        const se::SkinnedMeshComponent& skin = se::getComponent<se::SkinnedMeshComponent>(scene, target);
        // Resolve the highlighted joint (a get-asset-model node index) to its spawned entity uuid; only
        // set while previewing, drawn in a distinct tint.
        se::Uuid highlightUuid{ 0 };
        if (editor.previewScene && editor.skeletonOverlay.highlightJoint >= 0 &&
            static_cast<std::size_t>(editor.skeletonOverlay.highlightJoint) < editor.previewBoneByNode.size())
        {
            highlightUuid = editor.previewBoneByNode[static_cast<std::size_t>(editor.skeletonOverlay.highlightJoint)];
        }
        constexpr glm::vec4 BoneColor{ 0.55f, 0.78f, 1.0f, 0.95f };
        constexpr glm::vec4 JointColor{ 1.0f, 0.78f, 0.18f, 1.0f };
        constexpr glm::vec4 HighlightColor{ 0.30f, 1.0f, 0.45f, 1.0f };
        constexpr se::f32 AxisLen = 0.08f;  // per-joint axis length in world units
        const std::array<glm::vec4, 3> axisColors{ glm::vec4{ 1.0f, 0.32f, 0.32f, 0.95f },
                                                   glm::vec4{ 0.40f, 0.90f, 0.40f, 0.95f },
                                                   glm::vec4{ 0.42f, 0.62f, 1.0f, 0.95f } };

        for (const entt::entity handle : skin.boneHandles)
        {
            if (handle == entt::null)
            {
                continue;
            }
            const se::Entity bone{ handle };
            const glm::vec3 worldPos = se::worldTranslation(scene, bone);
            const se::GizmoProjection joint = se::viewportProject(cam, width, height, worldPos);
            if (!joint.visible)
            {
                continue;
            }
            // Bone segment to the parent, only when the parent is itself a joint.
            const entt::entity parentHandle = se::getComponent<se::RelationshipComponent>(scene, bone).parentHandle;
            if (parentHandle != entt::null && se::hasComponent<se::BoneComponent>(scene, se::Entity{ parentHandle }))
            {
                const se::GizmoProjection parent =
                    se::viewportProject(cam, width, height, se::worldTranslation(scene, se::Entity{ parentHandle }));
                if (parent.visible)
                {
                    addLine(vertices, parent.pixel, joint.pixel, 2.0f, BoneColor, width, height);
                }
            }
            // Joint dot: a constant pixel radius (joint.pixel is already the projected screen point and
            // addCircleFill takes pixels), so the dot stays the same on-screen size at any zoom. The
            // highlighted joint draws larger in a distinct tint (the skeleton-tree selection channel).
            const bool highlighted = highlightUuid.value != 0 &&
                                     se::getComponent<se::IdComponent>(scene, bone).id.value == highlightUuid.value;
            const se::f32 baseRadius = std::max(2.5f, editor.skeletonOverlay.jointSize);
            se::f32 radius = baseRadius;
            glm::vec4 jointColor = JointColor;
            if (highlighted)
            {
                radius = baseRadius * 1.8f;
                jointColor = HighlightColor;
            }
            addCircleFill(vertices, joint.pixel, radius, jointColor, width, height);
            // Optional per-joint RGB axes from the bone's world-rotation basis.
            if (editor.skeletonOverlay.axes)
            {
                const glm::quat rotation = se::worldRotation(scene, bone);
                const std::array<glm::vec3, 3> basis{ rotation * glm::vec3{ 1.0f, 0.0f, 0.0f },
                                                      rotation * glm::vec3{ 0.0f, 1.0f, 0.0f },
                                                      rotation * glm::vec3{ 0.0f, 0.0f, 1.0f } };
                for (se::u32 axis = 0; axis < 3; axis = axis + 1)
                {
                    const se::GizmoProjection tip =
                        se::viewportProject(cam, width, height, worldPos + basis[axis] * AxisLen);
                    if (tip.visible)
                    {
                        addLine(vertices, joint.pixel, tip.pixel, 1.5f, axisColors[axis], width, height);
                    }
                }
            }
        }
    }

    // Colored screen-space glyphs for meshless light/camera entities.
    void buildSceneEditBillboards(se::SceneEditContext& editor, const se::CameraView& cam, se::u32 width,
                                  se::u32 height, std::vector<se::OverlayVertex>& vertices)
    {
        if (width == 0 || height == 0)
        {
            return;
        }
        const glm::vec4 selectedColor{ 1.0f, 0.78f, 0.18f, 1.0f };
        se::Scene& scene = se::activeScene(editor);

        se::forEach<se::TransformComponent>(
            scene,
            [&](se::Entity e, se::TransformComponent&)
            {
                const BillboardKind kind = billboardKind(scene, e);
                if (kind == BillboardKind::None)
                {
                    return;
                }
                const glm::vec3 position = se::worldTranslation(scene, e);
                const se::GizmoProjection p = se::viewportProject(cam, width, height, position);
                if (!p.visible)
                {
                    return;
                }
                const bool sel = editor.selected.handle == e.handle;
                if (kind == BillboardKind::PointLight)
                {
                    glm::vec4 bulbColor{ 1.0f, 0.84f, 0.34f, 0.95f };
                    if (sel)
                    {
                        bulbColor = selectedColor;
                    }
                    addBulbIcon(vertices, p.pixel, bulbColor, width, height);
                    return;
                }
                if (kind == BillboardKind::SpotLight)
                {
                    glm::vec4 color{ 0.45f, 0.85f, 1.0f, 0.9f };
                    if (sel)
                    {
                        color = selectedColor;
                    }
                    addBulbIcon(vertices, p.pixel, color, width, height);
                    const glm::vec3 forward = se::worldRotation(scene, e) * glm::vec3{ 0.0f, 0.0f, -1.0f };
                    const se::GizmoProjection tip = se::viewportProject(cam, width, height, position + forward * 0.6f);
                    if (tip.visible)
                    {
                        addLine(vertices, p.pixel, tip.pixel, 3.0f, color, width, height);
                    }
                    return;
                }
                if (kind == BillboardKind::Camera)
                {
                    if (se::getComponent<se::CameraComponent>(scene, e).showModel)
                    {
                        return;
                    }
                    glm::vec4 cameraColor{ 0.85f, 0.87f, 0.92f, 0.95f };
                    if (sel)
                    {
                        cameraColor = selectedColor;
                    }
                    addCameraIcon(vertices, p.pixel, cameraColor, width, height);
                }
            });
    }

    auto clipOverlayLine(glm::vec4& a, glm::vec4& b) -> bool
    {
        auto clipPlane = [&](auto distance) -> bool
        {
            const se::f32 da = distance(a);
            const se::f32 db = distance(b);
            if (da >= 0.0f && db >= 0.0f)
            {
                return true;
            }
            if (da < 0.0f && db < 0.0f)
            {
                return false;
            }
            const se::f32 t = da / (da - db);
            const glm::vec4 p = a + (b - a) * t;
            if (da < 0.0f)
            {
                a = p;
            }
            else
            {
                b = p;
            }
            return true;
        };
        return clipPlane([](glm::vec4 p) { return p.x + p.w; }) && clipPlane([](glm::vec4 p) { return p.w - p.x; }) &&
               clipPlane([](glm::vec4 p) { return p.y + p.w; }) && clipPlane([](glm::vec4 p) { return p.w - p.y; }) &&
               clipPlane([](glm::vec4 p) { return p.z; }) && clipPlane([](glm::vec4 p) { return p.w - p.z; });
    }

    auto clipToPixel(glm::vec4 clip, se::u32 width, se::u32 height) -> glm::vec2
    {
        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        return glm::vec2{ (ndc.x * 0.5f + 0.5f) * static_cast<se::f32>(width),
                          (1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<se::f32>(height) };
    }

    void addClippedOverlayLine(std::vector<se::OverlayVertex>& vertices, const glm::mat4& viewProjection,
                               glm::vec3 aWorld, glm::vec3 bWorld, se::f32 thickness, glm::vec4 color, se::u32 width,
                               se::u32 height)
    {
        glm::vec4 aClip = viewProjection * glm::vec4(aWorld, 1.0f);
        glm::vec4 bClip = viewProjection * glm::vec4(bWorld, 1.0f);
        if (std::abs(aClip.w) < 0.0001f || std::abs(bClip.w) < 0.0001f || !clipOverlayLine(aClip, bClip))
        {
            return;
        }
        // After clipping, clip.z/clip.w is the Vulkan [0,1] NDC depth (GLM_FORCE_DEPTH_ZERO_TO_ONE);
        // the rasterizer interpolates it screen-linearly across the quad, matching the depth buffer.
        addLine(vertices, clipToPixel(aClip, width, height), clipToPixel(bClip, width, height), thickness, color, width,
                height, aClip.z / aClip.w, bClip.z / bClip.w);
    }

    void buildSceneEditCameraFrustums(se::SceneEditContext& editor, const se::CameraView& cam, se::u32 width,
                                      se::u32 height, std::vector<se::OverlayVertex>& vertices)
    {
        if (width == 0 || height == 0)
        {
            return;
        }
        constexpr glm::vec4 FrustumColor{ 0.78f, 0.29f, 0.02f, 0.95f };
        constexpr std::array<std::pair<se::u32, se::u32>, 12> Edges{
            std::pair{ 0u, 1u }, std::pair{ 1u, 2u }, std::pair{ 2u, 3u }, std::pair{ 3u, 0u },
            std::pair{ 4u, 5u }, std::pair{ 5u, 6u }, std::pair{ 6u, 7u }, std::pair{ 7u, 4u },
            std::pair{ 0u, 4u }, std::pair{ 1u, 5u }, std::pair{ 2u, 6u }, std::pair{ 3u, 7u }
        };
        se::Scene& scene = se::activeScene(editor);
        const se::f32 aspect = static_cast<se::f32>(width) / static_cast<se::f32>(height);
        const glm::mat4 viewProjection = se::cameraProjection(cam, aspect) * cam.view;

        se::forEach<se::TransformComponent, se::CameraComponent>(
            scene,
            [&](se::Entity entity, se::TransformComponent&, se::CameraComponent& camera)
            {
                if (!camera.showFrustum)
                {
                    return;
                }
                const se::f32 nearPlane = std::max(camera.nearPlane, 0.001f);
                const se::f32 maxDistance = std::max(camera.frustumMaxDistance, nearPlane + 0.001f);
                const se::f32 farPlane = std::min(std::max(camera.farPlane, nearPlane + 0.001f), maxDistance);
                const se::f32 halfFov = glm::radians(std::clamp(camera.fov, 1.0f, 179.0f)) * 0.5f;
                const se::f32 nearY = std::tan(halfFov) * nearPlane;
                const se::f32 nearX = nearY * aspect;
                const se::f32 farY = std::tan(halfFov) * farPlane;
                const se::f32 farX = farY * aspect;
                const glm::mat4 model = se::worldMatrix(scene, entity);
                const std::array<glm::vec3, 8> local{
                    glm::vec3{ -nearX, -nearY, -nearPlane }, glm::vec3{ -nearX, nearY, -nearPlane },
                    glm::vec3{ nearX, nearY, -nearPlane },   glm::vec3{ nearX, -nearY, -nearPlane },
                    glm::vec3{ -farX, -farY, -farPlane },    glm::vec3{ -farX, farY, -farPlane },
                    glm::vec3{ farX, farY, -farPlane },      glm::vec3{ farX, -farY, -farPlane }
                };
                std::array<glm::vec3, 8> world{};
                for (se::u32 i = 0; i < local.size(); i = i + 1)
                {
                    world[i] = glm::vec3(model * glm::vec4(local[i], 1.0f));
                }
                for (const auto& edge : Edges)
                {
                    addClippedOverlayLine(vertices, viewProjection, world[edge.first], world[edge.second], 2.0f,
                                          FrustumColor, width, height);
                }
            });
    }

    // A world-space AABB as 12 edges; lines go into the depth-tested bucket so scene
    // geometry occludes the box.
    void addWorldAabb(std::vector<se::OverlayVertex>& vertices, const glm::mat4& viewProjection, glm::vec3 lo,
                      glm::vec3 hi, glm::vec4 color, se::u32 width, se::u32 height)
    {
        const std::array<glm::vec3, 8> corners{ glm::vec3{ lo.x, lo.y, lo.z }, glm::vec3{ hi.x, lo.y, lo.z },
                                                glm::vec3{ hi.x, hi.y, lo.z }, glm::vec3{ lo.x, hi.y, lo.z },
                                                glm::vec3{ lo.x, lo.y, hi.z }, glm::vec3{ hi.x, lo.y, hi.z },
                                                glm::vec3{ hi.x, hi.y, hi.z }, glm::vec3{ lo.x, hi.y, hi.z } };
        constexpr std::array<std::pair<se::u32, se::u32>, 12> Edges{
            std::pair{ 0u, 1u }, std::pair{ 1u, 2u }, std::pair{ 2u, 3u }, std::pair{ 3u, 0u },
            std::pair{ 4u, 5u }, std::pair{ 5u, 6u }, std::pair{ 6u, 7u }, std::pair{ 7u, 4u },
            std::pair{ 0u, 4u }, std::pair{ 1u, 5u }, std::pair{ 2u, 6u }, std::pair{ 3u, 7u }
        };
        for (const auto& edge : Edges)
        {
            addClippedOverlayLine(vertices, viewProjection, corners[edge.first], corners[edge.second], 1.5f, color,
                                  width, height);
        }
    }

    // A world-space ring of `radius` in the plane spanned by the unit axes a, b.
    void addWorldRing(std::vector<se::OverlayVertex>& vertices, const glm::mat4& viewProjection, glm::vec3 center,
                      glm::vec3 a, glm::vec3 b, se::f32 radius, glm::vec4 color, se::u32 width, se::u32 height)
    {
        constexpr se::u32 segments = 32;
        glm::vec3 prev = center + a * radius;
        for (se::u32 i = 1; i <= segments; i = i + 1)
        {
            const se::f32 t = static_cast<se::f32>(i) / static_cast<se::f32>(segments) * glm::two_pi<se::f32>();
            const glm::vec3 cur = center + (a * std::cos(t) + b * std::sin(t)) * radius;
            addClippedOverlayLine(vertices, viewProjection, prev, cur, 1.5f, color, width, height);
            prev = cur;
        }
    }

    // addWorldRing generalized to a sub-range: a world-space arc of `radius` in the plane spanned
    // by unit axes a, b, swept over [t0, t1]. Used for the capsule's pole hemispheres.
    void addWorldArc(std::vector<se::OverlayVertex>& vertices, const glm::mat4& viewProjection, glm::vec3 center,
                     glm::vec3 a, glm::vec3 b, se::f32 radius, se::f32 t0, se::f32 t1, glm::vec4 color, se::u32 width,
                     se::u32 height)
    {
        constexpr se::u32 segments = 16;
        glm::vec3 prev = center + (a * std::cos(t0) + b * std::sin(t0)) * radius;
        for (se::u32 i = 1; i <= segments; i = i + 1)
        {
            const se::f32 t = t0 + (t1 - t0) * static_cast<se::f32>(i) / static_cast<se::f32>(segments);
            const glm::vec3 cur = center + (a * std::cos(t) + b * std::sin(t)) * radius;
            addClippedOverlayLine(vertices, viewProjection, prev, cur, 1.5f, color, width, height);
            prev = cur;
        }
    }

    // An oriented box: transform the 8 local ±he corners by `model` and draw the 12 edges. Unlike
    // addWorldAabb (which re-encloses axis-aligned), this keeps the box oriented by `model`.
    void addWorldOrientedBox(std::vector<se::OverlayVertex>& vertices, const glm::mat4& viewProjection,
                             const glm::mat4& model, glm::vec3 he, glm::vec4 color, se::u32 width, se::u32 height)
    {
        std::array<glm::vec3, 8> corners{};
        for (se::u32 corner = 0; corner < 8; corner = corner + 1)
        {
            const glm::vec3 local{ (corner & 1u) != 0u ? he.x : -he.x, (corner & 2u) != 0u ? he.y : -he.y,
                                   (corner & 4u) != 0u ? he.z : -he.z };
            corners[corner] = glm::vec3(model * glm::vec4(local, 1.0f));
        }
        constexpr std::array<std::pair<se::u32, se::u32>, 12> Edges{
            std::pair{ 0u, 1u }, std::pair{ 1u, 3u }, std::pair{ 3u, 2u }, std::pair{ 2u, 0u },
            std::pair{ 4u, 5u }, std::pair{ 5u, 7u }, std::pair{ 7u, 6u }, std::pair{ 6u, 4u },
            std::pair{ 0u, 4u }, std::pair{ 1u, 5u }, std::pair{ 2u, 6u }, std::pair{ 3u, 7u }
        };
        for (const auto& edge : Edges)
        {
            addClippedOverlayLine(vertices, viewProjection, corners[edge.first], corners[edge.second], 1.5f, color,
                                  width, height);
        }
    }

    // The viewport debug overlays (set-debug-overlays): per-entity bounds = the exact box
    // pickEntity tests (static draw + skinned joint-union), the whole-scene AABB the shadow
    // fit uses, and point/spot light volumes. World-space lines, depth-tested, Edit-only.
    void buildDebugOverlays(se::SceneEditContext& editor, se::AssetServer& assets, se::Renderer& renderer,
                            const se::CameraView& cam, se::u32 width, se::u32 height,
                            std::vector<se::OverlayVertex>& vertices)
    {
        const se::DebugOverlayOptions& opts = editor.debugOverlays;
        if (width == 0 || height == 0 || (!opts.bounds && !opts.sceneAabb && !opts.lightVolumes))
        {
            return;
        }
        se::Scene& scene = se::activeScene(editor);
        const se::f32 aspect = static_cast<se::f32>(width) / static_cast<se::f32>(height);
        const glm::mat4 viewProjection = se::cameraProjection(cam, aspect) * cam.view;
        constexpr glm::vec4 StaticBoundsColor{ 0.35f, 0.95f, 0.55f, 0.9f };
        constexpr glm::vec4 SkinnedBoundsColor{ 0.95f, 0.45f, 0.95f, 0.9f };
        constexpr glm::vec4 SceneAabbColor{ 0.95f, 0.85f, 0.25f, 0.85f };
        constexpr glm::vec4 PointColor{ 1.0f, 0.84f, 0.34f, 0.85f };
        constexpr glm::vec4 SpotColor{ 0.45f, 0.85f, 1.0f, 0.85f };

        glm::vec3 sceneMin{ std::numeric_limits<se::f32>::max() };
        glm::vec3 sceneMax{ std::numeric_limits<se::f32>::lowest() };
        bool haveScene = false;

        se::forEach<se::TransformComponent, se::MeshComponent>(
            scene,
            [&](se::Entity entity, se::TransformComponent&, se::MeshComponent& mesh)
            {
                const se::Ref<se::GpuMesh> meshRef = se::loadMeshAsset(assets, renderer, mesh.mesh);
                if (!meshRef)
                {
                    return;
                }
                glm::vec3 lo{ std::numeric_limits<se::f32>::max() };
                glm::vec3 hi{ std::numeric_limits<se::f32>::lowest() };
                se::worldAabbFromCorners(se::worldMatrix(scene, entity), meshRef->boundsMin, meshRef->boundsMax, lo,
                                         hi);
                if (opts.bounds)
                {
                    addWorldAabb(vertices, viewProjection, lo, hi, StaticBoundsColor, width, height);
                }
                sceneMin = glm::min(sceneMin, lo);
                sceneMax = glm::max(sceneMax, hi);
                haveScene = true;
            });

        se::forEach<se::TransformComponent, se::SkinnedMeshComponent>(
            scene,
            [&](se::Entity, se::TransformComponent&, se::SkinnedMeshComponent& skin)
            {
                const se::Ref<se::GpuMesh> meshRef = se::loadMeshAsset(assets, renderer, skin.mesh);
                if (!meshRef)
                {
                    return;
                }
                std::vector<glm::mat4> palette;
                se::jointMatrices(scene, skin, palette);
                if (palette.empty())
                {
                    return;
                }
                glm::vec3 lo{ std::numeric_limits<se::f32>::max() };
                glm::vec3 hi{ std::numeric_limits<se::f32>::lowest() };
                for (const glm::mat4& joint : palette)
                {
                    se::worldAabbFromCorners(joint, meshRef->boundsMin, meshRef->boundsMax, lo, hi);
                }
                if (opts.bounds)
                {
                    addWorldAabb(vertices, viewProjection, lo, hi, SkinnedBoundsColor, width, height);
                }
                sceneMin = glm::min(sceneMin, lo);
                sceneMax = glm::max(sceneMax, hi);
                haveScene = true;
            });

        // The whole-scene AABB the directional-shadow / DDGI fit derives each frame; renderScene
        // recomputes and discards it, and this recompute intentionally mirrors that one.
        if (opts.sceneAabb && haveScene)
        {
            addWorldAabb(vertices, viewProjection, sceneMin, sceneMax, SceneAabbColor, width, height);
        }

        if (opts.lightVolumes)
        {
            se::forEach<se::TransformComponent, se::PointLightComponent>(
                scene,
                [&](se::Entity entity, se::TransformComponent&, se::PointLightComponent& light)
                {
                    if (light.range <= 0.0f)
                    {
                        return;
                    }
                    const glm::vec3 center = se::worldTranslation(scene, entity);
                    addWorldRing(vertices, viewProjection, center, glm::vec3{ 1.0f, 0.0f, 0.0f },
                                 glm::vec3{ 0.0f, 1.0f, 0.0f }, light.range, PointColor, width, height);
                    addWorldRing(vertices, viewProjection, center, glm::vec3{ 0.0f, 1.0f, 0.0f },
                                 glm::vec3{ 0.0f, 0.0f, 1.0f }, light.range, PointColor, width, height);
                    addWorldRing(vertices, viewProjection, center, glm::vec3{ 1.0f, 0.0f, 0.0f },
                                 glm::vec3{ 0.0f, 0.0f, 1.0f }, light.range, PointColor, width, height);
                });

            se::forEach<se::TransformComponent, se::SpotLightComponent>(
                scene,
                [&](se::Entity entity, se::TransformComponent&, se::SpotLightComponent& light)
                {
                    if (light.range <= 0.0f)
                    {
                        return;
                    }
                    // Matches the lighting upload: dir = normalize(worldRotation * component direction).
                    const glm::vec3 apex = se::worldTranslation(scene, entity);
                    const glm::vec3 dir = glm::normalize(se::worldRotation(scene, entity) * light.direction);
                    glm::vec3 up{ 0.0f, 1.0f, 0.0f };
                    if (std::abs(dir.y) > 0.99f)
                    {
                        up = glm::vec3{ 0.0f, 0.0f, 1.0f };
                    }
                    const glm::vec3 right = glm::normalize(glm::cross(dir, up));
                    const glm::vec3 up2 = glm::cross(right, dir);
                    const glm::vec3 base = apex + dir * light.range;
                    const se::f32 baseRadius =
                        light.range * std::tan(glm::radians(std::clamp(light.outerAngle, 0.5f, 89.0f)));
                    addWorldRing(vertices, viewProjection, base, right, up2, baseRadius, SpotColor, width, height);
                    for (se::u32 i = 0; i < 4; i = i + 1)
                    {
                        const se::f32 t = static_cast<se::f32>(i) / 4.0f * glm::two_pi<se::f32>();
                        const glm::vec3 rim = base + (right * std::cos(t) + up2 * std::sin(t)) * baseRadius;
                        addClippedOverlayLine(vertices, viewProjection, apex, rim, 1.5f, SpotColor, width, height);
                    }
                });
        }
    }

    // The physics collider overlay (set-debug-overlays {colliders}): a world-space wireframe per
    // ColliderComponent — oriented box / sphere / capsule, or the cook-source mesh AABB for hull/mesh.
    // Drawn SCALE-FREE to match the Jolt body: position + rotation only (populatePhysicsWorld), with the
    // collider offset in the rotated body-local frame (wrapOffset) — never worldMatrix (which carries
    // entity scale). Reads the authored ColliderComponent, present in Edit AND Play, so it lives outside
    // editChrome and carries its own preview guard.
    void buildColliderOverlays(se::SceneEditContext& editor, se::AssetServer& assets, se::Renderer& renderer,
                               const se::CameraView& cam, se::u32 width, se::u32 height,
                               std::vector<se::OverlayVertex>& vertices)
    {
        if (!editor.debugOverlays.colliders || editor.previewScene.has_value() || width == 0 || height == 0)
        {
            return;
        }
        se::Scene& scene = se::activeScene(editor);
        const se::f32 aspect = static_cast<se::f32>(width) / static_cast<se::f32>(height);
        const glm::mat4 viewProjection = se::cameraProjection(cam, aspect) * cam.view;
        constexpr glm::vec4 ColliderColor{ 0.20f, 0.95f, 0.85f, 0.9f };  // cyan: solid colliders
        constexpr glm::vec4 SensorColor{ 0.30f, 0.90f, 0.40f, 0.9f };    // green: trigger volumes
        constexpr glm::vec4 SelectedColor{ 1.0f, 0.55f, 0.1f, 1.0f };    // orange: the selected collider

        se::forEach<se::TransformComponent, se::ColliderComponent>(
            scene,
            [&](se::Entity entity, se::TransformComponent&, se::ColliderComponent& collider)
            {
                const glm::vec4 color = editor.selected.handle == entity.handle ? SelectedColor
                                        : collider.isSensor                     ? SensorColor
                                                                                : ColliderColor;
                // Scale-free body frame: T(pos) * R(rot) * T(offset) — the offset rides the rotated
                // body-local frame (wrapOffset), the body carries no scale (populatePhysicsWorld).
                const glm::mat4 model = glm::translate(glm::mat4(1.0f), se::worldTranslation(scene, entity)) *
                                        glm::mat4_cast(se::worldRotation(scene, entity)) *
                                        glm::translate(glm::mat4(1.0f), collider.offset);
                const glm::vec3 he = glm::max(collider.halfExtents, glm::vec3(0.01f));
                const glm::vec3 center = glm::vec3(model[3]);

                switch (collider.shape)
                {
                case se::ColliderComponent::Shape::Box:
                    addWorldOrientedBox(vertices, viewProjection, model, he, color, width, height);
                    break;
                case se::ColliderComponent::Shape::Sphere:
                {
                    // Sphere radius packs from halfExtents.x (mirrors physics.cpp createShape); the three
                    // world-axis rings are rotation-invariant.
                    addWorldRing(vertices, viewProjection, center, glm::vec3{ 1, 0, 0 }, glm::vec3{ 0, 1, 0 }, he.x,
                                 color, width, height);
                    addWorldRing(vertices, viewProjection, center, glm::vec3{ 0, 1, 0 }, glm::vec3{ 0, 0, 1 }, he.x,
                                 color, width, height);
                    addWorldRing(vertices, viewProjection, center, glm::vec3{ 1, 0, 0 }, glm::vec3{ 0, 0, 1 }, he.x,
                                 color, width, height);
                    break;
                }
                case se::ColliderComponent::Shape::Capsule:
                {
                    // Y-up capsule: radius from halfExtents.x, half-height from halfExtents.y. Axes from
                    // the body rotation columns.
                    const se::f32 radius = he.x;
                    const se::f32 halfHeight = he.y;
                    const glm::vec3 right = glm::normalize(glm::vec3(model[0]));
                    const glm::vec3 up = glm::normalize(glm::vec3(model[1]));
                    const glm::vec3 fwd = glm::normalize(glm::vec3(model[2]));
                    const glm::vec3 topC = center + up * halfHeight;
                    const glm::vec3 botC = center - up * halfHeight;
                    addWorldRing(vertices, viewProjection, topC, right, fwd, radius, color, width, height);
                    addWorldRing(vertices, viewProjection, botC, right, fwd, radius, color, width, height);
                    for (const glm::vec3& side : { right, -right, fwd, -fwd })
                    {
                        addClippedOverlayLine(vertices, viewProjection, topC + side * radius, botC + side * radius,
                                              1.5f, color, width, height);
                    }
                    const se::f32 pi = glm::pi<se::f32>();
                    addWorldArc(vertices, viewProjection, topC, right, up, radius, 0.0f, pi, color, width, height);
                    addWorldArc(vertices, viewProjection, topC, fwd, up, radius, 0.0f, pi, color, width, height);
                    addWorldArc(vertices, viewProjection, botC, right, up, radius, pi, 2.0f * pi, color, width, height);
                    addWorldArc(vertices, viewProjection, botC, fwd, up, radius, pi, 2.0f * pi, color, width, height);
                    break;
                }
                case se::ColliderComponent::Shape::ConvexHull:
                case se::ColliderComponent::Shape::Mesh:
                {
                    // The documented cook-source-AABB approximation (no CPU hull edges are kept): resolve
                    // the cook mesh (sourceMesh, else the entity's Mesh, else SkinnedMesh) and draw its
                    // bounds box, oriented by the same scale-free body frame.
                    se::Uuid meshId = collider.sourceMesh;
                    if (meshId.value == 0 && se::hasComponent<se::MeshComponent>(scene, entity))
                    {
                        meshId = se::getComponent<se::MeshComponent>(scene, entity).mesh;
                    }
                    else if (meshId.value == 0 && se::hasComponent<se::SkinnedMeshComponent>(scene, entity))
                    {
                        meshId = se::getComponent<se::SkinnedMeshComponent>(scene, entity).mesh;
                    }
                    if (meshId.value == 0)
                    {
                        break;
                    }
                    const se::Ref<se::GpuMesh> meshRef = se::loadMeshAsset(assets, renderer, meshId);
                    if (!meshRef)
                    {
                        break;
                    }
                    const glm::vec3 boundsCenter = (meshRef->boundsMin + meshRef->boundsMax) * 0.5f;
                    const glm::vec3 boundsHe =
                        glm::max((meshRef->boundsMax - meshRef->boundsMin) * 0.5f, glm::vec3(0.01f));
                    const glm::mat4 boxModel = model * glm::translate(glm::mat4(1.0f), boundsCenter);
                    addWorldOrientedBox(vertices, viewProjection, boxModel, boundsHe, color, width, height);
                    break;
                }
                }
            });
    }

    // Builds the editor overlay and submits it once per frame: camera frustums are
    // depth-tested against the scene (occluded by geometry); billboards, the active gizmo,
    // and the skeleton overlay always draw on top. The gizmo + billboards + frustums are
    // Edit-only editor chrome; the skeleton overlay (when shown) renders in every play
    // state so a played clip shows its bones move. `editChrome` is false during play.
    void submitSceneEditOverlay(se::SceneEditContext& editor, se::AssetServer& assets, se::Renderer& renderer,
                                const se::CameraView& cam, se::u32 width, se::u32 height, bool editChrome)
    {
        std::vector<se::OverlayVertex> depthTested;
        std::vector<se::OverlayVertex> onTop;
        if (editChrome)
        {
            buildSceneEditCameraFrustums(editor, cam, width, height, depthTested);
            buildDebugOverlays(editor, assets, renderer, cam, width, height, depthTested);
            buildSceneEditBillboards(editor, cam, width, height, onTop);
            buildNativeGizmo(editor, cam, width, height, onTop);
        }
        // Colliders draw in Edit AND Play (they read the authored ColliderComponent), so they sit
        // outside editChrome like the skeleton overlay, with their own preview guard (inside the call).
        buildColliderOverlays(editor, assets, renderer, cam, width, height, depthTested);
        buildSkeletonOverlay(editor, cam, width, height, onTop);
        se::submitOverlay(renderer, std::move(depthTested), std::move(onTop));
    }

}

export namespace se
{
    /// Builds the editor App (window + renderer + UI + editor/control/asset state),
    /// runs the main loop, and returns the process exit code. Takes plain title/size
    /// so the caller (main) needs no engine config types.
    auto runHost(std::string title, u32 width, u32 height) -> int
    {
        auto state = std::make_shared<HostState>();

        se::AppConfig config;
        config.window = se::WindowConfig{
            .title = std::move(title),
            .width = width,
            .height = height,
            .hidden = std::getenv("SAFFRON_EDITOR_NATIVE_VIEWPORT") != nullptr,
        };

        config.onCreate = [state](se::App& app)
        {
            state->editor = se::newSceneEditContext();
            state->control = se::newControlContext();
            state->assets = se::newAssetServer(se::assetPath("assets"));

            // The animation evaluator lives below Saffron.Assets, so it can't read a clip out of its
            // `.smodel` container itself; the Host hands it a loader that resolves a clip id to bytes.
            // The runtime and the AssetServer are siblings in HostState, so a raw pointer is lifetime-safe.
            se::AssetServer* assets = &state->assets;
            state->animation.clipLoader = [assets](se::Uuid id) { return se::loadAnimationClipAsset(*assets, id); };

            // Registered here, not in Saffron.Control: the handler needs the Lua
            // schema reader, and the Host is the only TU that may import Script.
            se::registerCommand<se::GetScriptSchemaParams, se::GetScriptSchemaResult>(
                state->control->registry, "get-script-schema",
                "get-script-schema {path} — a project script's declared fields (path relative to src/)",
                [state](se::EngineContext&,
                        const se::GetScriptSchemaParams& params) -> se::Result<se::GetScriptSchemaResult>
                {
                    if (params.path.empty() || params.path.find("..") != std::string::npos)
                    {
                        return se::Err(std::string{ "path must be relative to the project src/" });
                    }
                    const std::filesystem::path file =
                        std::filesystem::path(state->editor->projectRoot) / "src" / params.path;
                    auto schema = se::readScriptSchema(file.string());
                    if (!schema)
                    {
                        return se::Err(schema.error());
                    }
                    se::GetScriptSchemaResult out;
                    for (se::ScriptField& field : *schema)
                    {
                        out.fields.push_back(se::ScriptFieldDto{ std::move(field.name),
                                                                 se::scriptFieldTypeName(field.type),
                                                                 std::move(field.defaultValue) });
                    }
                    return out;
                });
            // The editor is the headless native-viewport host: always present-only (no engine
            // panels), driven over the control plane.
            se::setPresentViewportOnly(app.renderer, true);
            // The editor sets a per-view shm segment env var: each view publishes its frames
            // into its own segment for the compositor-side presenter instead of presenting to
            // the (hidden) swapchain. Only the active view publishes new frames each frame
            // (gated by activeShmPublish/activeView), but both segments must exist so both
            // panes have a ring. A standalone/CLI/headless run may set neither (or just scene).
            if (const char* shm = std::getenv("SAFFRON_VIEWPORT_SHM_SCENE"); shm != nullptr && shm[0] != '\0')
            {
                se::enableViewportShmPublish(app.renderer, se::ViewId::Scene, shm);
                state->shmPublish = true;
            }
            if (const char* shm = std::getenv("SAFFRON_VIEWPORT_SHM_ASSET"); shm != nullptr && shm[0] != '\0')
            {
                se::enableViewportShmPublish(app.renderer, se::ViewId::AssetPreview, shm);
                state->shmPublish = true;
            }
            // Default AA: MSAA 4x, clamped to device support. A loaded project's
            // renderSettings block overrides it below.
            se::setAa(app.renderer, 4, false, false);

            // The registry exists for its JSON serde (scene save/load + control plane); the
            // present-only host renders no inspector, so no draw lambdas / thumbnails.
            se::registerBuiltinComponents(state->editor->registry);

            // Script lifecycle: the VM exists exactly while play is active — created on
            // Edit->Playing, kept across pause/resume, destroyed on ->Edit. Scripts load
            // from the project's src/ directory.
            state->scriptSubscription = state->editor->onPlayStateChanged.subscribe(
                [state](se::PlayState next)
                {
                    if (next == se::PlayState::Playing && !state->scriptVmActive)
                    {
                        const std::filesystem::path src = std::filesystem::path(state->editor->projectRoot) / "src";
                        auto started =
                            se::startScripts(state->script, se::activeScene(*state->editor), state->editor->registry,
                                             src.string(), state->editor->scriptInput);
                        if (!started)
                        {
                            se::logError(std::format("script start failed: {}", started.error()));
                            return false;
                        }
                        state->scriptVmActive = true;
                        state->editor->scriptInstanceCount = static_cast<se::i32>(state->script.instances.size());
                    }
                    else if (next == se::PlayState::Edit && state->scriptVmActive)
                    {
                        se::stopScripts(state->script);
                        state->scriptVmActive = false;
                        state->editor->scriptInstanceCount = 0;
                    }
                    return false;
                });
            // Physics lifecycle: a Jolt world exists exactly while play is active, built on
            // Edit->Playing and dropped on ->Edit — a sibling of the script VM above. The Jolt
            // globals install lazily on the first play so the self-test (which init/shutdowns
            // its own globals) stays independent.
            state->physicsSubscription = state->editor->onPlayStateChanged.subscribe(
                [state](se::PlayState next)
                {
                    if (next == se::PlayState::Playing && !state->physics.has_value())
                    {
                        if (!state->physicsInit)
                        {
                            if (auto inited = se::initPhysics(); !inited)
                            {
                                se::logError(std::format("physics init failed: {}", inited.error()));
                                return false;
                            }
                            state->physicsInit = true;
                        }
                        auto world = se::createPhysicsWorld();
                        if (!world)
                        {
                            se::logError(std::format("physics world create failed: {}", world.error()));
                            return false;
                        }
                        state->physics.emplace(std::move(*world));
                        // Turn the play scene's components into Jolt bodies (collider -> shape,
                        // rigidbody -> motion/mass). The cook seam decodes .smesh CPU vertices for
                        // convex-hull/mesh shapes, keeping <Jolt/...> out of Saffron.Assets.
                        se::populatePhysicsWorld(*state->physics, se::activeScene(*state->editor),
                                                 [state](se::Uuid id) -> se::Result<se::Mesh>
                                                 { return se::loadMeshCpuAsset(state->assets, id); });
                        // Per-bone kinematic bodies for rigs that opted into bone-following.
                        se::buildBoneBodies(*state->physics, se::activeScene(*state->editor));
                        // A CharacterVirtual per character-controller entity.
                        se::forEach<se::CharacterControllerComponent>(
                            se::activeScene(*state->editor),
                            [state](se::Entity character, se::CharacterControllerComponent&)
                            {
                                static_cast<void>(
                                    se::addCharacter(*state->physics, character, se::activeScene(*state->editor)));
                            });
                        state->contactCursor = 0;  // fresh world, fresh contact-event seq
                    }
                    else if (next == se::PlayState::Edit && state->physics.has_value())
                    {
                        state->physics.reset();  // RAII: removes bodies + frees the Jolt world
                    }
                    return false;
                });
            // The simulation seam, composed physics-then-scripts: physics steps + writes transforms
            // back first, so a script reading a body's post-step transform sees this frame's settled
            // physics (the mirror of "animation before scripts"). Animation already ran ahead of
            // tickPlay, so the per-frame order is animation -> (physics -> scripts) -> renderScene.
            state->editor->simTick = [state](se::Scene& scene, se::f32 dt)
            {
                if (state->physics.has_value())
                {
                    // Active ragdolls track the animation: motor each toward this frame's animated
                    // pose (the evaluator's lastPose, bones-order), ease the per-bone physics weight,
                    // then step. Drive before the step so the motors are read during the solve.
                    std::vector<se::PoseTarget> targets;
                    targets.reserve(state->animation.lastPose.size());
                    for (const auto& [rig, pose] : state->animation.lastPose)
                    {
                        targets.push_back(se::PoseTarget{ .rig = rig, .local = pose });
                    }
                    se::driveRagdollsToPose(*state->physics, targets);
                    se::advanceRagdollBlend(*state->physics, dt);
                    se::stepPhysics(*state->physics, scene, dt);
                    // Ragdoll bodies drive the bones: write each part's pose into the bone's
                    // PoseOverride (after tickAnimation, after the step) so physics wins the frame.
                    se::writeRagdollPoses(*state->physics, scene);
                    // Drain this tick's new contacts and dispatch them to scripts before on_update,
                    // so a trigger/contact handler runs the same frame the contact fired.
                    if (state->scriptVmActive)
                    {
                        const se::ContactDrain drain = se::drainContacts(*state->physics, state->contactCursor);
                        state->contactCursor = drain.highWaterSeq;
                        for (const se::ContactEvent& event : drain.events)
                        {
                            auto failure = se::dispatchContact(
                                state->script, scene, event.entityA, event.entityB,
                                event.kind == se::ContactEvent::Kind::Begin, event.sensor, event.point.x, event.point.y,
                                event.point.z, event.normal.x, event.normal.y, event.normal.z);
                            if (failure)
                            {
                                se::logError(std::format("script contact handler in '{}': {}", failure->script,
                                                         failure->message));
                                se::pushScriptError(*state->editor, failure->entityUuid, failure->script,
                                                    std::move(failure->message));
                                state->scriptErrorPending = true;
                                break;
                            }
                        }
                    }
                }
                if (!state->scriptVmActive)
                {
                    return;
                }
                se::deriveScriptInputEdges(state->editor->scriptInput);
                if (auto failure = se::tickScripts(state->script, scene, dt))
                {
                    se::logError(std::format("script error in '{}': {}", failure->script, failure->message));
                    se::pushScriptError(*state->editor, failure->entityUuid, failure->script,
                                        std::move(failure->message));
                    state->scriptErrorPending = true;
                }
            };
            // Bridge se.raycast (Lua) to the live physics world without Saffron.Script importing
            // Physics: the binding calls this, which queries the world while it exists (null in Edit).
            state->script.raycast = [state](se::f32 ox, se::f32 oy, se::f32 oz, se::f32 dx, se::f32 dy, se::f32 dz,
                                            se::f32 maxDist) -> se::ScriptRayHit
            {
                if (!state->physics.has_value())
                {
                    return {};
                }
                const se::PhysicsRayHit hit =
                    se::raycastWorld(*state->physics, glm::vec3(ox, oy, oz), glm::vec3(dx, dy, dz), maxDist);
                return se::ScriptRayHit{ .hit = hit.hit,
                                         .entity = hit.entity,
                                         .px = hit.point.x,
                                         .py = hit.point.y,
                                         .pz = hit.point.z,
                                         .nx = hit.normal.x,
                                         .ny = hit.normal.y,
                                         .nz = hit.normal.z,
                                         .distance = hit.distance };
            };
            state->script.sphereCast = [state](se::f32 ox, se::f32 oy, se::f32 oz, se::f32 dx, se::f32 dy, se::f32 dz,
                                               se::f32 radius, se::f32 maxDist) -> se::ScriptRayHit
            {
                if (!state->physics.has_value())
                {
                    return {};
                }
                const se::PhysicsRayHit hit =
                    se::sphereCastWorld(*state->physics, glm::vec3(ox, oy, oz), glm::vec3(dx, dy, dz), radius, maxDist);
                return se::ScriptRayHit{ .hit = hit.hit,
                                         .entity = hit.entity,
                                         .px = hit.point.x,
                                         .py = hit.point.y,
                                         .pz = hit.point.z,
                                         .nx = hit.normal.x,
                                         .ny = hit.normal.y,
                                         .nz = hit.normal.z,
                                         .distance = hit.distance };
            };
            state->script.applyImpulse = [state](se::u64 entity, glm::vec3 v)
            {
                if (state->physics.has_value())
                {
                    se::applyBodyImpulse(*state->physics, entity, v);
                }
            };
            state->script.addForce = [state](se::u64 entity, glm::vec3 v)
            {
                if (state->physics.has_value())
                {
                    se::addBodyForce(*state->physics, entity, v);
                }
            };
            state->script.setVelocity = [state](se::u64 entity, glm::vec3 v)
            {
                if (state->physics.has_value())
                {
                    se::setBodyLinearVelocity(*state->physics, entity, v);
                }
            };
            state->script.getVelocity = [state](se::u64 entity) -> glm::vec3
            {
                return state->physics.has_value() ? se::bodyLinearVelocity(*state->physics, entity) : glm::vec3{ 0.0f };
            };
            state->script.setRagdollEnabled = [state](se::u64 rig, bool enable) -> bool
            {
                if (!state->physics.has_value())
                {
                    return false;
                }
                if (!enable)
                {
                    se::disableRagdoll(*state->physics, rig);
                    return true;
                }
                se::Scene& scene = se::activeScene(*state->editor);
                const se::Entity entity = se::findEntityByUuid(scene, rig);
                if (!se::valid(scene, entity))
                {
                    return false;
                }
                if (auto enabled = se::enableRagdoll(*state->physics, scene, entity); !enabled)
                {
                    se::logWarn(std::format("script: enable_ragdoll: {}", enabled.error()));
                    return false;
                }
                return true;
            };
            state->script.setRagdollBlend = [state](se::u64 rig, bool active, se::f32 weight)
            {
                if (state->physics.has_value())
                {
                    static_cast<void>(
                        se::setRagdollBlend(*state->physics, rig, active, weight, std::nullopt, std::nullopt));
                }
            };
            state->script.ragdollState = [state](se::u64 rig) -> se::ScriptRagdollState
            {
                if (!state->physics.has_value())
                {
                    return {};
                }
                const se::RagdollState s = se::ragdollState(*state->physics, rig);
                return se::ScriptRagdollState{
                    .present = s.present, .active = s.active, .bodyWeight = s.bodyWeight, .bones = s.bones
                };
            };
            // se.log → the edit context's script-log ring, so the editor's Script Logs panel can drain it.
            state->script.logSink = [state](se::u64 uuid, const char* msg)
            { se::pushScriptLog(*state->editor, uuid, msg != nullptr ? msg : ""); };

            // Headless self-test entry point: pairs with SAFFRON_EXIT_AFTER_FRAMES for
            // CI-style runs; results land in the log.
            if (std::getenv("SAFFRON_SELFTEST") != nullptr)
            {
                se::runSceneSerializationSelfTest();
                se::runSceneHierarchySelfTest();
                se::runPlayModeSelfTest();
                se::runGeometrySelfTest(se::assetPath("models"));
                se::runContainerMetadataSelfTest();
                se::runCatalogLinkageSelfTest();
                se::runBakeModelSelfTest();
                se::runChunkLoaderSelfTest();
                se::runInstantiateSelfTest();
                se::runExtractSelfTest();
                se::runReimportSelfTest();
                if (auto animation = se::runAnimationSelfTest(); !animation)
                {
                    se::logError(animation.error());
                }
                if (auto script = se::runScriptSelfTest(); !script)
                {
                    se::logError(script.error());
                }
                else
                {
                    se::logInfo("script self-test passed");
                }
                if (auto signal = se::runSignalSelfTest(); !signal)
                {
                    se::logError(signal.error());
                }
                else
                {
                    se::logInfo("signal self-test passed");
                }
                if (auto physics = se::runPhysicsSelfTest(); !physics)
                {
                    se::logError(physics.error());
                }
                else
                {
                    se::logInfo("physics self-test passed");
                }
            }

            // Auto-load the selected project (SAFFRON_PROJECT), then an auto-empty project,
            // then a project.json in the working directory; otherwise wait for the Tauri
            // project picker to create/open one.
            constexpr const char* defaultProject = "project.json";
            auto applyProject = [state](const se::ProjectInfo& project)
            {
                state->editor->projectLoaded = project.loaded;
                state->editor->projectRoot = project.root;
                state->editor->projectPath = project.path;
                state->editor->projectName = project.name;
                state->editor->projectDisplayName = project.displayName;
                state->editor->scenePath = project.path;
                state->animation.clipCache.clear();  // the new catalog rebinds clip ids
            };
            if (const char* selected = std::getenv("SAFFRON_PROJECT"); selected != nullptr && selected[0] != '\0')
            {
                se::ProjectInfo project;
                nlohmann::json editorCamera;
                nlohmann::json debugOverlays;
                se::Result<void> result = {};
                if (se::validProjectName(selected) && !std::filesystem::exists(se::projectJsonPath(selected)))
                {
                    result = se::createProject(state->assets, app.renderer, state->editor->registry,
                                               state->editor->scene, project, selected, "");
                }
                else
                {
                    result = se::loadProject(state->assets, app.renderer, state->editor->registry, state->editor->scene,
                                             project, selected, &editorCamera, &debugOverlays);
                }
                if (result)
                {
                    applyProject(project);
                    se::sceneEditCameraFromJson(state->editor->camera, editorCamera);
                    se::debugOverlaysFromJson(state->editor->debugOverlays, debugOverlays);
                }
                else
                {
                    se::logError(result.error());
                }
            }
            else if (std::getenv("SAFFRON_AUTO_EMPTY_PROJECT") != nullptr)
            {
                se::ProjectInfo project;
                if (auto result = se::createAutoEmptyProject(state->assets, app.renderer, state->editor->registry,
                                                             state->editor->scene, project))
                {
                    applyProject(project);
                }
                else
                {
                    se::logError(result.error());
                }
            }
            else if (std::filesystem::exists(defaultProject))
            {
                se::ProjectInfo project;
                nlohmann::json editorCamera;
                nlohmann::json debugOverlays;
                if (auto result =
                        se::loadProject(state->assets, app.renderer, state->editor->registry, state->editor->scene,
                                        project, defaultProject, &editorCamera, &debugOverlays))
                {
                    applyProject(project);
                    se::sceneEditCameraFromJson(state->editor->camera, editorCamera);
                    se::debugOverlaysFromJson(state->editor->debugOverlays, debugOverlays);
                }
                else
                {
                    se::logError(result.error());
                }
            }

            // The native-viewport host has no hierarchy panel to select from; auto-select
            // the first mesh entity so the embedded viewport starts with something selected.
            se::Entity renderable{ entt::null };
            se::forEach<se::MeshComponent>(state->editor->scene,
                                           [&renderable](se::Entity entity, se::MeshComponent&)
                                           {
                                               if (renderable.handle == entt::null)
                                               {
                                                   renderable = entity;
                                               }
                                           });
            if (renderable.handle != entt::null)
            {
                se::setSelection(*state->editor, renderable);
            }

            // When the Tauri editor spawns this host (NATIVE_VIEWPORT marker), exit if the editor
            // dies however it dies — a crash or SIGKILL skips the editor's normal teardown and would
            // otherwise leave this process orphaned. getppid() changing (the host is reparented away
            // from the editor) is the parent-death signal; gated on the marker so a standalone/CLI/e2e
            // run, whose parent is a shell, is never auto-killed.
            const bool editorSpawned = std::getenv("SAFFRON_EDITOR_NATIVE_VIEWPORT") != nullptr;
            pid_t editorPid = 0;
            if (editorSpawned)
            {
                editorPid = getppid();
            }

            // Off-thread thumbnail generation: a cold-cache get-thumbnail enqueues here and replies
            // pending rather than blocking the frame loop on decode + upload + render.
            se::startThumbnailWorker(state->assets, app.renderer);

            se::Layer layer;
            layer.name = "HostLayer";
            layer.onUpdate = [state, &app, editorSpawned, editorPid](se::TimeSpan dt)
            {
                if (editorSpawned && getppid() != editorPid)
                {
                    app.window.shouldClose = true;
                    return;
                }
                if (state->control != nullptr)
                {
                    se::PhysicsWorld* world = state->physics.has_value() ? &*state->physics : nullptr;
                    se::pollControl(*state->control, app.window, app.renderer, *state->editor, state->assets, world);
                }
                // Insert any thumbnails the worker finished this interval into the GPU caches.
                se::drainThumbnailCompletions(state->assets);
                // Entering or leaving the asset preview swaps activeScene to a fresh entity set; drop the
                // anim runtime's per-entity transition/pose entries so a re-entered preview starts clean
                // and dead preview-entity entries never accumulate across opens.
                if (const bool previewing = state->editor->previewScene.has_value(); previewing != state->previewActive)
                {
                    state->animation.transitions.clear();
                    state->animation.lastPose.clear();
                    state->previewActive = previewing;
                }
                // Animation runs every frame in both Edit (preview) and Play, before
                // scripts so a script can still override a bone the same frame. It only
                // writes runtime PoseOverrideComponents, never the authored rest pose.
                se::AnimMode animMode = se::AnimMode::Play;
                if (state->editor->playState == se::PlayState::Edit)
                {
                    animMode = se::AnimMode::Edit;
                }
                se::tickAnimation(state->animation, se::activeScene(*state->editor), dt.seconds, animMode);
                // Control first, tick second: a play/pause/step command that arrives this
                // frame takes effect this frame (a step runs its tick the same frame).
                se::tickPlay(*state->editor, dt.seconds);
                // A script error pauses play, but never from inside the tick (that would
                // re-enter the play state machine); flip once here. A stepped tick can
                // error while already paused — that pause rejection is fine to drop.
                if (state->scriptErrorPending)
                {
                    state->scriptErrorPending = false;
                    auto paused = se::pausePlay(*state->editor);
                    static_cast<void>(paused);
                }
                // Command-driven gizmo drags arrive at the webview's pointer rate (~60Hz);
                // smooth toward the latest sample every frame so the drag renders fluidly.
                {
                    const se::CameraView cam = se::sceneEditCameraView(state->editor->camera);
                    se::stepNativeGizmoDrag(*state->editor, cam, se::viewportWidth(app.renderer),
                                            se::viewportHeight(app.renderer), dt.seconds);
                }
                // Smoothed edits (`set-material`/`set-transform smooth:1`) converge the same way.
                se::stepEditSmoothing(*state->editor, dt.seconds);
                // Fly-cam: the editor streams pointer-lock input over the control plane
                // (fly-input command). Drain the accumulated look delta each frame so a
                // burst of samples between frames is not lost.
                const se::SceneEditCameraInput input = state->editor->flyInput;
                state->editor->flyInput.lookDelta = glm::vec2{ 0.0f };
                se::updateSceneEditCamera(state->editor->camera, input, dt.seconds);
            };
            // Headless host: the Tauri editor spawns this process and presents its frames
            // from shared memory. There are no engine panels — the scene renders through
            // the editor (fly-cam) camera into the offscreen target, with the gizmo handles
            // + entity billboards drawn by the engine overlay pass. The full editor UI is
            // the React/Tauri frontend, which drives this host over the control plane.
            layer.onUi = [state, &app]()
            {
                se::SceneEditContext& editor = *state->editor;
                se::Scene& live = se::activeScene(editor);
                // The pickers + serde read the catalog through the scene (a borrowed
                // pointer, valid only for this frame); also set on the control side.
                live.catalog = &state->assets.catalog;
                // Publish mode: the editor owns the render size (set-viewport-size); the
                // hidden SDL window's size is meaningless. Present mode tracks the window.
                if (!state->shmPublish)
                {
                    se::setViewportDesiredSize(app.renderer, se::ViewId::Scene, app.window.width, app.window.height);
                }
                se::syncNativeGizmo(editor);
                se::CameraView cam = se::renderCameraView(editor);
                const se::u32 viewWidth = se::viewportWidth(app.renderer);
                const se::u32 viewHeight = se::viewportHeight(app.renderer);
                if (viewWidth > 0 && viewHeight > 0)
                {
                    se::RenderSceneOptions options;
                    options.showEditorCameraModels = editor.playState == se::PlayState::Edit;
                    // The grid is Edit-only debug chrome, like the bounds/light-volume overlays.
                    options.showGrid = editor.debugOverlays.grid && editor.playState == se::PlayState::Edit &&
                                       !editor.previewScene.has_value();
                    se::renderScene(app.renderer, live, state->assets, cam, options);
                    // The gizmo + billboards are editor chrome: hidden inside the game view, and during
                    // the asset preview (an "Edit without chrome" view). The gizmo would write transforms
                    // the play duplicate swallows. The skeleton overlay (when shown) still draws in both.
                    const bool editChrome = editor.playState == se::PlayState::Edit && !editor.previewScene.has_value();
                    se::submitSceneEditOverlay(editor, state->assets, app.renderer, cam, viewWidth, viewHeight,
                                               editChrome);
                }
            };
            se::attachLayer(app, std::move(layer));

            app.window.onKeyPressed.subscribe(
                [&app](se::i32 key, bool isRepeat)
                {
                    static_cast<void>(isRepeat);
                    if (key == KeyEscape)
                    {
                        app.window.shouldClose = true;
                    }
                    return false;
                });
        };

        config.onExit = [state](se::App& app)
        {
            static_cast<void>(app);
            // Drain + join the thumbnail worker before any teardown: it borrows the renderer, and run()
            // calls waitGpuIdle/destroyRenderer only after onExit returns.
            se::stopThumbnailWorker(state->assets);
            if (state->control != nullptr)
            {
                se::destroyControlContext(state->control);
                state->control = nullptr;
            }
            if (state->editor != nullptr)
            {
                // Quit can land mid-play: tear the VM down first (it never touches the
                // scene), drop the physics world (RAII frees its Jolt objects), detach the
                // seams, then free the context.
                se::stopScripts(state->script);
                state->scriptVmActive = false;
                state->physics.reset();
                state->editor->simTick = nullptr;
                state->editor->onPlayStateChanged.unsubscribe(state->scriptSubscription);
                state->editor->onPlayStateChanged.unsubscribe(state->physicsSubscription);
                se::destroySceneEditContext(state->editor);
                state->editor = nullptr;
            }
            // The Jolt globals outlive every world; shut them down only after the last
            // world is gone (above) so the Factory/registered types outlive all bodies.
            if (state->physicsInit)
            {
                se::shutdownPhysics();
                state->physicsInit = false;
            }
            // Drop every GPU Ref this client holds before destroyRenderer frees the
            // device/allocator — otherwise the cached meshes/textures and the pipeline
            // would be freed too late (use-after-free).
            state->assets.meshRefByUuid.clear();
            state->assets.textureRefByUuid.clear();
        };

        return se::run(std::move(config));
    }
}
