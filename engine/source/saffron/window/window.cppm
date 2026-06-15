module;

#include <SDL3/SDL.h>

export module Saffron.Window;

import std;
import Saffron.Core;
import Saffron.Signal;

export namespace sa
{
    struct WindowConfig
    {
        std::string title = "Saffron";
        u32 width = 1600;
        u32 height = 900;
        bool hidden = false;
    };

    // Plain data: a native handle, current size, and typed event signals.
    struct Window
    {
        SDL_Window* handle = nullptr;
        u32 width = 0;
        u32 height = 0;
        bool shouldClose = false;

        SubscriberList<> onClose;
        SubscriberList<u32, u32> onResize;          // width, height (pixels)
        SubscriberList<i32, bool> onKeyPressed;     // keycode, isRepeat
        SubscriberList<i32> onKeyReleased;          // keycode
        SubscriberList<std::string> onFileDropped;  // dropped file path

        // Raw SDL events are forwarded to each sink before typed dispatch.
        // The host uses this to feed the gizmo + editor fly-camera input.
        std::vector<std::function<void(const SDL_Event&)>> eventSinks;
    };

    auto newWindow(const WindowConfig& config) -> Result<Window>
    {
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            return Err(std::format("SDL_Init failed: {}", SDL_GetError()));
        }

        SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (config.hidden)
        {
            flags |= SDL_WINDOW_HIDDEN;
        }
        SDL_Window* handle = SDL_CreateWindow(config.title.c_str(), static_cast<int>(config.width),
                                              static_cast<int>(config.height), flags);
        if (handle == nullptr)
        {
            return Err(std::format("SDL_CreateWindow failed: {}", SDL_GetError()));
        }

        Window window;
        window.handle = handle;
        window.width = config.width;
        window.height = config.height;
        return window;
    }

    void destroyWindow(Window& window)
    {
        if (window.handle != nullptr)
        {
            SDL_DestroyWindow(window.handle);
            window.handle = nullptr;
        }
        SDL_Quit();
    }

    void pollEvents(Window& window)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            for (const std::function<void(const SDL_Event&)>& sink : window.eventSinks)
            {
                sink(event);
            }

            if (event.type == SDL_EVENT_QUIT)
            {
                window.shouldClose = true;
                window.onClose.publish();
            }
            else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            {
                window.shouldClose = true;
                window.onClose.publish();
            }
            else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
            {
                window.width = static_cast<u32>(event.window.data1);
                window.height = static_cast<u32>(event.window.data2);
                window.onResize.publish(window.width, window.height);
            }
            else if (event.type == SDL_EVENT_KEY_DOWN)
            {
                window.onKeyPressed.publish(static_cast<i32>(event.key.key), event.key.repeat);
            }
            else if (event.type == SDL_EVENT_KEY_UP)
            {
                window.onKeyReleased.publish(static_cast<i32>(event.key.key));
            }
            else if (event.type == SDL_EVENT_DROP_FILE && event.drop.data != nullptr)
            {
                window.onFileDropped.publish(std::string{ event.drop.data });
            }
        }
    }
}
