module;

#include <nlohmann/json.hpp>
#include <SDL3/SDL.h>
#include <X11/Xlib.h>

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <format>
#include <string>

module Saffron.Control;

import Saffron.Core;
import Saffron.Rendering;

namespace se
{
    auto renderStatsJson(Renderer& renderer) -> json
    {
        const RenderStats stats = renderStats(renderer);
        return json{ { "drawCalls", stats.drawCalls },
                     { "batches", stats.batches },
                     { "instances", stats.instances },
                     { "clustered", clusteredEnabled(renderer) },
                     { "depthPrepass", depthPrepassEnabled(renderer) },
                     { "shadows", shadowsEnabled(renderer) },
                     { "ibl", iblEnabled(renderer) },
                     { "ssao", ssaoEnabled(renderer) },
                     { "contactShadows", contactShadowsEnabled(renderer) },
                     { "ssgi", ssgiEnabled(renderer) },
                     { "ddgi", ddgiEnabled(renderer) },
                     { "rtSupported", rtSupported(renderer) },
                     { "rtShadows", rtShadowsEnabled(renderer) },
                     { "restir", restirEnabled(renderer) },
                     { "blasCount", rtBlasCount(renderer) },
                     { "pipelines", pipelineCount(renderer) },
                     { "hdr", true },
                     { "exposureEv", exposureEv(renderer) },
                     { "aa", aaMode(renderer) } };
    }

    void registerRenderCommands(CommandRegistry& reg)
    {
        registerCommand(reg, "ping", "liveness + engine info",
            [](EngineContext&, const json&) -> Result<json>
            {
                return json{ { "pong", true },
                             { "engine", std::string{ EngineName } },
                             { "version", std::string{ EngineVersion } },
                             { "pid", static_cast<int>(::getpid()) } };
            });

        registerCommand(reg, "help", "list available commands",
            [&reg](EngineContext&, const json&) -> Result<json>
            {
                json commands = json::array();
                for (const CommandTraits& command : reg.rows)
                {
                    commands.push_back(json{ { "name", command.name }, { "help", command.help } });
                }
                return json{ { "commands", std::move(commands) } };
            });

        registerCommand(reg, "render-stats", "last frame's scene draw counters",
            [](EngineContext& ctx, const json&) -> Result<json>
            {
                return renderStatsJson(ctx.renderer);
            });

        registerCommand(reg, "set-aa", "set-aa {off|fxaa|taa|msaa2|msaa4|msaa8} — anti-aliasing mode",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const json value = positionalOr(params, "mode", 0);
                std::string mode = "off";
                if (value.is_string())
                {
                    mode = value.get<std::string>();
                }
                u32 samples = 1;
                bool fxaa = false;
                bool taa = false;
                if (mode == "fxaa") { fxaa = true; }
                else if (mode == "taa") { taa = true; }
                else if (mode == "msaa2") { samples = 2; }
                else if (mode == "msaa4") { samples = 4; }
                else if (mode == "msaa8") { samples = 8; }
                else if (mode != "off")
                {
                    return Err(std::string{ "expected off|fxaa|taa|msaa2|msaa4|msaa8" });
                }
                setAa(ctx.renderer, samples, fxaa, taa);
                return json{ { "aa", aaMode(ctx.renderer) } };
            });

