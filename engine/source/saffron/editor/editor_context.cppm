module;

// The editor's shared types + public surface. ImGui/ImGuizmo/entt/glm are header-heavy,
// so this partition uses classic includes (no `import std`), like the rest of the editor.
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <imgui.h>
#include <ImGuizmo.h>

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

        // Imports a file into the asset catalog (File > Import, drag-and-drop, the asset
        // panel). The editor has no renderer/assets, so the client routes by extension.
        std::function<void(const std::string&)> onImport;
        // Save/load the whole project (asset catalog + scene); delegated for the same reason.
        std::function<void(const std::string&)> onSaveProject;
        std::function<void(const std::string&)> onLoadProject;
        std::string importPath;  // the Import dialog's text buffer

        // Spawns the bundled cube mesh (Create > Cube); delegated because the editor has
        // no AssetServer to resolve/upload the mesh itself.
        std::function<void()> onCreateCube;
        ImGuizmo::OPERATION gizmoOp = ImGuizmo::TRANSLATE;  // W/E/R cycle translate/rotate/scale
    };

    // The payload dragged from an asset tile onto a component picker field.
    struct AssetDragPayload
    {
        u64 id = 0;
        AssetType type = AssetType::Mesh;
    };

    void setSelection(EditorContext& ctx, Entity entity);

    // A combo that picks a catalog asset of `type` into `target` (the component's Uuid),
    // showing each asset's thumbnail + name. Reads the catalog through scene.catalog
    // (borrowed; tolerates null). `thumbnailFor` maps an asset to an ImGui texture (0 = none).
    void drawAssetPicker(Scene& scene, AssetType type, const char* label, Uuid& target,
                         const std::function<ImTextureID(const AssetEntry&)>& thumbnailFor);

    // Concrete built-in component registration — the imgui draw lambdas + json serde
    // live here (the one place with both imgui and json). `thumbnailFor` is captured by
    // the Mesh/Material draws so their pickers can show asset thumbnails.
    void registerBuiltinComponents(ComponentRegistry& reg,
                                   std::function<ImTextureID(const AssetEntry&)> thumbnailFor);

    // Heap-owned so EditorContext's heavy destructor (entt/json) is instantiated
    // here, not in the client TU. The editor holds only the pointer.
    auto newEditorContext() -> EditorContext*;
    void destroyEditorContext(EditorContext* ctx);

    void hierarchyPanel(EditorContext& ctx);
    // Registry-driven: iterates ComponentTraits rows, no per-component switch.
    void inspectorPanel(EditorContext& ctx);

    // The shared "Import Asset" modal body (a path field → ctx.onImport). Drawn by the
    // asset panel; opened from there or from File ▸ Import via OpenPopup("Import Asset").
    void drawImportModal(EditorContext& ctx);

    // The project asset catalog: a tile grid of imported assets (thumbnail + editable
    // name). Import via the button/modal or drag-and-drop; drag a tile onto a component
    // picker to assign it. `catalog` is mutable so names can be edited in place.
    void assetCatalogPanel(EditorContext& ctx, AssetCatalog* catalog,
                           const std::function<ImTextureID(const AssetEntry&)>& thumbnailFor,
                           const std::function<void(const AssetEntry&)>& onView = nullptr,
                           ImTextureID eyeIcon = 0);

    /// Floating preview window for a single catalog asset. Caller owns `previewId`
    /// and must unregister it when `open` transitions from true to false.
    void viewerPanel(bool& open, const char* title, ImTextureID previewId);

    void drawEditorMenuBar(EditorContext& ctx);

    // The editor camera's forward (world space) from its yaw/pitch.
    auto editorCameraForward(const EditorCamera& camera) -> glm::vec3;

    // The editor camera as a Scene CameraView (view + projection params), so renderScene
    // and the gizmo draw from the same eye.
    auto editorCameraView(const EditorCamera& camera) -> CameraView;

    // Fly the editor camera while RMB is held over the viewport: mouse look + WASD move,
    // Shift up / Ctrl down (world Y). Reads ImGui input, so call from onUi each frame.
    void updateEditorCamera(EditorCamera& camera, bool viewportHovered, f32 dt);

    // In-viewport translate/rotate/scale gizmo for the selected entity. `proj` MUST be
    // the un-flipped projection (the Vulkan Y-flip stays local to the renderer) or the
    // gizmo mirrors vertically. Drawn into the "Viewport" window's draw list so it clips
    // to the panel and takes mouse input there. W/E/R cycle the op (not while flying the
    // camera with RMB). `imagePos`/`imageSize` are the viewport image's screen rect.
    void drawGizmo(EditorContext& ctx, const glm::mat4& view, const glm::mat4& proj,
                   ImVec2 imagePos, ImVec2 imageSize, bool hovered);

    // Draws 2D billboard icons for PointLight, SpotLight, and Camera entities in the
    // "Viewport" draw list. Returns the entity whose icon was clicked (entt::null = none).
    auto drawEditorBillboards(EditorContext& ctx, const CameraView& cam, float aspect,
                              ImVec2 vpPos, ImVec2 vpSize,
                              ImTextureID pointLightIcon, ImTextureID spotLightIcon,
                              ImTextureID cameraIcon) -> Entity;
}
