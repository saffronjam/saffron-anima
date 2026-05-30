// imgui.h is a heavy C++ header, so this TU uses classic includes (no `import
// std`) — consistent with the engine's rendering/ui/scene modules.
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

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
import Saffron.Geometry;

namespace
{
    constexpr se::i32 KeyEscape = 27;  // SDLK_ESCAPE

    // State shared across the app lifecycle closures. The EditorContext is owned
    // by the engine (heap) so its heavy entt/json destructor stays out of this TU.
    struct EditorState
    {
        se::EditorContext* editor = nullptr;
        se::ControlContext* control = nullptr;
        se::u32 meshPipeline = 0;
        se::u32 cubeMesh = 0;
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
        std::expected<se::Mesh, std::string> cube = se::importModelFile(se::assetPath("models/cube.gltf"));
        if (!cube)
        {
            se::logError(cube.error());
        }
        if (pipeline && cube)
        {
            std::expected<se::u32, std::string> uploaded = se::uploadMesh(app.renderer, *cube);
            if (uploaded)
            {
                state->meshPipeline = *pipeline;
                state->cubeMesh = *uploaded;
                state->meshReady = true;
            }
            else
            {
                se::logError(uploaded.error());
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
            if (!state->meshReady)
            {
                return;
            }
            const float aspect = static_cast<float>(app.window.width) / static_cast<float>(app.window.height);
            glm::mat4 model = glm::rotate(glm::mat4(1.0f), glm::radians(35.0f),
                                          glm::normalize(glm::vec3(0.4f, 1.0f, 0.2f)));
            glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f),
                                         glm::vec3(0.0f, 1.0f, 0.0f));
            glm::mat4 proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f);
            proj[1][1] *= -1.0f;  // flip Y into Vulkan clip space
            se::drawMesh(app.renderer, state->cubeMesh, state->meshPipeline, proj * view * model);
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