        registerCommand(reg, "set-clustered", "set-clustered {0|1} — toggle clustered light culling",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const json value = positionalOr(params, "enabled", 0);
                bool enabled = true;
                if (value.is_number())
                {
                    enabled = value.get<double>() != 0.0;
                }
                else if (value.is_boolean())
                {
                    enabled = value.get<bool>();
                }
                else if (value.is_string())
                {
                    const std::string s = value.get<std::string>();
                    enabled = !(s == "0" || s == "false" || s == "off");
                }
                setClustered(ctx.renderer, enabled);
                return json{ { "clustered", enabled } };
            });

        registerCommand(reg, "set-ibl", "set-ibl {0|1} — toggle image-based ambient (vs flat ambient)",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const json value = positionalOr(params, "enabled", 0);
                bool enabled = true;
                if (value.is_number())
                {
                    enabled = value.get<double>() != 0.0;
                }
                else if (value.is_boolean())
                {
                    enabled = value.get<bool>();
                }
                else if (value.is_string())
                {
                    const std::string s = value.get<std::string>();
                    enabled = !(s == "0" || s == "false" || s == "off");
                }
                setIbl(ctx.renderer, enabled);
                return json{ { "ibl", iblEnabled(ctx.renderer) } };
            });

        registerCommand(reg, "set-ssao", "set-ssao {0|1} — toggle screen-space ambient occlusion (GTAO)",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const json value = positionalOr(params, "enabled", 0);
                bool enabled = true;
                if (value.is_number())
                {
                    enabled = value.get<double>() != 0.0;
                }
                else if (value.is_boolean())
                {
                    enabled = value.get<bool>();
                }
                else if (value.is_string())
                {
                    const std::string s = value.get<std::string>();
                    enabled = !(s == "0" || s == "false" || s == "off");
                }
                setSsao(ctx.renderer, enabled);
                return json{ { "ssao", ssaoEnabled(ctx.renderer) } };
            });

        registerCommand(reg, "set-contact-shadows", "set-contact-shadows {0|1} — screen-space contact shadows",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const json value = positionalOr(params, "enabled", 0);
                bool enabled = true;
                if (value.is_number()) { enabled = value.get<double>() != 0.0; }
                else if (value.is_boolean()) { enabled = value.get<bool>(); }
                else if (value.is_string())
                {
                    const std::string s = value.get<std::string>();
                    enabled = !(s == "0" || s == "false" || s == "off");
                }
                setContactShadows(ctx.renderer, enabled);
                return json{ { "contactShadows", contactShadowsEnabled(ctx.renderer) } };
            });

        registerCommand(reg, "set-ssgi", "set-ssgi {0|1} — screen-space one-bounce global illumination",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const json value = positionalOr(params, "enabled", 0);
                bool enabled = true;
                if (value.is_number()) { enabled = value.get<double>() != 0.0; }
                else if (value.is_boolean()) { enabled = value.get<bool>(); }
                else if (value.is_string())
                {
                    const std::string s = value.get<std::string>();
                    enabled = !(s == "0" || s == "false" || s == "off");
                }
                setSsgi(ctx.renderer, enabled);
                return json{ { "ssgi", ssgiEnabled(ctx.renderer) } };
            });

        registerCommand(reg, "set-rt-shadows", "set-rt-shadows {0|1} — hardware ray-query shadows (if supported)",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                if (!rtSupported(ctx.renderer))
                {
                    return Err(std::string{ "ray tracing not supported on this device" });
                }
                const json value = positionalOr(params, "enabled", 0);
                bool enabled = true;
                if (value.is_number()) { enabled = value.get<double>() != 0.0; }
                else if (value.is_boolean()) { enabled = value.get<bool>(); }
                else if (value.is_string())
                {
                    const std::string s = value.get<std::string>();
                    enabled = !(s == "0" || s == "false" || s == "off");
                }
                setRtShadows(ctx.renderer, enabled);
                return json{ { "rtShadows", rtShadowsEnabled(ctx.renderer) } };
            });

        registerCommand(reg, "set-restir", "set-restir {0|1} — ReSTIR stochastic many-light direct (if RT supported)",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                if (!rtSupported(ctx.renderer))
                {
                    return Err(std::string{ "ray tracing not supported on this device" });
                }
                const json value = positionalOr(params, "enabled", 0);
                bool enabled = true;
                if (value.is_number()) { enabled = value.get<double>() != 0.0; }
                else if (value.is_boolean()) { enabled = value.get<bool>(); }
                else if (value.is_string())
                {
                    const std::string s = value.get<std::string>();
                    enabled = !(s == "0" || s == "false" || s == "off");
                }
                setRestir(ctx.renderer, enabled);
                return json{ { "restir", restirEnabled(ctx.renderer) } };
            });

        registerCommand(reg, "set-gi", "set-gi {off|ddgi} — DDGI probe global illumination (multi-bounce)",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const json value = positionalOr(params, "mode", 0);
                std::string mode = "off";
                if (value.is_string()) { mode = value.get<std::string>(); }
                else if (value.is_number()) { mode = value.get<double>() != 0.0 ? "ddgi" : "off"; }
                if (mode != "off" && mode != "ddgi")
                {
                    return Err(std::string{ "expected off|ddgi" });
                }
                setDdgi(ctx.renderer, mode == "ddgi");
                return json{ { "ddgi", ddgiEnabled(ctx.renderer) } };
            });

        registerCommand(reg, "set-shadows", "set-shadows {0|1} — toggle the directional shadow map",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const json value = positionalOr(params, "enabled", 0);
                bool enabled = true;
                if (value.is_number())
                {
                    enabled = value.get<double>() != 0.0;
                }
                else if (value.is_boolean())
                {
                    enabled = value.get<bool>();
                }
                else if (value.is_string())
                {
                    const std::string s = value.get<std::string>();
                    enabled = !(s == "0" || s == "false" || s == "off");
                }
                setShadows(ctx.renderer, enabled);
                return json{ { "shadows", enabled } };
            });

        registerCommand(reg, "set-exposure", "set-exposure {ev} — tonemap exposure in stops (exp2)",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const json value = positionalOr(params, "ev", 0);
                if (!value.is_number())
                {
                    return Err(std::string{ "expected a numeric EV" });
                }
                setExposure(ctx.renderer, static_cast<f32>(value.get<double>()));
                return json{ { "exposureEv", exposureEv(ctx.renderer) } };
            });

        registerCommand(reg, "set-depth-prepass", "set-depth-prepass {0|1} — toggle the depth pre-pass",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                const json value = positionalOr(params, "enabled", 0);
                bool enabled = true;
                if (value.is_number())
                {
                    enabled = value.get<double>() != 0.0;
                }
                else if (value.is_boolean())
                {
                    enabled = value.get<bool>();
                }
                else if (value.is_string())
                {
                    const std::string s = value.get<std::string>();
                    enabled = !(s == "0" || s == "false" || s == "off");
                }
                setDepthPrepass(ctx.renderer, enabled);
                return json{ { "depthPrepass", enabled } };
            });

        registerCommand(reg, "viewport-native-info", "native viewport bridge status",
            [](EngineContext& ctx, const json&) -> Result<json>
            {
                std::string controlPath = controlSocketPath();
                return json{
                    { "platform", "linux" },
                    { "transport", "x11-child-window" },
                    { "status", "engine-window-ready" },
                    { "controlSocket", controlPath },
                    { "width", viewportWidth(ctx.renderer) },
                    { "height", viewportHeight(ctx.renderer) },
                    { "message", "engine SDL/Vulkan window can be reparented into an X11 host" }
                };
            });

        registerCommand(reg, "attach-native-viewport",
            "attach-native-viewport {parentXid,x?,y?,width?,height?} — reparent the engine window into an X11 host",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                auto readU64 = [&params](const char* name) -> Result<u64>
                {
                    const json value = positionalOr(params, name, 0);
                    if (value.is_number_unsigned())
                    {
                        return value.get<u64>();
                    }
                    if (value.is_number_integer())
                    {
                        const i64 parsed = value.get<i64>();
                        if (parsed >= 0) { return static_cast<u64>(parsed); }
                    }
                    if (value.is_string())
                    {
                        const std::string text = value.get<std::string>();
                        u64 parsed = 0;
                        std::from_chars_result result =
                            std::from_chars(text.data(), text.data() + text.size(), parsed);
                        if (result.ec == std::errc{} && result.ptr == text.data() + text.size())
                        {
                            return parsed;
                        }
                    }
                    return Err(std::format("expected numeric {}", name));
                };
                auto readI32 = [&params](const char* name, i32 fallback) -> i32
                {
                    const json value = params.contains(name) ? params[name] : json{ fallback };
                    if (value.is_number_integer()) { return value.get<i32>(); }
                    if (value.is_number()) { return static_cast<i32>(value.get<double>()); }
                    return fallback;
                };

                auto parent = readU64("parentXid");
                if (!parent)
                {
                    return Err(parent.error());
                }
                const i32 x = readI32("x", 0);
                const i32 y = readI32("y", 0);
                const i32 width = std::max(1, readI32("width", static_cast<i32>(ctx.window.width)));
                const i32 height = std::max(1, readI32("height", static_cast<i32>(ctx.window.height)));

                SDL_SetWindowBordered(ctx.window.handle, false);
                SDL_SetWindowSize(ctx.window.handle, width, height);
                SDL_SetWindowPosition(ctx.window.handle, x, y);
                SDL_SyncWindow(ctx.window.handle);

                SDL_PropertiesID props = SDL_GetWindowProperties(ctx.window.handle);
                Display* display = static_cast<Display*>(
                    SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
                const ::Window child = static_cast<::Window>(
                    SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
                if (display == nullptr || child == 0)
                {
                    return Err(std::string{ "engine window is not using SDL's X11 backend; run with SDL_VIDEODRIVER=x11" });
                }

                XReparentWindow(display, child, static_cast<::Window>(*parent), x, y);
                XMoveResizeWindow(display, child, x, y,
                    static_cast<unsigned int>(width), static_cast<unsigned int>(height));
                XMapRaised(display, child);
                XFlush(display);

                ctx.window.width = static_cast<u32>(width);
                ctx.window.height = static_cast<u32>(height);
                setViewportDesiredSize(ctx.renderer, static_cast<u32>(width), static_cast<u32>(height));
                setPresentViewportOnly(ctx.renderer, true);

                return json{
                    { "attached", true },
                    { "transport", "x11-child-window" },
                    { "x", x },
                    { "y", y },
                    { "width", width },
                    { "height", height }
                };
            });

        registerCommand(reg, "resize-native-viewport",
            "resize-native-viewport {x,y,width,height} — move/resize the reparented child (no reparent)",
            [](EngineContext& ctx, const json& params) -> Result<json>
            {
                auto readI32 = [&params](const char* name, i32 fallback) -> i32
                {
                    const json value = params.contains(name) ? params[name] : json{ fallback };
                    if (value.is_number_integer()) { return value.get<i32>(); }
                    if (value.is_number()) { return static_cast<i32>(value.get<double>()); }
                    return fallback;
                };

                const i32 x = readI32("x", 0);
                const i32 y = readI32("y", 0);
                const i32 width = std::max(1, readI32("width", static_cast<i32>(ctx.window.width)));
                const i32 height = std::max(1, readI32("height", static_cast<i32>(ctx.window.height)));

                SDL_SetWindowSize(ctx.window.handle, width, height);
                SDL_SetWindowPosition(ctx.window.handle, x, y);

                SDL_PropertiesID props = SDL_GetWindowProperties(ctx.window.handle);
                Display* display = static_cast<Display*>(
                    SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
                const ::Window child = static_cast<::Window>(
                    SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
                if (display == nullptr || child == 0)
                {
                    return Err(std::string{ "engine window is not using SDL's X11 backend; run with SDL_VIDEODRIVER=x11" });
                }
                XMoveResizeWindow(display, child, x, y,
                    static_cast<unsigned int>(width), static_cast<unsigned int>(height));
                XFlush(display);

                ctx.window.width = static_cast<u32>(width);
                ctx.window.height = static_cast<u32>(height);
                setViewportDesiredSize(ctx.renderer, static_cast<u32>(width), static_cast<u32>(height));
                return json{ { "resized", true }, { "x", x }, { "y", y }, { "width", width }, { "height", height } };
            });
    }
}
