module;

#include <nlohmann/json.hpp>
#include <SDL3/SDL.h>

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

    auto profilerModeDto(ProfilerMode mode) -> ProfilerModeDto
    {
        switch (mode)
        {
        case ProfilerMode::Timestamps:
            return ProfilerModeDto::Timestamps;
        case ProfilerMode::PipelineStats:
            return ProfilerModeDto::PipelineStats;
        case ProfilerMode::Off:
            break;
        }
        return ProfilerModeDto::Off;
    }

    auto profilerModeFromDto(ProfilerModeDto mode) -> ProfilerMode
    {
        switch (mode)
        {
        case ProfilerModeDto::Timestamps:
            return ProfilerMode::Timestamps;
        case ProfilerModeDto::PipelineStats:
            return ProfilerMode::PipelineStats;
        case ProfilerModeDto::Off:
            break;
        }
        return ProfilerMode::Off;
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
                               stats.cpuFrameMs,
                               stats.gpuMs,
                               stats.cpuWaitMs,
                               static_cast<i32>(stats.triangles),
                               static_cast<i32>(stats.descriptorBinds),
                               static_cast<i32>(stats.commandBuffers),
                               static_cast<i32>(stats.queueSubmits),
                               static_cast<i32>(stats.pipelinesCreated),
                               stats.vramUsageBytes,
                               stats.vramBudgetBytes,
                               stats.softwareGpu,
                               profilerModeDto(stats.profilerMode),
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

    auto passTimingsDto(Renderer& renderer) -> RenderPassTimingsDto
    {
        RenderPassTimingsDto out;
        for (const PassTiming& timing : passTimings(renderer))
        {
            out.passes.push_back(RenderPassTimingDto{ timing.name, timing.gpuMs });
        }
        out.gpuTotalMs = passTimingsTotalMs(renderer);
        out.softwareGpu = softwareGpu(renderer);
        out.profilerMode = profilerModeDto(profilerMode(renderer));
        return out;
    }

    auto perfConfigDto(const PerfConfig& config) -> PerfConfigDto
    {
        return PerfConfigDto{ config.targetFps,      perfBudgetMs(config),  config.greenBudgetFrac,
                              config.greenMedianMul, config.amberMedianMul, config.frozenMs,
                              config.vramWarnFrac,   config.vramCritFrac };
    }

    auto frameHistoryDto(Renderer& renderer, i32 samples) -> FrameHistoryDto
    {
        const FrameHistoryStats stats = frameHistoryStats(renderer);
        FrameHistoryDto out;
        out.p50Ms = stats.p50Ms;
        out.p95Ms = stats.p95Ms;
        out.p99Ms = stats.p99Ms;
        out.p999Ms = stats.p999Ms;
        out.maxMs = stats.maxMs;
        out.meanMs = stats.meanMs;
        out.stddevMs = stats.stddevMs;
        out.stutterCount = static_cast<i64>(stats.stutterCount);
        out.sampleCount = static_cast<i32>(stats.sampleCount);
        out.budgetMs = perfBudgetMs(perfConfig(renderer));
        if (samples > 0)
        {
            for (const FrameSample& sample : frameSamples(renderer, static_cast<u32>(samples)))
            {
                out.samples.push_back(FrameSampleDto{ static_cast<i64>(sample.frameIndex), sample.cpuMs, sample.gpuMs,
                                                      sample.cpuWaitMs });
            }
        }
        return out;
    }

    auto alarmSeverityDto(AlarmSeverity severity) -> AlarmSeverityDto
    {
        switch (severity)
        {
        case AlarmSeverity::Warning:
            return AlarmSeverityDto::Warning;
        case AlarmSeverity::Critical:
            return AlarmSeverityDto::Critical;
        case AlarmSeverity::Info:
            break;
        }
        return AlarmSeverityDto::Info;
    }

    auto alarmEventDto(const AlarmEvent& event) -> AlarmEventDto
    {
        return AlarmEventDto{ static_cast<i64>(event.seq),
                              std::to_string(event.fingerprint),
                              event.metric,
                              event.pass,
                              alarmSeverityDto(event.severity),
                              event.kind == AlarmEventKind::Resolved ? AlarmStateDto::Resolved : AlarmStateDto::Firing,
                              event.value,
                              event.threshold,
                              static_cast<i64>(event.sinceFrame),
                              static_cast<i32>(event.count),
                              event.durationMs };
    }

    auto drainAlarmsDto(Renderer& renderer, i64 since) -> DrainAlarmsResult
    {
        const AlarmDrain drain = drainAlarms(renderer, since < 0 ? 0 : static_cast<u64>(since));
        DrainAlarmsResult out;
        for (const AlarmEvent& event : drain.events)
        {
            out.events.push_back(alarmEventDto(event));
        }
        out.highWaterSeq = static_cast<i64>(drain.highWaterSeq);
        out.oldestSeq = static_cast<i64>(drain.oldestSeq);
        out.overflowed = drain.overflowed;
        return out;
    }

    auto activeAlarmsDto(Renderer& renderer) -> ActiveAlarmsDto
    {
        ActiveAlarmsDto out;
        for (const ActiveAlarm& alarm : activeAlarms(renderer))
        {
            out.alarms.push_back(ActiveAlarmDto{ std::to_string(alarm.fingerprint), alarm.metric, alarm.pass,
                                                 alarmSeverityDto(alarm.severity), alarm.value, alarm.threshold,
                                                 static_cast<i64>(alarm.sinceFrame), static_cast<i32>(alarm.count) });
        }
        return out;
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

        registerCommand<ProfilerSetModeParams, ProfilerModeResult>(
            reg, "profiler.set-mode",
            "profiler.set-mode {off|timestamps|pipeline-stats} — per-pass GPU timing + counters",
            [](EngineContext& ctx, const ProfilerSetModeParams& params) -> Result<ProfilerModeResult>
            {
                setProfilerMode(ctx.renderer, profilerModeFromDto(params.mode.value_or(ProfilerModeDto::Off)));
                return ProfilerModeResult{ profilerModeDto(profilerMode(ctx.renderer)),
                                           profilerTimestampsSupported(ctx.renderer),
                                           profilerPipelineStatsSupported(ctx.renderer), softwareGpu(ctx.renderer) };
            });

        registerCommand<EmptyParams, RenderPassTimingsDto>(
            reg, "pass-timings", "last frame's per-pass GPU timings (needs profiler timestamps mode)",
            [](EngineContext& ctx, const EmptyParams&) -> Result<RenderPassTimingsDto>
            { return passTimingsDto(ctx.renderer); });

        registerCommand<FrameHistoryParams, FrameHistoryDto>(
            reg, "frame-history", "frame-time percentiles + stutter count (+ optional recent samples)",
            [](EngineContext& ctx, const FrameHistoryParams& params) -> Result<FrameHistoryDto>
            { return frameHistoryDto(ctx.renderer, params.samples.value_or(0)); });

        registerCommand<EmptyParams, PerfConfigDto>(reg, "get-perf-config",
                                                    "the shared frame-budget / green-amber-red threshold config",
                                                    [](EngineContext& ctx, const EmptyParams&) -> Result<PerfConfigDto>
                                                    { return perfConfigDto(perfConfig(ctx.renderer)); });

        registerCommand<SetPerfConfigParams, PerfConfigDto>(
            reg, "set-perf-config", "set-perf-config {targetFps,...} — frame budget + green/amber/red thresholds",
            [](EngineContext& ctx, const SetPerfConfigParams& params) -> Result<PerfConfigDto>
            {
                PerfConfig config = perfConfig(ctx.renderer);
                if (params.targetFps)
                {
                    config.targetFps = *params.targetFps;
                }
                if (params.greenBudgetFrac)
                {
                    config.greenBudgetFrac = *params.greenBudgetFrac;
                }
                if (params.greenMedianMul)
                {
                    config.greenMedianMul = *params.greenMedianMul;
                }
                if (params.amberMedianMul)
                {
                    config.amberMedianMul = *params.amberMedianMul;
                }
                if (params.frozenMs)
                {
                    config.frozenMs = *params.frozenMs;
                }
                if (params.vramWarnFrac)
                {
                    config.vramWarnFrac = *params.vramWarnFrac;
                }
                if (params.vramCritFrac)
                {
                    config.vramCritFrac = *params.vramCritFrac;
                }
                setPerfConfig(ctx.renderer, config);
                return perfConfigDto(perfConfig(ctx.renderer));
            });

        registerCommand<DrainAlarmsParams, DrainAlarmsResult>(
            reg, "drain-alarms", "drain-alarms {since} — perf-alarm events with seq > since (non-blocking)",
            [](EngineContext& ctx, const DrainAlarmsParams& params) -> Result<DrainAlarmsResult>
            { return drainAlarmsDto(ctx.renderer, params.since.value_or(0)); });

        registerCommand<EmptyParams, ActiveAlarmsDto>(
            reg, "list-active-alarms", "currently firing perf alarms (the badge + row highlights)",
            [](EngineContext& ctx, const EmptyParams&) -> Result<ActiveAlarmsDto>
            { return activeAlarmsDto(ctx.renderer); });

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

        registerCommand<ToggleParams, SetSkinningResult>(
            reg, "set-skinning", "set-skinning {0|1} — toggle the GPU skinning path",
            [](EngineContext& ctx, const ToggleParams& params) -> Result<SetSkinningResult>
            {
                const bool enabled = params.enabled.value_or(true);
                setSkinning(ctx.renderer, enabled);
                return SetSkinningResult{ enabled };
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
                                                 "wayland-subsurface",
                                                 "engine-ready",
                                                 controlPath,
                                                 static_cast<i32>(viewportWidth(ctx.renderer)),
                                                 static_cast<i32>(viewportHeight(ctx.renderer)),
                                                 "engine renders offscreen; the editor presents frames "
                                                 "from shared memory on a wayland subsurface" };
            });

        registerCommand<SetViewportSizeParams, SetViewportSizeResult>(
            reg, "set-viewport-size",
            "set-viewport-size {width, height} — set the offscreen render size (device pixels)",
            [](EngineContext& ctx, const SetViewportSizeParams& params) -> Result<SetViewportSizeResult>
            {
                const i32 width = std::max(1, params.width.value_or(static_cast<i32>(viewportWidth(ctx.renderer))));
                const i32 height = std::max(1, params.height.value_or(static_cast<i32>(viewportHeight(ctx.renderer))));
                setViewportDesiredSize(ctx.renderer, static_cast<u32>(width), static_cast<u32>(height));
                return SetViewportSizeResult{ width, height };
            });
    }
}
