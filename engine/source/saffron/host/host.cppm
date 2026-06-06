module;

// SDL3 + glm are header-heavy C++ headers, so this TU uses classic includes (no
// `import std`) — consistent with the engine's rendering/scene modules.
#include <entt/entt.hpp>
#include <SDL3/SDL.h>
#include <X11/Xlib.h>
// Xlib's None macro collides with the scoped-enum None members used below.
#undef None
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
        bool flyActive = false;          // RMB-fly: focus taken, keyboard grabbed, pointer locked
        glm::vec2 flyLookDelta{ 0.0f };  // accumulated xrel/yrel, drained each onUpdate
    };

    // The X11 display/window behind the SDL window, plus the parent it was reparented
    // into. embedded is false when running standalone (parent is the root window).
    struct X11WindowInfo
    {
        Display* display = nullptr;
        ::Window window = 0;
        ::Window parent = 0;
        bool embedded = false;
    };

    auto x11WindowInfo(SDL_Window* window) -> X11WindowInfo
    {
        X11WindowInfo info;
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        info.display =
            static_cast<Display*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
        info.window = static_cast<::Window>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
        if (info.display == nullptr || info.window == 0)
        {
            return info;
        }
        ::Window root = 0;
        ::Window* children = nullptr;
        unsigned int childCount = 0;
        if (XQueryTree(info.display, info.window, &root, &info.parent, &children, &childCount) != 0)
        {
            if (children != nullptr)
            {
                XFree(children);
            }
            info.embedded = info.parent != 0 && info.parent != root;
        }
        return info;
    }

    // Release the RMB-fly grabs + pointer lock, clear accumulated look, and hand the
    // X11 input focus back to the Tauri parent. Idempotent (safe when not flying) —
    // used on RMB-up, focus loss, ESC, and teardown.
    void endSceneEditFly(HostState& state, SDL_Window* window)
    {
        if (!state.flyActive)
        {
            return;
        }
        state.flyActive = false;
        state.flyLookDelta = glm::vec2{ 0.0f };
        SDL_SetWindowRelativeMouseMode(window, false);
        SDL_SetWindowMouseGrab(window, false);
        SDL_SetWindowKeyboardGrab(window, false);
        SDL_ShowCursor();
        const X11WindowInfo x11 = x11WindowInfo(window);
        if (x11.embedded)
        {
            XSetInputFocus(x11.display, x11.parent, RevertToParent, CurrentTime);
            XFlush(x11.display);
        }
    }

    enum class BillboardKind
    {
        None,
        PointLight,
        SpotLight,
        Camera,
    };

    // The overlay-gizmo + billboard geometry builders + the SDL pointer handler. These
    // touch Rendering (OverlayVertex / submitOverlay / Renderer) + Assets (pickEntity) +
    // SDL, so they stay in this TU; the pure-math hit-test/drag live in Saffron.SceneEdit.

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
        const glm::vec2 n = glm::vec2{ -delta.y, delta.x } / len * (thickness * 0.5f);
        const glm::vec2 a0 = se::pixelToNdc(aPx + n, width, height);
        const glm::vec2 a1 = se::pixelToNdc(aPx - n, width, height);
        const glm::vec2 b0 = se::pixelToNdc(bPx + n, width, height);
        const glm::vec2 b1 = se::pixelToNdc(bPx - n, width, height);
        addTriangle(vertices, a0, b0, b1, color);
        addTriangle(vertices, a0, b1, a1, color);
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
        if (editor.nativeGizmo.mode == se::NativeGizmoMode::Translate)
        {
            const std::array<std::pair<se::NativeGizmoHandle, glm::vec3>, 3> planes{
                std::pair{ se::NativeGizmoHandle::XY, axes[0] * axisLen * 0.26f + axes[1] * axisLen * 0.26f },
                std::pair{ se::NativeGizmoHandle::YZ, axes[1] * axisLen * 0.26f + axes[2] * axisLen * 0.26f },
                std::pair{ se::NativeGizmoHandle::XZ, axes[0] * axisLen * 0.26f + axes[2] * axisLen * 0.26f }
            };
            for (const auto& [handle, offset] : planes)
            {
                const se::GizmoProjection center = se::viewportProject(cam, width, height, position + offset);
                if (center.visible)
                {
                    addBox(vertices, center.pixel, 18.0f, se::axisColor(handle, editor.nativeGizmo), width, height);
                }
            }
        }
        else if (editor.nativeGizmo.mode == se::NativeGizmoMode::Rotate)
        {
            constexpr se::u32 segments = 96;
            const se::f32 radius = axisLen * 0.72f;
            for (se::u32 axis = 0; axis < 3; axis = axis + 1)
            {
                const glm::vec3 n = axes[axis];
                glm::vec3 a = glm::normalize(glm::cross(n, glm::vec3{ 0.0f, 1.0f, 0.0f }));
                if (glm::length(a) < 0.001f)
                {
                    a = glm::normalize(glm::cross(n, glm::vec3{ 1.0f, 0.0f, 0.0f }));
                }
                const glm::vec3 b = glm::normalize(glm::cross(n, a));
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

    auto pickSceneEditBillboard(se::SceneEditContext& editor, const se::CameraView& cam, se::u32 width, se::u32 height,
                                glm::vec2 mouse) -> se::Entity
    {
        if (width == 0 || height == 0)
        {
            return se::Entity{ entt::null };
        }
        constexpr se::f32 half = 14.0f;
        se::Entity hit{ entt::null };
        se::f32 best = half;
        se::forEach<se::TransformComponent>(editor.scene,
                                            [&](se::Entity e, se::TransformComponent&)
                                            {
                                                if (billboardKind(editor.scene, e) == BillboardKind::None)
                                                {
                                                    return;
                                                }
                                                const se::GizmoProjection p = se::viewportProject(
                                                    cam, width, height, se::worldTranslation(editor.scene, e));
                                                if (!p.visible)
                                                {
                                                    return;
                                                }
                                                const glm::vec2 d = glm::abs(mouse - p.pixel);
                                                if (d.x <= half && d.y <= half)
                                                {
                                                    const se::f32 dist = glm::length(mouse - p.pixel);
                                                    if (dist <= best)
                                                    {
                                                        best = dist;
                                                        hit = e;
                                                    }
                                                }
                                            });
        return hit;
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

    // SDL pointer → overlay-gizmo hover/drag, or (on a miss) a mesh ray-pick that updates
    // the selection. Wired to the window event sinks under the native viewport host.
    auto handleNativeGizmoPointer(se::SceneEditContext& editor, se::AssetServer& assets, se::Renderer& renderer,
                                  const se::CameraView& cam, const SDL_Event& event) -> bool
    {
        if (renderer.window == nullptr || renderer.window->width == 0 || renderer.window->height == 0)
        {
            return false;
        }
        se::NativeGizmoState& gizmo = editor.nativeGizmo;
        const se::u32 width = renderer.window->width;
        const se::u32 height = renderer.window->height;
        if (event.type == SDL_EVENT_MOUSE_MOTION)
        {
            const glm::vec2 mouse{ event.motion.x, event.motion.y };
            if (gizmo.dragging)
            {
                se::applyNativeGizmoDrag(editor, cam, width, height, mouse);
                return true;
            }
            gizmo.hovered = se::hitNativeGizmo(editor, cam, width, height, mouse);
            return gizmo.hovered != se::NativeGizmoHandle::None;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT)
        {
            const glm::vec2 mouse{ event.button.x, event.button.y };
            gizmo.hovered = se::hitNativeGizmo(editor, cam, width, height, mouse);
            if (gizmo.hovered != se::NativeGizmoHandle::None)
            {
                gizmo.active = gizmo.hovered;
                gizmo.dragging = true;
                gizmo.startMouse = mouse;
                gizmo.target = editor.selected;
                se::snapshotNativeGizmoStart(editor, editor.selected);
                return true;
            }
            const se::Entity billboard = pickSceneEditBillboard(editor, cam, width, height, mouse);
            if (billboard.handle != entt::null)
            {
                se::setSelection(editor, billboard);
                return true;
            }
            const glm::vec2 ndc{ mouse.x / static_cast<se::f32>(width) * 2.0f - 1.0f,
                                 mouse.y / static_cast<se::f32>(height) * 2.0f - 1.0f };
            se::setSelection(editor, se::pickEntity(editor.scene, assets, renderer, cam, ndc));
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT)
        {
            gizmo.dragging = false;
            gizmo.active = se::NativeGizmoHandle::None;
            gizmo.target = se::Entity{ entt::null };
            return true;
        }
        return false;
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
            // panels), reparented under the Tauri window and driven over the control plane.
            se::setPresentViewportOnly(app.renderer, true);

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

            // Raw SDL input. Mouse events reach the reparented child by cursor position
            // (overlay-gizmo hover/drag + mesh ray-pick on LMB); keyboard does not, because
            // focus stays on the Tauri webview. So while RMB is held we take the X11 input
            // focus, grab the keyboard, and lock the pointer (mouse grab + relative mode),
            // releasing everything on RMB-up or focus loss. Gizmo/pick is suppressed while
            // flying.
            app.window.eventSinks.push_back(
                [state, &app](const SDL_Event& event)
                {
                    SDL_Window* win = app.window.handle;
                    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT)
                    {
                        state->flyActive = true;
                        state->flyLookDelta = glm::vec2{ 0.0f };
                        // A reparented X11 child never holds input focus (it stays on the
                        // Tauri toplevel), and SDL only applies grabs and routes relative
                        // motion to a focused window — take the focus for the duration of the
                        // fly. The grabs + pointer lock are asserted in onUpdate, once SDL has
                        // processed the focus change.
                        const X11WindowInfo x11 = x11WindowInfo(win);
                        if (x11.embedded)
                        {
                            XSetInputFocus(x11.display, x11.window, RevertToParent, CurrentTime);
                            XFlush(x11.display);
                        }
                        return;
                    }
                    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_RIGHT)
                    {
                        endSceneEditFly(*state, win);
                        return;
                    }
                    if (event.type == SDL_EVENT_MOUSE_MOTION && state->flyActive)
                    {
                        state->flyLookDelta.x += event.motion.xrel;
                        state->flyLookDelta.y += event.motion.yrel;
                        return;
                    }
                    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST || event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE)
                    {
                        endSceneEditFly(*state, win);
                        return;
                    }
                    if (!state->flyActive)
                    {
                        se::syncNativeGizmo(*state->editor);
                        const se::CameraView cam = se::sceneEditCameraView(state->editor->camera);
                        static_cast<void>(
                            handleNativeGizmoPointer(*state->editor, state->assets, app.renderer, cam, event));
                    }
                });

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
                // Fly-cam: drive the editor eye from raw SDL input while RMB is held.
                se::SceneEditCameraInput input;
                input.active = state->flyActive;
                if (state->flyActive)
                {
                    // (Re)assert the grabs + pointer lock each frame: SDL only engages
                    // relative mouse once it has processed the focus change taken on
                    // RMB-down, which lands a frame later for a reparented child. All are
                    // idempotent. SDL_HideCursor covers compositors that show the system
                    // cursor under relative mode.
                    SDL_Window* win = app.window.handle;
                    SDL_SetWindowKeyboardGrab(win, true);
                    SDL_SetWindowMouseGrab(win, true);
                    SDL_SetWindowRelativeMouseMode(win, true);
                    SDL_HideCursor();

                    input.lookDelta = state->flyLookDelta;
                    const bool* keys = SDL_GetKeyboardState(nullptr);
                    input.forward = keys[SDL_SCANCODE_W];
                    input.back = keys[SDL_SCANCODE_S];
                    input.left = keys[SDL_SCANCODE_A];
                    input.right = keys[SDL_SCANCODE_D];
                    input.up = keys[SDL_SCANCODE_SPACE];
                    input.down = keys[SDL_SCANCODE_LSHIFT];
                }
                se::updateSceneEditCamera(state->editor->camera, input, dt.seconds);
                state->flyLookDelta = glm::vec2{ 0.0f };

                if (state->flyActive)
                {
                    // Pin the cursor to the window center so it cannot escape the viewport.
                    // Relative mode reports raw deltas and ignores this warp, so it adds no
                    // drift; it covers compositors (XWayland) where relative mode does not
                    // freeze the OS pointer.
                    SDL_Window* win = app.window.handle;
                    int w = 0;
                    int h = 0;
                    SDL_GetWindowSize(win, &w, &h);
                    SDL_WarpMouseInWindow(win, static_cast<f32>(w) * 0.5f, static_cast<f32>(h) * 0.5f);
                }
            };
            // Present-only host: the editor is the headless native-viewport host the Tauri
            // app spawns + reparents. There are no engine panels — the scene renders through
            // the editor (fly-cam) camera into the swapchain, with the gizmo handles + entity
            // billboards drawn by the engine overlay pass. The full editor UI is the React/
            // Tauri frontend, which drives this host over the control plane.
            layer.onUi = [state, &app]()
            {
                // The pickers + serde read the catalog through the scene (a borrowed
                // pointer, valid only for this frame); also set on the control side.
                state->editor->scene.catalog = &state->assets.catalog;
                se::setViewportDesiredSize(app.renderer, app.window.width, app.window.height);
                se::syncNativeGizmo(*state->editor);
                se::CameraView cam = se::sceneEditCameraView(state->editor->camera);
                if (app.window.width > 0 && app.window.height > 0)
                {
                    se::renderScene(app.renderer, state->editor->scene, state->assets, cam);
                    se::submitNativeGizmo(*state->editor, app.renderer, cam, app.window.width, app.window.height);
                }
            };
            se::attachLayer(app, std::move(layer));

            app.window.onKeyPressed.subscribe(
                [state, &app](se::i32 key, bool isRepeat)
                {
                    static_cast<void>(isRepeat);
                    if (key == KeyEscape)
                    {
                        // While flying, ESC ends fly (releasing the grab) rather than
                        // quitting the host; otherwise it closes the standalone window.
                        if (state->flyActive)
                        {
                            endSceneEditFly(*state, app.window.handle);
                        }
                        else
                        {
                            app.window.shouldClose = true;
                        }
                    }
                    return false;
                });
        };

        config.onExit = [state](se::App& app)
        {
            endSceneEditFly(*state, app.window.handle);
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
