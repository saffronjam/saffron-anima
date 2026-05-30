// imgui.h is a heavy C++ header, so this TU uses classic includes (no `import
// std`) — consistent with the engine's rendering/ui/scene modules.
#include <imgui.h>
#include <utility>

import Saffron.Core;
import Saffron.App;
import Saffron.Window;
import Saffron.Scene;

namespace
{
    constexpr se::i32 KeyEscape = 27;  // SDLK_ESCAPE

    se::Layer makeEditorLayer()
    {
        se::Layer layer;
        layer.name = "EditorLayer";
        layer.onAttach = []() { se::logInfo("editor layer attached"); };
        layer.onUpdate = [](se::TimeSpan delta) { static_cast<void>(delta); };
        layer.onUi = []()
        {
            ImGui::ShowDemoWindow();
            ImGui::Begin("Saffron");
            ImGui::Text("FPS: %.1f", static_cast<double>(ImGui::GetIO().Framerate));
            ImGui::End();
        };
        layer.onDetach = []() { se::logInfo("editor layer detached"); };
        return layer;
    }
}

int main()
{
    se::AppConfig config;
    config.window = se::WindowConfig{ .title = "Saffron Editor", .width = 1600, .height = 900 };

    config.onCreate = [](se::App& app)
    {
        se::runSceneSelfTest();
        attachLayer(app, makeEditorLayer());

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

    return se::run(std::move(config));
}
