module;

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <string>

module Saffron.Control;

import Saffron.Core;
import Saffron.Rendering;

namespace se
{
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
                const RenderStats stats = renderStats(ctx.renderer);
                return json{ { "drawCalls", stats.drawCalls },
                             { "batches", stats.batches },
                             { "instances", stats.instances },
                             { "clustered", clusteredEnabled(ctx.renderer) },
                             { "depthPrepass", depthPrepassEnabled(ctx.renderer) },
                             { "shadows", shadowsEnabled(ctx.renderer) },
                             { "ibl", iblEnabled(ctx.renderer) },
                             { "ssao", ssaoEnabled(ctx.renderer) },
                             { "contactShadows", contactShadowsEnabled(ctx.renderer) },
                             { "ssgi", ssgiEnabled(ctx.renderer) },
                             { "ddgi", ddgiEnabled(ctx.renderer) },
                             { "pipelines", pipelineCount(ctx.renderer) },
                             { "hdr", true },
                             { "exposureEv", exposureEv(ctx.renderer) },
                             { "aa", aaMode(ctx.renderer) } };
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
    }
}
