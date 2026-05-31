module;

// imgui.h is a heavy C++ header, so this TU uses classic includes (no `import
// std`) — consistent with the engine's rendering/ui/scene modules.
#include <entt/entt.hpp>
#include <imgui.h>
#include <ImGuizmo.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cctype>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

export module Saffron.EditorApp;

import Saffron.Core;
import Saffron.App;
import Saffron.Window;
import Saffron.Rendering;
import Saffron.Ui;
import Saffron.Editor;
import Saffron.Control;
import Saffron.Scene;
import Saffron.Assets;

namespace se
{
    constexpr se::i32 KeyEscape = 27;  // SDLK_ESCAPE

    // A cached asset thumbnail: the GPU texture + its ImGui handle.
    struct Thumbnail
    {
        se::Ref<se::GpuTexture> texture;
        ImTextureID id = 0;
    };

    // State shared across the app lifecycle closures. The EditorContext is owned
    // by the engine (heap) so its heavy entt/json destructor stays out of this TU.
    struct EditorState
    {
        se::EditorContext* editor = nullptr;
        se::ControlContext* control = nullptr;
        se::AssetServer assets;

        // Fallback SVG type icons + the per-asset thumbnail cache (textures show their
        // image, meshes their rendered preview). All freed in onExit before the renderer.
        Thumbnail meshIcon;
        Thumbnail textureIcon;
        Thumbnail fileIcon;
        // Editor-only billboard icons for lights + cameras (drawn in the viewport).
        Thumbnail pointLightIcon;
        Thumbnail spotLightIcon;
        Thumbnail cameraIcon;
        std::unordered_map<se::u64, Thumbnail> thumbnails;
        Thumbnail eyeIcon;
        struct {
            bool open = false;
            std::string title;
            ImTextureID previewId = 0;
            se::Ref<se::GpuTexture> previewTexture;
        } viewer;
    };
}

