module;

// The editor's shared types + public surface. ImGui/ImGuizmo/entt/glm are header-heavy,
// so this partition uses classic includes (no `import std`), like the rest of the editor.
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <imgui.h>
#include <ImGuizmo.h>

#include <array>
#include <functional>
#include <string>

export module Saffron.Editor:Context;

import Saffron.Core;
import Saffron.Signal;
import Saffron.Scene;

export namespace se
{
    // The viewport's own fly-camera (the scene-view eye, distinct from any ECS
    // CameraComponent / game camera). Hold RMB over the viewport to look + WASD to move,
    // Shift up / Ctrl down. yaw/pitch in degrees; at yaw 0 the camera looks down -Z.
    struct EditorCamera
    {
        glm::vec3 position{ 3.0f, 2.5f, 4.0f };
        f32 yaw = -37.0f;
        f32 pitch = -29.0f;
        f32 fov = 45.0f;
        f32 nearPlane = 0.1f;
        f32 farPlane = 100.0f;
        f32 moveSpeed = 6.0f;   // units/second
        f32 lookSpeed = 0.12f;  // degrees/pixel
        bool controlling = false;  // latched while RMB is held (so a drag can leave the rect)
    };

    // Backend-neutral gizmo op + reference space (no ImGuizmo/imgui types, so the control
    // TU can read/write them); editor_gizmo.cpp maps these to ImGuizmo at the call site.
    enum class GizmoOp { Translate, Rotate, Scale };
    enum class GizmoSpace { World, Local };

    auto gizmoOpName(GizmoOp op) -> const char*;            // "translate"|"rotate"|"scale"
    auto gizmoOpFromName(const std::string& name) -> GizmoOp;
    auto gizmoSpaceName(GizmoSpace space) -> const char*;   // "world"|"local"
    auto gizmoSpaceFromName(const std::string& name) -> GizmoSpace;

    // The engine-rendered (overlay) gizmo. mode/space are driven FROM the backend-neutral
    // GizmoOp/GizmoSpace (the single source) — mapped each frame — so the ImGuizmo path
    // (editor-old) and the native overlay path stay in sync. The remaining fields are the
    // overlay's own hover/drag interaction state.
    enum class NativeGizmoMode { Translate, Rotate, Scale };
    enum class NativeGizmoSpace { World, Local };
    enum class NativeGizmoHandle { None, X, Y, Z, XY, YZ, XZ, Screen, Uniform };

    struct NativeGizmoState
    {
        NativeGizmoMode mode = NativeGizmoMode::Translate;
        NativeGizmoSpace space = NativeGizmoSpace::World;
        NativeGizmoHandle hovered = NativeGizmoHandle::None;
        NativeGizmoHandle active = NativeGizmoHandle::None;
        bool dragging = false;
        glm::vec2 startMouse{ 0.0f };
        glm::vec3 startTranslation{ 0.0f };
        glm::vec3 startRotation{ 0.0f };
        glm::vec3 startScale{ 1.0f };
        Entity target{ entt::null };
    };

    // The editor's mutable state: the scene being edited, the component registry
    // that drives every panel, and the current selection (broadcast as a signal).
    struct EditorContext
    {
        Scene scene;
        ComponentRegistry registry;
        Entity selected{ entt::null };
        SubscriberList<Entity> onSelectionChanged;
        std::string scenePath;
        EditorCamera camera;
        u64 sceneVersion = 0;       // bumped by add/copy/destroy-entity + load (control-plane diff poll)
        u64 selectionVersion = 0;   // bumped on every selection change

        GizmoOp gizmoOp = GizmoOp::Translate;       // W/E/R cycle translate/rotate/scale
        GizmoSpace gizmoSpace = GizmoSpace::World;  // gizmo reference space (world/local)
        NativeGizmoState nativeGizmo;               // overlay-gizmo hover/drag state (mode/space synced from above)
    };

    // The payload dragged from an asset tile onto a component picker field.
    struct AssetDragPayload
    {
        u64 id = 0;
        AssetType type = AssetType::Mesh;
    };

    void setSelection(EditorContext& ctx, Entity entity);

    // Concrete built-in component registration — the json serde lambdas live here. The
    // present-only native-viewport host renders no inspector, so the per-component
    // drawInspector is a no-op; the registry exists for its serialize/deserialize.
    void registerBuiltinComponents(ComponentRegistry& reg);

    // Heap-owned so EditorContext's heavy destructor (entt/json) is instantiated
    // here, not in the client TU. The editor holds only the pointer.
    auto newEditorContext() -> EditorContext*;
    void destroyEditorContext(EditorContext* ctx);

    // The editor camera's forward (world space) from its yaw/pitch.
    auto editorCameraForward(const EditorCamera& camera) -> glm::vec3;

    // The editor camera as a Scene CameraView (view + projection params), so renderScene
    // and the gizmo draw from the same eye.
    auto editorCameraView(const EditorCamera& camera) -> CameraView;

    // Fly the editor camera while RMB is held over the viewport: mouse look + WASD move,
    // Shift up / Ctrl down (world Y). Reads ImGui input, so call from onUi each frame.
    void updateEditorCamera(EditorCamera& camera, bool viewportHovered, f32 dt);

    // Native (overlay) gizmo math — pure glm + Scene types, no Rendering. Shared by the
    // SDL event sink (editor app) and the gizmo-pointer control command so both drive one
    // hit-test/drag implementation.

    // A world point projected to the viewport: pixel + NDC + whether it is on-screen.
    struct GizmoProjection
    {
        glm::vec2 pixel{ 0.0f };
        glm::vec2 ndc{ 0.0f };
        bool visible = false;
    };

    // Projects a world point through the camera to viewport pixels (top-left origin) + NDC.
    auto viewportProject(const CameraView& cam, u32 width, u32 height, glm::vec3 world) -> GizmoProjection;
    // Viewport pixel (top-left origin) to clip-space NDC.
    auto pixelToNdc(glm::vec2 p, u32 width, u32 height) -> glm::vec2;
    // The camera's world position from its view matrix.
    auto cameraPosition(const CameraView& cam) -> glm::vec3;
    // Distance from p to segment [a,b], all in pixels.
    auto pointSegmentDistance(glm::vec2 p, glm::vec2 a, glm::vec2 b) -> f32;
    // The display color for a gizmo handle (axis-tinted; highlighted when hovered/active).
    auto axisColor(NativeGizmoHandle handle, const NativeGizmoState& gizmo) -> glm::vec4;
    // The gizmo's X/Y/Z basis: world identity, or the transform's rotated basis in Local space.
    auto gizmoAxes(const TransformComponent& transform, NativeGizmoSpace space) -> std::array<glm::vec3, 3>;
    // The world-space axis for a single-axis handle (zero for plane/screen/uniform handles).
    auto handleAxis(NativeGizmoHandle handle, const std::array<glm::vec3, 3>& axes) -> glm::vec3;
    // Hit-tests the selected entity's gizmo at `mouse` (viewport pixels) for the active mode/space.
    auto hitNativeGizmo(EditorContext& editor, const CameraView& cam, u32 width, u32 height, glm::vec2 mouse)
        -> NativeGizmoHandle;
    // Applies an in-progress gizmo drag, writing the dragged entity's TransformComponent.
    void applyNativeGizmoDrag(EditorContext& editor, const CameraView& cam, u32 width, u32 height, glm::vec2 mouse);

    // Mirrors the backend-neutral GizmoOp/GizmoSpace (the single source) onto nativeGizmo.mode/.space.
    void syncNativeGizmo(EditorContext& ctx);
}
