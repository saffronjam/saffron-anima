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

namespace
{
    constexpr se::i32 KeyEscape = 27;  // SDLK_ESCAPE

    // State shared across the app lifecycle closures. The EditorContext is owned
    // by the engine (heap) so its heavy entt/json destructor stays out of this TU.
    struct EditorState
    {
        se::EditorContext* editor = nullptr;
        se::u32 trianglePipeline = 0;
        bool pipelineReady = false;
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

        std::expected<se::u32, std::string> pipeline =
            se::newTrianglePipeline(app.renderer, "shaders/triangle.spv");
        if (pipeline)
        {
            state->trianglePipeline = *pipeline;
            state->pipelineReady = true;
        }
        else
        {
            se::logError(pipeline.error());
        }

        se::Layer layer;
        layer.name = "EditorLayer";
        layer.onRender = [state, &app]()
        {
            if (state->pipelineReady)
            {
                se::drawTriangle(app.renderer, state->trianglePipeline);  // placeholder Viewport content
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
        if (state->editor != nullptr)
        {
            se::destroyEditorContext(state->editor);
            state->editor = nullptr;
        }
    };

    return se::run(std::move(config));
}
