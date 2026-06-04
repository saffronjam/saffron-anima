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
    auto aaModeDto(const std::string& mode) -> AaModeDto
    {
        if (mode == "fxaa")
        {
            return AaModeDto::Fxaa;
        }
        if (mode == "taa")
        {
            return AaModeDto::Taa;
        }
        if (mode == "msaa2")
        {
            return AaModeDto::Msaa2;
        }
        if (mode == "msaa4")
        {
            return AaModeDto::Msaa4;
        }
        if (mode == "msaa8")
        {
            return AaModeDto::Msaa8;
        }
        return AaModeDto::Off;
    }

    void applyAaMode(Renderer& renderer, AaModeDto mode)
    {
        u32 samples = 1;
        bool fxaa = false;
        bool taa = false;
        if (mode == AaModeDto::Fxaa)
        {
            fxaa = true;
        }
        else if (mode == AaModeDto::Taa)
        {
            taa = true;
        }
        else if (mode == AaModeDto::Msaa2)
        {
            samples = 2;
        }
        else if (mode == AaModeDto::Msaa4)
        {
            samples = 4;
        }
        else if (mode == AaModeDto::Msaa8)
        {
            samples = 8;
        }
        setAa(renderer, samples, fxaa, taa);
    }

    auto renderStatsDto(Renderer& renderer) -> RenderStatsDto
    {
        const RenderStats stats = renderStats(renderer);
        return RenderStatsDto{ static_cast<i32>(stats.drawCalls),
                               static_cast<i32>(stats.batches),
                               static_cast<i32>(stats.instances),
                               stats.frameMs,
                               stats.fps,
                               stats.gpuMs,
                               clusteredEnabled(renderer),
                               depthPrepassEnabled(renderer),
                               shadowsEnabled(renderer),
                               iblEnabled(renderer),
                               ssaoEnabled(renderer),
                               contactShadowsEnabled(renderer),
                               ssgiEnabled(renderer),
                               ddgiEnabled(renderer),
                               rtSupported(renderer),
                               rtShadowsEnabled(renderer),
                               restirEnabled(renderer),
                               static_cast<i32>(rtBlasCount(renderer)),
                               static_cast<i32>(pipelineCount(renderer)),
                               true,
                               exposureEv(renderer),
                               aaModeDto(aaMode(renderer)) };
    }

    auto renderStatsJson(Renderer& renderer) -> json
    {
        return dtoToJson(renderStatsDto(renderer));
    }

    void registerRenderCommands(CommandRegistry& reg)
    {
        registerCommand<PingParams, PingResult>(reg, "ping", "liveness + engine info",
                                                [](EngineContext&, const PingParams&) -> Result<PingResult>
                                                {
                                                    return PingResult{ true, std::string{ EngineName },
                                                                       std::string{ EngineVersion },
                                                                       static_cast<i32>(::getpid()) };
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

        registerCommand<EmptyParams, RenderStatsDto>(
            reg, "render-stats", "last frame's scene draw counters",
            [](EngineContext& ctx, const EmptyParams&) -> Result<RenderStatsDto>
            { return renderStatsDto(ctx.renderer); });

        registerCommand<SetAaParams, SetAaResult>(
            reg, "set-aa", "set-aa {off|fxaa|taa|msaa2|msaa4|msaa8} — anti-aliasing mode",
            [](EngineContext& ctx, const SetAaParams& params) -> Result<SetAaResult>
            {
                applyAaMode(ctx.renderer, params.mode.value_or(AaModeDto::Off));
                return SetAaResult{ aaModeDto(aaMode(ctx.renderer)) };
            });

        registerCommand<ToggleParams, SetClusteredResult>(
            reg, "set-clustered", "set-clustered {0|1} — toggle clustered light culling",
            [](EngineContext& ctx, const ToggleParams& params) -> Result<SetClusteredResult>
            {
                const bool enabled = params.enabled.value_or(true);
                setClustered(ctx.renderer, enabled);
                return SetClusteredResult{ enabled };
            });

        registerCommand<ToggleParams, SetIblResult>(
            reg, "set-ibl", "set-ibl {0|1} — toggle image-based ambient (vs flat ambient)",
            [](EngineContext& ctx, const ToggleParams& params) -> Result<SetIblResult>
            {
                setIbl(ctx.renderer, params.enabled.value_or(true));
                return SetIblResult{ iblEnabled(ctx.renderer) };
            });

        registerCommand<ToggleParams, SetSsaoResult>(
            reg, "set-ssao", "set-ssao {0|1} — toggle screen-space ambient occlusion (GTAO)",
            [](EngineContext& ctx, const ToggleParams& params) -> Result<SetSsaoResult>
            {
                setSsao(ctx.renderer, params.enabled.value_or(true));
                return SetSsaoResult{ ssaoEnabled(ctx.renderer) };
            });

        registerCommand<ToggleParams, SetContactShadowsResult>(
            reg, "set-contact-shadows", "set-contact-shadows {0|1} — screen-space contact shadows",
            [](EngineContext& ctx, const ToggleParams& params) -> Result<SetContactShadowsResult>
            {
                setContactShadows(ctx.renderer, params.enabled.value_or(true));
                return SetContactShadowsResult{ contactShadowsEnabled(ctx.renderer) };
            });

        registerCommand<ToggleParams, SetSsgiResult>(
            reg, "set-ssgi", "set-ssgi {0|1} — screen-space one-bounce global illumination",
            [](EngineContext& ctx, const ToggleParams& params) -> Result<SetSsgiResult>
            {
                setSsgi(ctx.renderer, params.enabled.value_or(true));
                return SetSsgiResult{ ssgiEnabled(ctx.renderer) };
            });

        registerCommand<ToggleParams, SetRtShadowsResult>(
            reg, "set-rt-shadows", "set-rt-shadows {0|1} — hardware ray-query shadows (if supported)",
            [](EngineContext& ctx, const ToggleParams& params) -> Result<SetRtShadowsResult>
            {
                if (!rtSupported(ctx.renderer))
                {
                    return Err(std::string{ "ray tracing not supported on this device" });
                }
                setRtShadows(ctx.renderer, params.enabled.value_or(true));
                return SetRtShadowsResult{ rtShadowsEnabled(ctx.renderer) };
            });

        registerCommand<ToggleParams, SetRestirResult>(
            reg, "set-restir", "set-restir {0|1} — ReSTIR stochastic many-light direct (if RT supported)",
            [](EngineContext& ctx, const ToggleParams& params) -> Result<SetRestirResult>
            {
                if (!rtSupported(ctx.renderer))
                {
                    return Err(std::string{ "ray tracing not supported on this device" });
                }
                setRestir(ctx.renderer, params.enabled.value_or(true));
                return SetRestirResult{ restirEnabled(ctx.renderer) };
            });

        registerCommand<SetGiParams, SetGiResult>(
            reg, "set-gi", "set-gi {off|ddgi} — DDGI probe global illumination (multi-bounce)",
            [](EngineContext& ctx, const SetGiParams& params) -> Result<SetGiResult>
            {
                setDdgi(ctx.renderer, params.mode == GiModeDto::Ddgi);
                return SetGiResult{ ddgiEnabled(ctx.renderer) };
            });

        registerCommand<ToggleParams, SetShadowsResult>(
            reg, "set-shadows", "set-shadows {0|1} — toggle the directional shadow map",
            [](EngineContext& ctx, const ToggleParams& params) -> Result<SetShadowsResult>
            {
                const bool enabled = params.enabled.value_or(true);
                setShadows(ctx.renderer, enabled);
                return SetShadowsResult{ enabled };
            });

        registerCommand<SetExposureParams, SetExposureResult>(
            reg, "set-exposure", "set-exposure {ev} — tonemap exposure in stops (exp2)",
            [](EngineContext& ctx, const SetExposureParams& params) -> Result<SetExposureResult>
            {
                setExposure(ctx.renderer, params.ev);
                return SetExposureResult{ exposureEv(ctx.renderer) };
            });

        registerCommand<ToggleParams, SetDepthPrepassResult>(
            reg, "set-depth-prepass", "set-depth-prepass {0|1} — toggle the depth pre-pass",
            [](EngineContext& ctx, const ToggleParams& params) -> Result<SetDepthPrepassResult>
            {
                const bool enabled = params.enabled.value_or(true);
                setDepthPrepass(ctx.renderer, enabled);
                return SetDepthPrepassResult{ enabled };
            });

        registerCommand<EmptyParams, ViewportNativeInfoResult>(
            reg, "viewport-native-info", "native viewport bridge status",
            [](EngineContext& ctx, const EmptyParams&) -> Result<ViewportNativeInfoResult>
            {
                std::string controlPath = controlSocketPath();
                return ViewportNativeInfoResult{ "linux",
                                                 "x11-child-window",
                                                 "engine-window-ready",
                                                 controlPath,
                                                 static_cast<i32>(viewportWidth(ctx.renderer)),
                                                 static_cast<i32>(viewportHeight(ctx.renderer)),
                                                 "engine SDL/Vulkan window can be reparented into an X11 host" };
            });

        registerCommand<AttachNativeViewportParams, AttachNativeViewportResult>(
            reg, "attach-native-viewport",
            "attach-native-viewport {parentXid,x?,y?,width?,height?} — reparent the engine window into an X11 host",
            [](EngineContext& ctx, const AttachNativeViewportParams& params) -> Result<AttachNativeViewportResult>
            {
                const i32 x = params.x.value_or(0);
                const i32 y = params.y.value_or(0);
                const i32 width = std::max(1, params.width.value_or(static_cast<i32>(ctx.window.width)));
                const i32 height = std::max(1, params.height.value_or(static_cast<i32>(ctx.window.height)));

                SDL_SetWindowBordered(ctx.window.handle, false);
                SDL_SetWindowSize(ctx.window.handle, width, height);
                SDL_SetWindowPosition(ctx.window.handle, x, y);
                SDL_SyncWindow(ctx.window.handle);

                SDL_PropertiesID props = SDL_GetWindowProperties(ctx.window.handle);
                Display* display =
                    static_cast<Display*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
                const ::Window child =
                    static_cast<::Window>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
                if (display == nullptr || child == 0)
                {
                    return Err(
                        std::string{ "engine window is not using SDL's X11 backend; run with SDL_VIDEODRIVER=x11" });
                }

                XReparentWindow(display, child, static_cast<::Window>(params.parentXid.value), x, y);
                XMoveResizeWindow(display, child, x, y, static_cast<unsigned int>(width),
                                  static_cast<unsigned int>(height));
                SDL_ShowWindow(ctx.window.handle);
                XMapRaised(display, child);
                XFlush(display);

                ctx.window.width = static_cast<u32>(width);
                ctx.window.height = static_cast<u32>(height);
                setViewportDesiredSize(ctx.renderer, static_cast<u32>(width), static_cast<u32>(height));
                setPresentViewportOnly(ctx.renderer, true);

                return AttachNativeViewportResult{ true, "x11-child-window", x, y, width, height };
            });

        registerCommand<ResizeNativeViewportParams, ResizeNativeViewportResult>(
            reg, "resize-native-viewport",
            "resize-native-viewport {x,y,width,height} — move/resize the reparented child (no reparent)",
            [](EngineContext& ctx, const ResizeNativeViewportParams& params) -> Result<ResizeNativeViewportResult>
            {
                const i32 x = params.x.value_or(0);
                const i32 y = params.y.value_or(0);
                const i32 width = std::max(1, params.width.value_or(static_cast<i32>(ctx.window.width)));
                const i32 height = std::max(1, params.height.value_or(static_cast<i32>(ctx.window.height)));

                SDL_SetWindowSize(ctx.window.handle, width, height);
                SDL_SetWindowPosition(ctx.window.handle, x, y);

                SDL_PropertiesID props = SDL_GetWindowProperties(ctx.window.handle);
                Display* display =
                    static_cast<Display*>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
                const ::Window child =
                    static_cast<::Window>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
                if (display == nullptr || child == 0)
                {
                    return Err(
                        std::string{ "engine window is not using SDL's X11 backend; run with SDL_VIDEODRIVER=x11" });
                }
                XMoveResizeWindow(display, child, x, y, static_cast<unsigned int>(width),
                                  static_cast<unsigned int>(height));
                XFlush(display);

                ctx.window.width = static_cast<u32>(width);
                ctx.window.height = static_cast<u32>(height);
                setViewportDesiredSize(ctx.renderer, static_cast<u32>(width), static_cast<u32>(height));
                return ResizeNativeViewportResult{ true, x, y, width, height };
            });
    }
}
