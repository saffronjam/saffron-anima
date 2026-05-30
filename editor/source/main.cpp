// imgui.h is a heavy C++ header, so this TU uses classic includes (no `import
// std`) — consistent with the engine's rendering/ui/scene modules.
#include <imgui.h>

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
        se::u32 meshPipeline = 0;
        bool meshReady = false;
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

        std::expected<se::u32, std::string> pipeline = se::newMeshPipeline(app.renderer, "shaders/mesh.spv");
        if (!pipeline)
        {
            se::logError(pipeline.error());
        }
        else
        {
            state->meshPipeline = *pipeline;
            std::expected<se::Uuid, std::string> cube =
                se::importModel(state->assets, app.renderer, se::assetPath("models/cube.gltf"));
            if (cube)
            {
                se::spawnMesh(state->editor->scene, "Cube", *cube);
                state->meshReady = true;
            }
            else
            {
                se::logError(cube.error());
            }
        }

        se::Layer layer;
        layer.name = "EditorLayer";
        layer.onUpdate = [state, &app](se::TimeSpan)
        {
            if (state->control != nullptr)
            {
                se::pollControl(*state->control, app.window, app.renderer, *state->editor);
            }
        };
        layer.onRender = [state, &app]()
        {
            if (state->meshReady)
            {
                se::renderScene(app.renderer, state->editor->scene, state->assets, state->meshPipeline);
            }
        };
        layer.onUi = [state, &app]()
        {
            se::drawEditorMenuBar(*state->editor);
            se::viewportPanel(app.ui, app.renderer);
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
    };

    return se::run(std::move(config));
}
