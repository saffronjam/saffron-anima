// imgui.h is a heavy C++ header, so this TU uses classic includes (no `import
// std`) — consistent with the engine's rendering/ui/scene modules.
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

import Saffron.Core;
import Saffron.App;
import Saffron.Window;
import Saffron.Rendering;
import Saffron.Ui;
import Saffron.Editor;
import Saffron.Control;
import Saffron.Scene;
import Saffron.Assets;

namespace
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
        se::Ref<se::Pipeline> meshPipeline;

        // Fallback SVG type icons + the per-asset thumbnail cache (textures show their
        // image, meshes their rendered preview). All freed in onExit before the renderer.
        Thumbnail meshIcon;
        Thumbnail textureIcon;
        Thumbnail fileIcon;
        std::unordered_map<se::u64, Thumbnail> thumbnails;
    };
}

int main()
{
    auto state = std::make_shared<EditorState>();

    se::AppConfig config;
    config.window = se::WindowConfig{ .title = "Saffron Editor", .width = 1600, .height = 900 };

    config.onCreate = [state](se::App& app)
    {
        state->editor = se::newEditorContext();
        state->control = se::newControlContext();
        state->assets = se::newAssetServer(se::assetPath("assets"));

        // Vendored Lucide type icons (the fallback when a real thumbnail isn't available).
        auto loadIcon = [&app](const char* file) -> Thumbnail
        {
            std::expected<se::Ref<se::GpuTexture>, std::string> tex =
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
                std::expected<se::Ref<se::GpuTexture>, std::string> rendered =
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

        std::expected<se::Ref<se::Pipeline>, std::string> pipeline = se::newMeshPipeline(app.renderer, "shaders/mesh.spv");
        if (!pipeline)
        {
            se::logError(pipeline.error());
        }
        else
        {
            state->meshPipeline = *pipeline;
            std::expected<se::ImportResult, std::string> cube =
                se::importModel(state->assets, app.renderer, se::assetPath("models/cube.gltf"));
            if (cube)
            {
                se::spawnModel(state->editor->scene, "Cube", *cube);
            }
            else
            {
                se::logError(cube.error());
            }
        }

        // Import a file into the asset catalog (no spawn), routed by extension. Used by
        // File > Import, the asset panel, and drag-and-drop.
        auto importToCatalog = [state, &app](const std::string& path)
        {
            std::string ext = std::filesystem::path(path).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg")
            {
                if (std::expected<se::Uuid, std::string> id = se::importTexture(state->assets, app.renderer, path); !id)
                {
                    se::logError(id.error());
                }
                return;
            }
            if (std::expected<se::ImportResult, std::string> model = se::importModel(state->assets, app.renderer, path); !model)
            {
                se::logError(model.error());
            }
        };
        state->editor->onImport = importToCatalog;
        // Create > Cube imports the bundled cube into the catalog and spawns an entity.
        state->editor->onCreateCube = [state, &app]()
        {
            std::expected<se::ImportResult, std::string> cube =
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
            if (std::expected<void, std::string> result =
                    se::saveProject(state->assets, state->editor->registry, state->editor->scene, path); !result)
            {
                se::logError(result.error());
            }
        };
        state->editor->onLoadProject = [state, &app](const std::string& path)
        {
            if (std::expected<void, std::string> result = se::loadProject(state->assets, app.renderer,
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
        };
        app.window.onFileDropped.subscribe([importToCatalog](std::string path)
        {
            importToCatalog(path);
            return false;
        });

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
        layer.onUi = [state, &app, thumbnailFor]()
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
            if (state->meshPipeline && vw > 0 && vh > 0)
            {
                se::renderScene(app.renderer, state->editor->scene, state->assets, state->meshPipeline, cam);

                // Gizmo: same camera, but the UN-flipped projection (the renderer keeps
                // the Vulkan Y-flip local) so it is not mirrored.
                glm::mat4 proj = se::cameraProjection(cam, static_cast<float>(vw) / static_cast<float>(vh));
                se::drawGizmo(*state->editor, cam.view, proj,
                              se::viewportContentPos(app.ui), se::viewportContentSize(app.ui),
                              se::viewportHovered(app.ui));

                // Left-click in empty viewport space ray-picks an entity (or clears the
                // selection). Skipped when the click is on/using the gizmo.
                if (se::viewportHovered(app.ui) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
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
            se::assetCatalogPanel(*state->editor, &state->assets.catalog, thumbnailFor);

            // Numeric/data fields read better in a monospace font.
            ImGui::PushFont(se::uiMonoFont(app.ui));
            se::inspectorPanel(*state->editor);
            ImGui::PopFont();
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

        state->assets.meshRefByUuid.clear();
        state->assets.textureRefByUuid.clear();
        state->meshPipeline.reset();
    };

    return se::run(std::move(config));
}