export namespace se
{
    /// Builds the editor App (window + renderer + UI + editor/control/asset state),
    /// runs the main loop, and returns the process exit code. Takes plain title/size
    /// so the caller (main) needs no engine config types.
    auto runEditor(std::string title, u32 width, u32 height) -> int
    {
        auto state = std::make_shared<EditorState>();

        se::AppConfig config;
        config.window = se::WindowConfig{ .title = std::move(title), .width = width, .height = height };

        config.onCreate = [state](se::App& app)
        {
            state->editor = se::newEditorContext();
            state->control = se::newControlContext();
            state->assets = se::newAssetServer(se::assetPath("assets"));

            // Vendored Lucide type icons (the fallback when a real thumbnail isn't available).
            auto loadIcon = [&app](const char* file) -> Thumbnail
            {
                auto tex =
                    se::uploadSvgIcon(app.renderer, se::assetPath(file), 64, glm::vec4(0.85f));
                if (!tex)
                {
                    se::logError(tex.error());
                    return Thumbnail{};
                }
                return Thumbnail{ *tex, se::uiRegisterTexture(*tex) };
            };
            state->meshIcon = loadIcon("icons/box.svg");
            state->textureIcon = loadIcon("icons/image.svg");
            state->fileIcon = loadIcon("icons/file.svg");
            state->eyeIcon = loadIcon("icons/eye.svg");
            state->pointLightIcon = loadIcon("icons/lightbulb.svg");
            state->spotLightIcon  = loadIcon("icons/flashlight.svg");
            state->cameraIcon     = loadIcon("icons/camera.svg");

            // Best-effort thumbnail per asset: textures show their image, anything else the
            // type icon (mesh render lands in a later step). Cached so AddTexture runs once.
            std::function<ImTextureID(const se::AssetEntry&)> thumbnailFor =
                [state, &app](const se::AssetEntry& entry) -> ImTextureID
            {
                auto cached = state->thumbnails.find(entry.id.value);
                if (cached != state->thumbnails.end())
                {
                    return cached->second.id;
                }
                if (entry.type == se::AssetType::Texture)
                {
                    se::Ref<se::GpuTexture> tex = se::loadTextureAsset(state->assets, app.renderer, entry.id);
                    if (!tex)
                    {
                        return state->textureIcon.id;
                    }
                    Thumbnail thumb{ tex, se::uiRegisterTexture(tex) };
                    state->thumbnails.emplace(entry.id.value, thumb);
                    return thumb.id;
                }
                if (entry.type == se::AssetType::Mesh)
                {
                    se::Ref<se::GpuMesh> mesh = se::loadMeshAsset(state->assets, app.renderer, entry.id);
                    if (!mesh)
                    {
                        return state->meshIcon.id;
                    }
                    auto rendered =
                        se::renderMeshThumbnail(app.renderer, mesh, 128);
                    if (!rendered)
                    {
                        se::logError(rendered.error());
                        return state->meshIcon.id;
                    }
                    Thumbnail thumb{ *rendered, se::uiRegisterTexture(*rendered) };
                    state->thumbnails.emplace(entry.id.value, thumb);
                    return thumb.id;
                }
                return state->fileIcon.id;
            };
            se::registerBuiltinComponents(state->editor->registry, thumbnailFor);

            std::function<void(const se::AssetEntry&)> onView =
                [state, &app](const se::AssetEntry& entry)
            {
                if (state->viewer.previewId != 0)
                {
                    se::uiUnregisterTexture(state->viewer.previewId);
                    state->viewer.previewId = 0;
                    state->viewer.previewTexture = nullptr;
                }
                if (entry.type == se::AssetType::Mesh)
                {
                    se::Ref<se::GpuMesh> mesh = se::loadMeshAsset(state->assets, app.renderer, entry.id);
                    if (!mesh) { return; }
                    auto rendered = se::renderMeshThumbnail(app.renderer, mesh, 512);
                    if (!rendered) { se::logError(rendered.error()); return; }
                    state->viewer.previewTexture = *rendered;
                    state->viewer.previewId      = se::uiRegisterTexture(*rendered);
                }
                else if (entry.type == se::AssetType::Texture)
                {
                    auto it = state->thumbnails.find(entry.id.value);
                    se::Ref<se::GpuTexture> tex =
                        (it != state->thumbnails.end() && it->second.texture)
                            ? it->second.texture
                            : se::loadTextureAsset(state->assets, app.renderer, entry.id);
                    if (!tex) { return; }
                    state->viewer.previewTexture = tex;
                    state->viewer.previewId      = se::uiRegisterTexture(tex);
                }
                else { return; }
                state->viewer.title = entry.name;
                state->viewer.open  = true;
            };

            // Import a file into the asset catalog (no spawn), routed by extension. Used by
            // File > Import, the asset panel, and drag-and-drop.
            auto importToCatalog = [state, &app](const std::string& path)
            {
                std::string ext = std::filesystem::path(path).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext == ".png" || ext == ".jpg" || ext == ".jpeg")
                {
                    if (auto id = se::importTexture(state->assets, app.renderer, path); !id)
                    {
                        se::logError(id.error());
                    }
                    return;
                }
                if (auto model = se::importModel(state->assets, app.renderer, path); !model)
                {
                    se::logError(model.error());
                }
            };
            state->editor->onImport = importToCatalog;
            // Create > Cube imports the bundled cube into the catalog and spawns an entity.
            state->editor->onCreateCube = [state, &app]()
            {
                auto cube =
                    se::importModel(state->assets, app.renderer, se::assetPath("models/cube.gltf"));
                if (!cube)
                {
                    se::logError(cube.error());
                    return;
                }
                se::setSelection(*state->editor, se::spawnModel(state->editor->scene, "Cube", *cube));
            };
            state->editor->onSaveProject = [state](const std::string& path)
            {
                if (auto result =
                        se::saveProject(state->assets, state->editor->registry, state->editor->scene, path); !result)
                {
                    se::logError(result.error());
                }
            };
            state->editor->onLoadProject = [state, &app](const std::string& path)
            {
                if (auto result = se::loadProject(state->assets, app.renderer,
                        state->editor->registry, state->editor->scene, path); !result)
                {
                    se::logError(result.error());
                    return;
                }
                // The catalog changed; drop stale thumbnails so they re-generate.
                for (auto& [id, thumb] : state->thumbnails)
                {
                    se::uiUnregisterTexture(thumb.id);
                }
                state->thumbnails.clear();
                if (state->viewer.previewId != 0)
                {
                    se::uiUnregisterTexture(state->viewer.previewId);
                    state->viewer.previewId      = 0;
                    state->viewer.previewTexture = nullptr;
                    state->viewer.open           = false;
                }
            };
            app.window.onFileDropped.subscribe([importToCatalog](std::string path)
            {
                importToCatalog(path);
                return false;
            });

            // Auto-load project.json from the working directory if it exists; otherwise seed
            // a default scene with a cube so a fresh checkout starts with something visible.
            constexpr const char* defaultProject = "project.json";
            if (std::filesystem::exists(defaultProject))
            {
                if (auto result = se::loadProject(state->assets, app.renderer,
                        state->editor->registry, state->editor->scene, defaultProject))
                {
                    state->editor->scenePath = defaultProject;
                }
                else
                {
                    se::logError(result.error());
                }
            }
            else
            {
                auto cube = se::importModel(state->assets, app.renderer, se::assetPath("models/cube.gltf"));
                if (cube) { se::spawnModel(state->editor->scene, "Cube", *cube); }
                else      { se::logError(cube.error()); }
            }

            se::Layer layer;
            layer.name = "EditorLayer";
            layer.onUpdate = [state, &app](se::TimeSpan)
            {
                if (state->control != nullptr)
                {
                    se::pollControl(*state->control, app.window, app.renderer, *state->editor, state->assets);
                }
            };
            // The scene renders through the editor (viewport) camera, which is driven by
            // ImGui input — valid only during onUi — so the scene draw + gizmo live here.
            // renderScene records closures the renderer replays in endFrame.
            layer.onUi = [state, &app, thumbnailFor, onView]()
            {
                // The inspector pickers + asset panel read the catalog through the scene
                // (a borrowed pointer, valid only for this frame).
                state->editor->scene.catalog = &state->assets.catalog;

                se::drawEditorMenuBar(*state->editor);
                se::viewportPanel(app.ui, app.renderer);

                se::updateEditorCamera(state->editor->camera, se::viewportHovered(app.ui),
                                       ImGui::GetIO().DeltaTime);
                se::CameraView cam = se::editorCameraView(state->editor->camera);
                const se::u32 vw = se::viewportWidth(app.renderer);
                const se::u32 vh = se::viewportHeight(app.renderer);
                if (vw > 0 && vh > 0)
                {
                    se::renderScene(app.renderer, state->editor->scene, state->assets, cam);

                    // Gizmo: same camera, but the UN-flipped projection (the renderer keeps
                    // the Vulkan Y-flip local) so it is not mirrored.
                    glm::mat4 proj = se::cameraProjection(cam, static_cast<float>(vw) / static_cast<float>(vh));
                    se::drawGizmo(*state->editor, cam.view, proj,
                                  se::viewportContentPos(app.ui), se::viewportContentSize(app.ui),
                                  se::viewportHovered(app.ui));

                    // Billboard icons for lights + cameras (drawn in the Viewport window).
                    const float aspect = static_cast<float>(vw) / static_cast<float>(vh);
                    ImGui::Begin("Viewport");
                    const se::Entity billboardHit = se::drawEditorBillboards(
                        *state->editor, cam, aspect,
                        se::viewportContentPos(app.ui), se::viewportContentSize(app.ui),
                        state->pointLightIcon.id, state->spotLightIcon.id, state->cameraIcon.id);
                    ImGui::End();
                    if (billboardHit.handle != entt::null)
                    {
                        se::setSelection(*state->editor, billboardHit);
                    }

                    // Left-click in empty viewport space ray-picks an entity (or clears the
                    // selection). Skipped when gizmo is active or a billboard was clicked.
                    if (billboardHit.handle == entt::null &&
                        se::viewportHovered(app.ui) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                        !ImGuizmo::IsOver() && !ImGuizmo::IsUsing())
                    {
                        const ImVec2 origin = se::viewportContentPos(app.ui);
                        const ImVec2 size = se::viewportContentSize(app.ui);
                        const ImVec2 mouse = ImGui::GetIO().MousePos;
                        const glm::vec2 ndc{ (mouse.x - origin.x) / size.x * 2.0f - 1.0f,
                                             (mouse.y - origin.y) / size.y * 2.0f - 1.0f };
                        se::setSelection(*state->editor,
                                         se::pickEntity(state->editor->scene, state->assets, app.renderer, cam, ndc));
                    }
                }

                se::hierarchyPanel(*state->editor);
                se::assetCatalogPanel(*state->editor, &state->assets.catalog, thumbnailFor,
                                      onView, state->eyeIcon.id);
                {
                    const bool wasOpen = state->viewer.open;
                    se::viewerPanel(state->viewer.open, state->viewer.title.c_str(),
                                    state->viewer.previewId);
                    if (wasOpen && !state->viewer.open)
                    {
                        se::uiUnregisterTexture(state->viewer.previewId);
                        state->viewer.previewId      = 0;
                        state->viewer.previewTexture = nullptr;
                    }
                }

                // Render stats overlay — dockable panel.
                if (ImGui::Begin("Render Stats"))
                {
                    const se::RenderStats stats = se::renderStats(app.renderer);
                    const ImGuiIO& io = ImGui::GetIO();
                    ImGui::Text("%.1f fps   %.2f ms", io.Framerate, 1000.0f / io.Framerate);
                    ImGui::Separator();
                    ImGui::Text("Draw calls  %u", stats.drawCalls);
                    ImGui::Text("Batches     %u", stats.batches);
                    ImGui::Text("Instances   %u", stats.instances);
                }
                ImGui::End();

                // Numeric/data fields read better in a monospace font.
                ImGui::PushFont(se::uiMonoFont(app.ui));
                se::inspectorPanel(*state->editor);
                ImGui::PopFont();
            };
            // Post-process is an app-authored render-graph pass: add it when enabled.
            layer.onRenderGraph = [&app](se::RenderGraph& graph)
            {
                if (se::postProcessEnabled(app.renderer))
                {
                    se::addTonemapPass(app.renderer, graph);
                }
            };
            se::attachLayer(app, std::move(layer));

            app.window.onKeyPressed.subscribe([&app](se::i32 key, bool isRepeat)
            {
                static_cast<void>(isRepeat);
                if (key == KeyEscape)
                {
                    app.window.shouldClose = true;
                }
                return false;
            });
        };

