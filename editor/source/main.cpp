// imgui.h is a heavy C++ header, so this TU uses classic includes (no `import
// std`) — consistent with the engine's rendering/ui/scene modules.
#include <imgui.h>
#include <glm/glm.hpp>

#include <expected>
#include <memory>
#include <string>
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

    // State shared across the app lifecycle closures. The EditorContext is owned
    // by the engine (heap) so its heavy entt/json destructor stays out of this TU.
    struct EditorState
    {
        se::EditorContext* editor = nullptr;
        se::ControlContext* control = nullptr;
        se::AssetServer assets;
        se::Ref<se::Pipeline> meshPipeline;
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

        // One import path for both GUI entry points: File > Import and drag-and-drop.
        auto importAndSpawn = [state, &app](const std::string& path)
        {
            std::expected<se::ImportResult, std::string> imported = se::importModel(state->assets, app.renderer, path);
            if (!imported)
            {
                se::logError(imported.error());
                return;
            }
            se::setSelection(*state->editor, se::spawnModel(state->editor->scene, "Mesh", *imported));
        };
        state->editor->onImportModel = importAndSpawn;
        state->editor->onCreateCube = [state, &app, importAndSpawn]()
        {
            importAndSpawn(se::assetPath("models/cube.gltf"));
        };
        app.window.onFileDropped.subscribe([importAndSpawn](std::string path)
        {
            importAndSpawn(path);
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
        layer.onRender = [state, &app]()
        {
            if (state->meshPipeline)
            {
                se::renderScene(app.renderer, state->editor->scene, state->assets, state->meshPipeline);
            }
        };
        layer.onUi = [state, &app]()
        {
            se::drawEditorMenuBar(*state->editor);
            se::viewportPanel(app.ui, app.renderer);

            // Gizmo overlay: drive it from the same camera the scene renders through,
            // but with the UN-flipped projection (the renderer keeps the Vulkan Y-flip
            // local). Aspect matches renderScene's (the offscreen viewport dimensions).
            const se::u32 vw = se::viewportWidth(app.renderer);
            const se::u32 vh = se::viewportHeight(app.renderer);
            se::CameraView cam = se::primaryCamera(state->editor->scene);
            if (cam.valid && vw > 0 && vh > 0)
            {
                glm::mat4 proj = se::cameraProjection(cam, static_cast<float>(vw) / static_cast<float>(vh));
                se::drawGizmo(*state->editor, cam.view, proj,
                              se::viewportContentPos(app.ui), se::viewportContentSize(app.ui),
                              se::viewportHovered(app.ui));
            }

            se::hierarchyPanel(*state->editor);
            se::inspectorPanel(*state->editor);
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
        // would be freed too late (use-after-free).
        state->assets.meshRefByUuid.clear();
        state->assets.textureRefByUuid.clear();
        state->meshPipeline.reset();
    };

    return se::run(std::move(config));
}
