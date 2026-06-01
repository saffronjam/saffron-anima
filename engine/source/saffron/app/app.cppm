export module Saffron.App;

import std;
import Saffron.Core;
import Saffron.Window;
import Saffron.Rendering;
import Saffron.Ui;

export namespace se
{
    // A Layer is a set of lifecycle callbacks — the runtime "interface" expressed
    // as a struct of function values (a Go interface's itable), not a virtual base.
    // Any callback may be left null.
    struct Layer
    {
        std::string name;
        std::function<void()> onAttach;
        std::function<void(TimeSpan)> onUpdate;
        std::function<void()> onRender;  // submit GPU work; runs inside the frame
        std::function<void()> onUi;
        std::function<void(RenderGraph&)> onRenderGraph;  // add passes to the frame graph
        std::function<void()> onDetach;
    };

    struct App
    {
        Window window;
        Renderer renderer;
        Ui ui;
        std::vector<Layer> layers;
        bool running = false;
    };

    // Client-provided configuration. onCreate runs once the window + renderer
    // exist (attach layers, wire signals there); onExit runs during teardown.
    struct AppConfig
    {
        WindowConfig window;
        std::function<void(App&)> onCreate;
        std::function<void(App&)> onExit;
        bool useImGui = true;  // present-only hosts disable ImGui entirely (no init, no per-frame work)
    };

    void attachLayer(App& app, Layer layer)
    {
        app.layers.push_back(std::move(layer));
    }

    namespace detail
    {
        auto frameLimitFromEnv() -> u64
        {
            const char* raw = std::getenv("SAFFRON_EXIT_AFTER_FRAMES");
            if (raw == nullptr)
            {
                return 0;
            }
            std::string_view text{ raw };
            u64 parsed = 0;
            std::from_chars_result result = std::from_chars(text.data(), text.data() + text.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
            {
                logError(std::format("invalid SAFFRON_EXIT_AFTER_FRAMES='{}', ignoring", text));
                return 0;
            }
            return parsed;
        }
    }

    // Owns the main loop. Returns a process exit code.
    auto run(AppConfig config) -> int
    {
        auto windowResult = newWindow(config.window);
        if (!windowResult)
        {
            logError(std::format("failed to create window: {}", windowResult.error()));
            return 1;
        }

        App app;
        app.window = std::move(*windowResult);
        app.window.onClose.subscribe([&app]() { app.running = false; return false; });

        auto rendererResult = newRenderer(app.window);
        if (!rendererResult)
        {
            logError(std::format("failed to create renderer: {}", rendererResult.error()));
            destroyWindow(app.window);
            return 1;
        }
        app.renderer = std::move(*rendererResult);

        const bool useImGui = config.useImGui;
        if (useImGui)
        {
            auto uiResult = newUi(app.renderer, app.window);
            if (!uiResult)
            {
                logError(std::format("failed to create ui: {}", uiResult.error()));
                destroyRenderer(app.renderer);
                destroyWindow(app.window);
                return 1;
            }
            app.ui = std::move(*uiResult);
        }

        if (config.onCreate)
        {
            config.onCreate(app);
        }
        for (Layer& layer : app.layers)
        {
            if (layer.onAttach)
            {
                layer.onAttach();
            }
        }

        const u64 frameLimit = detail::frameLimitFromEnv();
        u64 frameCount = 0;
        app.running = true;
        std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();

        while (app.running)
        {
            pollEvents(app.window);
            if (app.window.shouldClose)
            {
                app.running = false;
            }

            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            TimeSpan delta{ std::chrono::duration<f32>(now - last).count() };
            last = now;

            for (Layer& layer : app.layers)
            {
                if (layer.onUpdate)
                {
                    layer.onUpdate(delta);
                }
            }

            const bool minimized = app.window.width == 0 || app.window.height == 0;
            if (!minimized && beginFrame(app.renderer))
            {
                for (Layer& layer : app.layers)
                {
                    if (layer.onRender)
                    {
                        layer.onRender();
                    }
                }
                if (useImGui)
                {
                    uiBeginFrame(app.ui);
                }
                for (Layer& layer : app.layers)
                {
                    if (layer.onUi)
                    {
                        layer.onUi();
                    }
                }
                if (useImGui)
                {
                    uiEndFrame(app.ui);
                    uiRecordDrawData(app.renderer);
                }

                // Build the frame graph (cull + scene), let layers add passes against
                // it (e.g. post-process), then finish + execute it.
                beginFrameGraph(app.renderer);
                for (Layer& layer : app.layers)
                {
                    if (layer.onRenderGraph)
                    {
                        layer.onRenderGraph(frameGraph(app.renderer));
                    }
                }
                endFrame(app.renderer);
            }

            frameCount = frameCount + 1;
            if (frameLimit != 0 && frameCount >= frameLimit)
            {
                logInfo(std::format("frame limit reached ({}), exiting", frameLimit));
                app.running = false;
            }
        }

        // Finish all in-flight GPU work before any handler drops resource Refs, so
        // no command buffer still references a buffer/pipeline/image being freed.
        waitGpuIdle(app.renderer);

        for (Layer& layer : app.layers)
        {
            if (layer.onDetach)
            {
                layer.onDetach();
            }
        }
        if (config.onExit)
        {
            config.onExit(app);
        }

        // Optional verification: dump the offscreen viewport image to a PPM.
        if (const char* capturePath = std::getenv("SAFFRON_CAPTURE"))
        {
            auto captured = captureViewport(app.renderer, std::string{ capturePath });
            if (!captured)
            {
                logError(captured.error());
            }
        }

        if (useImGui)
        {
            destroyUi(app.renderer, app.ui);
        }
        destroyRenderer(app.renderer);
        destroyWindow(app.window);
        return 0;
    }
}