        config.onExit = [state](se::App&)
        {
            if (state->control != nullptr)
            {
                se::destroyControlContext(state->control);
                state->control = nullptr;
            }
            if (state->editor != nullptr)
            {
                se::destroyEditorContext(state->editor);
                state->editor = nullptr;
            }
            // Drop every GPU Ref this client holds before destroyRenderer frees the
            // device/allocator — otherwise the cached meshes/textures and the pipeline
            // would be freed too late (use-after-free). Unregister ImGui textures first
            // (RemoveTexture needs the ImGui Vulkan backend, torn down after onExit).
            auto freeThumbnail = [](Thumbnail& thumb)
            {
                se::uiUnregisterTexture(thumb.id);
                thumb = Thumbnail{};
            };
            for (auto& [id, thumb] : state->thumbnails)
            {
                se::uiUnregisterTexture(thumb.id);
            }
            state->thumbnails.clear();
            freeThumbnail(state->meshIcon);
            freeThumbnail(state->textureIcon);
            freeThumbnail(state->fileIcon);
            freeThumbnail(state->eyeIcon);
            freeThumbnail(state->pointLightIcon);
            freeThumbnail(state->spotLightIcon);
            freeThumbnail(state->cameraIcon);
            if (state->viewer.previewId != 0)
            {
                se::uiUnregisterTexture(state->viewer.previewId);
                state->viewer.previewId      = 0;
                state->viewer.previewTexture = nullptr;
            }

            state->assets.meshRefByUuid.clear();
            state->assets.textureRefByUuid.clear();
        };

        return se::run(std::move(config));
    }
}
