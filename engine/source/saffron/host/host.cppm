module;

// SDL3 + glm are header-heavy C++ headers, so this TU uses classic includes (no
// `import std`) — consistent with the engine's rendering/scene modules.
#include <entt/entt.hpp>
#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

export module Saffron.Host;

import Saffron.Core;
import Saffron.App;
import Saffron.Window;
import Saffron.Rendering;
import Saffron.SceneEdit;
import Saffron.Control;
import Saffron.Scene;
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
        bool shmPublish = false;  // frames publish to shared memory; the editor owns the render size
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
                 glm::vec4 color, se::u32 width, se::u32 height)
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
        vertices.push_back(se::OverlayVertex{ a0, color, edgePos });
        vertices.push_back(se::OverlayVertex{ b0, color, edgePos });
        vertices.push_back(se::OverlayVertex{ b1, color, edgeNeg });
        vertices.push_back(se::OverlayVertex{ a0, color, edgePos });
        vertices.push_back(se::OverlayVertex{ b1, color, edgeNeg });
        vertices.push_back(se::OverlayVertex{ a1, color, edgeNeg });
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
                addBox(vertices, end.pixel, editor.nativeGizmo.mode == se::NativeGizmoMode::Scale ? 12.0f : 8.0f,
                       se::axisColor(handles[i], editor.nativeGizmo), width, height);
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

    // Colored screen-space glyphs for meshless light/camera entities.
    void buildSceneEditBillboards(se::SceneEditContext& editor, const se::CameraView& cam, se::u32 width,
                                  se::u32 height, std::vector<se::OverlayVertex>& vertices)
    {
        if (width == 0 || height == 0)
        {
            return;
        }
        const glm::vec4 selectedColor{ 1.0f, 0.78f, 0.18f, 1.0f };

        se::forEach<se::TransformComponent>(
            editor.scene,
            [&](se::Entity e, se::TransformComponent&)
            {
                const BillboardKind kind = billboardKind(editor.scene, e);
                if (kind == BillboardKind::None)
                {
                    return;
                }
                const glm::vec3 position = se::worldTranslation(editor.scene, e);
                const se::GizmoProjection p = se::viewportProject(cam, width, height, position);
                if (!p.visible)
                {
                    return;
                }
                const bool sel = editor.selected.handle == e.handle;
                if (kind == BillboardKind::PointLight)
                {
                    addBulbIcon(vertices, p.pixel, sel ? selectedColor : glm::vec4{ 1.0f, 0.84f, 0.34f, 0.95f }, width,
                                height);
                    return;
                }
                if (kind == BillboardKind::SpotLight)
                {
                    const glm::vec4 color = sel ? selectedColor : glm::vec4{ 0.45f, 0.85f, 1.0f, 0.9f };
                    addBulbIcon(vertices, p.pixel, color, width, height);
                    const glm::vec3 forward = se::worldRotation(editor.scene, e) * glm::vec3{ 0.0f, 0.0f, -1.0f };
                    const se::GizmoProjection tip = se::viewportProject(cam, width, height, position + forward * 0.6f);
                    if (tip.visible)
                    {
                        addLine(vertices, p.pixel, tip.pixel, 3.0f, color, width, height);
                    }
                    return;
                }
                if (kind == BillboardKind::Camera)
                {
                    addCameraIcon(vertices, p.pixel, sel ? selectedColor : glm::vec4{ 0.85f, 0.87f, 0.92f, 0.95f },
                                  width, height);
                }
            });
    }

    // Builds the combined overlay (billboards first, gizmo on top) + submits it to the renderer.
    void submitNativeGizmo(se::SceneEditContext& editor, se::Renderer& renderer, const se::CameraView& cam,
                           se::u32 width, se::u32 height)
    {
        std::vector<se::OverlayVertex> vertices;
        buildSceneEditBillboards(editor, cam, width, height, vertices);
        buildNativeGizmo(editor, cam, width, height, vertices);
        se::submitOverlay(renderer, std::move(vertices));
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
            // The editor is the headless native-viewport host: always present-only (no engine
            // panels), driven over the control plane.
            se::setPresentViewportOnly(app.renderer, true);
            // The editor sets SAFFRON_VIEWPORT_SHM: frames publish into shared memory for
            // its compositor-side presenter instead of presenting to the (hidden) swapchain.
            if (const char* shm = std::getenv("SAFFRON_VIEWPORT_SHM"); shm != nullptr && shm[0] != '\0')
            {
                se::enableViewportShmPublish(app.renderer, shm);
                state->shmPublish = true;
            }

            // The registry exists for its JSON serde (scene save/load + control plane); the
            // present-only host renders no inspector, so no draw lambdas / thumbnails.
            se::registerBuiltinComponents(state->editor->registry);

            // Headless self-test entry point: pairs with SAFFRON_EXIT_AFTER_FRAMES for
            // CI-style runs; results land in the log.
            if (std::getenv("SAFFRON_SELFTEST") != nullptr)
            {
                se::runSceneSerializationSelfTest();
                se::runSceneHierarchySelfTest();
            }

            // Auto-load a selected project, then legacy root project.json; otherwise wait
            // for the Tauri project picker to create/open one.
            constexpr const char* defaultProject = "project.json";
            auto applyProject = [state](const se::ProjectInfo& project)
            {
                state->editor->projectLoaded = project.loaded;
                state->editor->projectRoot = project.root;
                state->editor->projectPath = project.path;
                state->editor->projectName = project.name;
                state->editor->projectDisplayName = project.displayName;
                state->editor->scenePath = project.path;
            };
            if (const char* selected = std::getenv("SAFFRON_PROJECT"); selected != nullptr && selected[0] != '\0')
            {
                se::ProjectInfo project;
                se::Result<void> result = {};
                if (se::validProjectName(selected) && !std::filesystem::exists(se::projectJsonPath(selected)))
                {
                    result = se::createProject(state->assets, app.renderer, state->editor->registry,
                                               state->editor->scene, project, selected, "");
                }
                else
                {
                    result = se::loadProject(state->assets, app.renderer, state->editor->registry, state->editor->scene,
                                             project, selected);
                }
                if (result)
                {
                    applyProject(project);
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
                if (auto result = se::loadProject(state->assets, app.renderer, state->editor->registry,
                                                  state->editor->scene, project, defaultProject))
                {
                    applyProject(project);
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

            se::Layer layer;
            layer.name = "HostLayer";
            layer.onUpdate = [state, &app](se::TimeSpan dt)
            {
                if (state->control != nullptr)
                {
                    se::pollControl(*state->control, app.window, app.renderer, *state->editor, state->assets);
                }
                // Command-driven gizmo drags arrive at the webview's pointer rate (~60Hz);
                // smooth toward the latest sample every frame so the drag renders fluidly.
                {
                    const se::CameraView cam = se::sceneEditCameraView(state->editor->camera);
                    se::stepNativeGizmoDrag(*state->editor, cam, se::viewportWidth(app.renderer),
                                            se::viewportHeight(app.renderer), dt.seconds);
                }
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
                // The pickers + serde read the catalog through the scene (a borrowed
                // pointer, valid only for this frame); also set on the control side.
                state->editor->scene.catalog = &state->assets.catalog;
                // Publish mode: the editor owns the render size (set-viewport-size); the
                // hidden SDL window's size is meaningless. Present mode tracks the window.
                if (!state->shmPublish)
                {
                    se::setViewportDesiredSize(app.renderer, app.window.width, app.window.height);
                }
                se::syncNativeGizmo(*state->editor);
                se::CameraView cam = se::sceneEditCameraView(state->editor->camera);
                const se::u32 viewWidth = se::viewportWidth(app.renderer);
                const se::u32 viewHeight = se::viewportHeight(app.renderer);
                if (viewWidth > 0 && viewHeight > 0)
                {
                    se::renderScene(app.renderer, state->editor->scene, state->assets, cam);
                    se::submitNativeGizmo(*state->editor, app.renderer, cam, viewWidth, viewHeight);
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
            if (state->control != nullptr)
            {
                se::destroyControlContext(state->control);
                state->control = nullptr;
            }
            if (state->editor != nullptr)
            {
                se::destroySceneEditContext(state->editor);
                state->editor = nullptr;
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
