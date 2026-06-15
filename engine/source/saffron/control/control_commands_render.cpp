module;

#include <nlohmann/json.hpp>
#include <SDL3/SDL.h>

#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

module Saffron.Control;

import Saffron.Core;
import Saffron.Rendering;

namespace sa
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

    auto viewModeDto(ViewMode mode) -> ViewModeDto
    {
        switch (mode)
        {
        case ViewMode::Wireframe:
            return ViewModeDto::Wireframe;
        case ViewMode::Albedo:
            return ViewModeDto::Albedo;
        case ViewMode::Normal:
            return ViewModeDto::Normal;
        case ViewMode::Roughness:
            return ViewModeDto::Roughness;
        case ViewMode::Metallic:
            return ViewModeDto::Metallic;
        case ViewMode::Emissive:
            return ViewModeDto::Emissive;
        case ViewMode::Lit:
            break;
        }
        return ViewModeDto::Lit;
    }

    auto viewModeFromDto(ViewModeDto mode) -> ViewMode
    {
        switch (mode)
        {
        case ViewModeDto::Wireframe:
            return ViewMode::Wireframe;
        case ViewModeDto::Albedo:
            return ViewMode::Albedo;
        case ViewModeDto::Normal:
            return ViewMode::Normal;
        case ViewModeDto::Roughness:
            return ViewMode::Roughness;
        case ViewModeDto::Metallic:
            return ViewMode::Metallic;
        case ViewModeDto::Emissive:
            return ViewMode::Emissive;
        case ViewModeDto::Lit:
            break;
        }
        return ViewMode::Lit;
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

    auto captureModeFromDto(CaptureModeDto mode) -> CaptureMode
    {
        switch (mode)
        {
        case CaptureModeDto::Frames:
            return CaptureMode::Frames;
        case CaptureModeDto::Rolling:
            return CaptureMode::Rolling;
        case CaptureModeDto::Single:
            break;
        }
        return CaptureMode::Single;
    }

    auto captureModeDto(CaptureMode mode) -> CaptureModeDto
    {
        switch (mode)
        {
        case CaptureMode::Frames:
            return CaptureModeDto::Frames;
        case CaptureMode::Rolling:
            return CaptureModeDto::Rolling;
        case CaptureMode::Single:
            break;
        }
        return CaptureModeDto::Single;
    }

    auto profileLaneDto(ProfileLane lane) -> ProfileLaneDto
    {
        switch (lane)
        {
        case ProfileLane::Gpu:
            return ProfileLaneDto::Gpu;
        case ProfileLane::Cpu:
            break;
        }
        return ProfileLaneDto::Cpu;
    }

    auto captureStateDto(CaptureState state) -> CaptureStateDto
    {
        switch (state)
        {
        case CaptureState::Arming:
            return CaptureStateDto::Arming;
        case CaptureState::Recording:
            return CaptureStateDto::Recording;
        case CaptureState::Ready:
            return CaptureStateDto::Ready;
        case CaptureState::Idle:
            break;
        }
        return CaptureStateDto::Idle;
    }

    auto profileCaptureDto(const ProfileCapture& capture) -> ProfileCaptureDto
    {
        ProfileCaptureDto out;
        out.spans.reserve(capture.spans.size());
        for (const ProfileSpan& s : capture.spans)
        {
            ProfileSpanDto span{ .name = s.name,
                                 .lane = profileLaneDto(s.lane),
                                 .startNs = s.startNs,
                                 .endNs = s.endNs,
                                 .parentIndex = s.parentIndex,
                                 .depth = s.depth,
                                 .pipelineStats = {} };
            if (s.hasStats)
            {
                span.pipelineStats = PipelineStatsDto{ .inputVertices = s.stats.inputVertices,
                                                       .vertexInvocations = s.stats.vertexInvocations,
                                                       .clippingInvocations = s.stats.clippingInvocations,
                                                       .clippingPrimitives = s.stats.clippingPrimitives,
                                                       .fragmentInvocations = s.stats.fragmentInvocations,
                                                       .computeInvocations = s.stats.computeInvocations,
                                                       .pixels = s.stats.pixels };
            }
            out.spans.push_back(std::move(span));
        }
        out.metadata = ProfileCaptureMetadataDto{ .softwareGpu = capture.meta.softwareGpu,
                                                  .correlated = capture.meta.correlated,
                                                  .deviceName = capture.meta.deviceName,
                                                  .timestampPeriod = capture.meta.timestampPeriod,
                                                  .targetFps = capture.meta.targetFps,
                                                  .mode = profilerModeDto(capture.meta.mode),
                                                  .filter = capture.meta.filter,
                                                  .frameCount = capture.meta.frameCount };
        return out;
    }

    // Serialize a capture to Chrome Trace Event JSON: `M` (metadata) events name the two lanes,
    // `X` (complete) events carry each span's microsecond ts/dur (the viewer derives nesting from
    // time containment). The honesty flags + device facts ride in otherData so the file is
    // self-documenting in chrome://tracing / Perfetto / speedscope.
    auto toChromeTrace(const ProfileCapture& capture) -> std::string
    {
        const int cpuTid = 1;
        const int gpuTid = 2;
        json events = json::array();
        events.push_back({ { "ph", "M" },
                           { "pid", "SaffronAnima" },
                           { "name", "process_name" },
                           { "args", { { "name", "SaffronAnima" } } } });
        events.push_back({ { "ph", "M" },
                           { "pid", "SaffronAnima" },
                           { "tid", cpuTid },
                           { "name", "thread_name" },
                           { "args", { { "name", "CPU render thread" } } } });
        events.push_back({ { "ph", "M" },
                           { "pid", "SaffronAnima" },
                           { "tid", gpuTid },
                           { "name", "thread_name" },
                           { "args", { { "name", "GPU queue" } } } });
        for (const ProfileSpan& s : capture.spans)
        {
            const double tsUs = static_cast<double>(s.startNs) / 1000.0;
            double durUs = 0.0;
            if (s.endNs > s.startNs)
            {
                durUs = static_cast<double>(s.endNs - s.startNs) / 1000.0;
            }
            json args = { { "depth", s.depth } };
            if (s.hasStats)
            {
                args["fragmentInvocations"] = s.stats.fragmentInvocations;
                args["vertexInvocations"] = s.stats.vertexInvocations;
                args["inputVertices"] = s.stats.inputVertices;
                args["clippingInvocations"] = s.stats.clippingInvocations;
                args["clippingPrimitives"] = s.stats.clippingPrimitives;
                args["computeInvocations"] = s.stats.computeInvocations;
                args["pixels"] = s.stats.pixels;
            }
            int laneTid = cpuTid;
            if (s.lane == ProfileLane::Gpu)
            {
                laneTid = gpuTid;
            }
            events.push_back({ { "ph", "X" },
                               { "pid", "SaffronAnima" },
                               { "tid", laneTid },
                               { "name", s.name },
                               { "ts", tsUs },
                               { "dur", durUs },
                               { "args", std::move(args) } });
        }
        const char* modeName = "pipeline-stats";
        if (profilerModeDto(capture.meta.mode) == ProfilerModeDto::Timestamps)
        {
            modeName = "timestamps";
        }
        json doc;
        doc["traceEvents"] = std::move(events);
        doc["displayTimeUnit"] = "ns";
        doc["otherData"] = { { "softwareGpu", capture.meta.softwareGpu },
                             { "correlated", capture.meta.correlated },
                             { "deviceName", capture.meta.deviceName },
                             { "mode", modeName },
                             { "targetFps", capture.meta.targetFps },
                             { "frameCount", capture.meta.frameCount },
                             { "filter", capture.meta.filter } };
        return doc.dump();
    }

    auto renderStatsDto(Renderer& renderer) -> RenderStatsDto
    {
        const RenderStats stats = renderStats(renderer);
        return RenderStatsDto{ .drawCalls = static_cast<i32>(stats.drawCalls),
                               .batches = static_cast<i32>(stats.batches),
                               .instances = static_cast<i32>(stats.instances),
                               .frameMs = stats.frameMs,
                               .fps = stats.fps,
                               .gpuMs = stats.gpuMs,
                               .cpuFrameMs = stats.cpuFrameMs,
                               .gpuFrameMs = stats.gpuMs,
                               .cpuWaitMs = stats.cpuWaitMs,
                               .triangles = static_cast<i32>(stats.triangles),
                               .descriptorBinds = static_cast<i32>(stats.descriptorBinds),
                               .commandBuffers = static_cast<i32>(stats.commandBuffers),
                               .queueSubmits = static_cast<i32>(stats.queueSubmits),
                               .pipelinesCreated = static_cast<i32>(stats.pipelinesCreated),
                               .vramUsageBytes = stats.vramUsageBytes,
                               .vramBudgetBytes = stats.vramBudgetBytes,
                               .softwareGpu = stats.softwareGpu,
                               .profilerMode = profilerModeDto(stats.profilerMode),
                               .clustered = clusteredEnabled(renderer),
                               .depthPrepass = depthPrepassEnabled(renderer),
                               .shadows = shadowsEnabled(renderer),
                               .ibl = iblEnabled(renderer),
                               .ssao = ssaoEnabled(renderer),
                               .contactShadows = contactShadowsEnabled(renderer),
                               .ssgi = ssgiEnabled(renderer),
                               .ddgi = ddgiEnabled(renderer),
                               .rtSupported = rtSupported(renderer),
                               .rtShadows = rtShadowsEnabled(renderer),
                               .restir = restirEnabled(renderer),
                               .blasCount = static_cast<i32>(rtBlasCount(renderer)),
                               .pipelines = static_cast<i32>(pipelineCount(renderer)),
                               .bindlessTextures = static_cast<i32>(bindlessTextureCount(renderer)),
                               .bindlessFree = static_cast<i32>(bindlessFreeCount(renderer)),
                               .hdr = true,
                               .exposureEv = exposureEv(renderer),
                               .aa = aaModeDto(aaMode(renderer)),
                               .viewMode = viewModeDto(viewMode(renderer)) };
    }

    auto passTimingsDto(Renderer& renderer) -> RenderPassTimingsDto
    {
        RenderPassTimingsDto out;
        for (const PassTiming& timing : passTimings(renderer))
        {
            out.passes.push_back(RenderPassTimingDto{ .name = timing.name, .gpuMs = timing.gpuMs });
        }
        out.gpuTotalMs = passTimingsTotalMs(renderer);
        out.softwareGpu = softwareGpu(renderer);
        out.profilerMode = profilerModeDto(profilerMode(renderer));
        return out;
    }

    auto perfConfigDto(const PerfConfig& config) -> PerfConfigDto
    {
        return PerfConfigDto{ .targetFps = config.targetFps,
                              .budgetMs = perfBudgetMs(config),
                              .greenBudgetFrac = config.greenBudgetFrac,
                              .greenMedianMul = config.greenMedianMul,
                              .amberMedianMul = config.amberMedianMul,
                              .frozenMs = config.frozenMs,
                              .vramWarnFrac = config.vramWarnFrac,
                              .vramCritFrac = config.vramCritFrac };
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
                out.samples.push_back(FrameSampleDto{ .frameIndex = static_cast<i64>(sample.frameIndex),
                                                      .cpuMs = sample.cpuMs,
                                                      .gpuMs = sample.gpuMs,
                                                      .cpuWaitMs = sample.cpuWaitMs });
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
        AlarmStateDto state = AlarmStateDto::Firing;
        if (event.kind == AlarmEventKind::Resolved)
        {
            state = AlarmStateDto::Resolved;
        }
        return AlarmEventDto{ .seq = static_cast<i64>(event.seq),
                              .fingerprint = std::to_string(event.fingerprint),
                              .metric = event.metric,
                              .pass = event.pass,
                              .severity = alarmSeverityDto(event.severity),
                              .state = state,
                              .value = event.value,
                              .threshold = event.threshold,
                              .sinceFrame = static_cast<i64>(event.sinceFrame),
                              .count = static_cast<i32>(event.count),
                              .durationMs = event.durationMs };
    }

    auto drainAlarmsDto(Renderer& renderer, i64 since) -> DrainAlarmsResult
    {
        u64 sinceSeq = 0;
        if (since >= 0)
        {
            sinceSeq = static_cast<u64>(since);
        }
        const AlarmDrain drain = drainAlarms(renderer, sinceSeq);
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
            out.alarms.push_back(ActiveAlarmDto{ .fingerprint = std::to_string(alarm.fingerprint),
                                                 .metric = alarm.metric,
                                                 .pass = alarm.pass,
                                                 .severity = alarmSeverityDto(alarm.severity),
                                                 .value = alarm.value,
                                                 .threshold = alarm.threshold,
                                                 .sinceFrame = static_cast<i64>(alarm.sinceFrame),
                                                 .count = static_cast<i32>(alarm.count) });
        }
        return out;
    }

    void registerRenderCommands(CommandRegistry& reg)
    {
        registerCommand<PingParams, PingResult>(reg, "ping", "liveness + engine info",
                                                [](EngineContext&, const PingParams&) -> Result<PingResult>
                                                {
                                                    return PingResult{ .pong = true,
                                                                       .engine = std::string{ EngineName },
                                                                       .version = std::string{ EngineVersion },
                                                                       .pid = static_cast<i32>(::getpid()) };
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
                return ProfilerModeResult{ .mode = profilerModeDto(profilerMode(ctx.renderer)),
                                           .timestampsSupported = profilerTimestampsSupported(ctx.renderer),
                                           .pipelineStatsSupported = profilerPipelineStatsSupported(ctx.renderer),
                                           .softwareGpu = softwareGpu(ctx.renderer) };
            });

        registerCommand<EmptyParams, RenderPassTimingsDto>(
            reg, "pass-timings", "last frame's per-pass GPU timings (needs profiler timestamps mode)",
            [](EngineContext& ctx, const EmptyParams&) -> Result<RenderPassTimingsDto>
            { return passTimingsDto(ctx.renderer); });

        registerCommand<CaptureStartParams, CaptureStartResult>(
            reg, "profiler.capture-start",
            "profiler.capture-start {mode,frames,filter,includeCpu,includePipelineStats} — arm a capture",
            [](EngineContext& ctx, const CaptureStartParams& params) -> Result<CaptureStartResult>
            {
                const CaptureMode mode = captureModeFromDto(params.mode.value_or(CaptureModeDto::Single));
                const u32 frames = static_cast<u32>(std::max(1, params.frames.value_or(60)));
                const u32 id =
                    startProfileCapture(ctx.renderer, mode, frames, params.filter.value_or(std::string{}),
                                        params.includeCpu.value_or(true), params.includePipelineStats.value_or(false));
                return CaptureStartResult{ .captureId = id, .ack = true };
            });

        registerCommand<EmptyParams, CaptureStopResult>(
            reg, "profiler.capture-stop",
            "profiler.capture-stop — finish + return the armed capture (inline single, file for frames:N)",
            [](EngineContext& ctx, const EmptyParams&) -> Result<CaptureStopResult>
            {
                const CaptureMode mode = profileCaptureMode(ctx.renderer);
                const ProfileCapture capture = stopProfileCapture(ctx.renderer);
                CaptureStopResult out;
                out.ready = capture.meta.frameCount > 0;
                out.mode = captureModeDto(mode);
                out.frameCount = capture.meta.frameCount;
                out.pending = false;
                // The structured spans always come back inline so the editor can render any
                // capture. The Chrome-Trace *string* rides inline for a small single-frame
                // capture; for a multi-frame one it is written to a file (path returned) to keep
                // the wire payload bounded.
                out.capture = profileCaptureDto(capture);
                out.inlined = mode == CaptureMode::Single || !out.ready;
                if (out.inlined)
                {
                    if (out.ready)
                    {
                        out.chromeTrace = toChromeTrace(capture);
                    }
                }
                else
                {
                    const std::filesystem::path file =
                        std::filesystem::temp_directory_path() /
                        std::format("saffron-profile-{}.json", static_cast<int>(::getpid()));
                    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
                    if (stream)
                    {
                        stream << toChromeTrace(capture);
                    }
                    out.path = file.string();
                }
                return out;
            });

        registerCommand<EmptyParams, CaptureStatusResult>(
            reg, "profiler.capture-status",
            "profiler.capture-status — non-destructive capture progress (poll until ready, then stop)",
            [](EngineContext& ctx, const EmptyParams&) -> Result<CaptureStatusResult>
            {
                return CaptureStatusResult{ .state = captureStateDto(profileCaptureState(ctx.renderer)),
                                            .capturedFrames = profileCaptureCapturedFrames(ctx.renderer),
                                            .targetFrames = profileCaptureTargetFrames(ctx.renderer),
                                            .mode = captureModeDto(profileCaptureMode(ctx.renderer)),
                                            .pipelineStatsSupported = profileStatsSupported(ctx.renderer) };
            });

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
                return SetAaResult{ .aa = aaModeDto(aaMode(ctx.renderer)) };
            });

        registerCommand<SetViewModeParams, SetViewModeResult>(
            reg, "set-view-mode",
            "set-view-mode {lit|wireframe|albedo|normal|roughness|metallic|emissive} — debug render-output (transient)",
            [](EngineContext& ctx, const SetViewModeParams& params) -> Result<SetViewModeResult>
            {
                setViewMode(ctx.renderer, viewModeFromDto(params.mode.value_or(ViewModeDto::Lit)));
                return SetViewModeResult{ .viewMode = viewModeDto(viewMode(ctx.renderer)) };
            });

        registerCommand<ToggleParams, SetClusteredResult>(
            reg, "set-clustered", "set-clustered {0|1} — toggle clustered light culling",
            [](EngineContext& ctx, const ToggleParams& params) -> Result<SetClusteredResult>
            {
                const bool enabled = params.enabled.value_or(true);
                setClustered(ctx.renderer, enabled);
                return SetClusteredResult{ .clustered = enabled };
            });

        registerCommand<ToggleParams, SetIblResult>(
            reg, "set-ibl", "set-ibl {0|1} — toggle image-based ambient (vs flat ambient)",
            [](EngineContext& ctx, const ToggleParams& params) -> Result<SetIblResult>
            {
                setIbl(ctx.renderer, params.enabled.value_or(true));
                return SetIblResult{ .ibl = iblEnabled(ctx.renderer) };
            });

        registerCommand<ToggleParams, SetSsaoResult>(
            reg, "set-ssao", "set-ssao {0|1} — toggle screen-space ambient occlusion (GTAO)",
            [](EngineContext& ctx, const ToggleParams& params) -> Result<SetSsaoResult>
            {
                setSsao(ctx.renderer, params.enabled.value_or(true));
                return SetSsaoResult{ .ssao = ssaoEnabled(ctx.renderer) };
            });

        registerCommand<ToggleParams, SetContactShadowsResult>(
            reg, "set-contact-shadows", "set-contact-shadows {0|1} — screen-space contact shadows",
            [](EngineContext& ctx, const ToggleParams& params) -> Result<SetContactShadowsResult>
            {
                setContactShadows(ctx.renderer, params.enabled.value_or(true));
                return SetContactShadowsResult{ .contactShadows = contactShadowsEnabled(ctx.renderer) };
            });

        registerCommand<ToggleParams, SetSsgiResult>(
            reg, "set-ssgi", "set-ssgi {0|1} — screen-space one-bounce global illumination",
            [](EngineContext& ctx, const ToggleParams& params) -> Result<SetSsgiResult>
            {
                setSsgi(ctx.renderer, params.enabled.value_or(true));
                return SetSsgiResult{ .ssgi = ssgiEnabled(ctx.renderer) };
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
                return SetRtShadowsResult{ .rtShadows = rtShadowsEnabled(ctx.renderer) };
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
                return SetRestirResult{ .restir = restirEnabled(ctx.renderer) };
            });

        registerCommand<SetGiParams, SetGiResult>(
            reg, "set-gi", "set-gi {off|ddgi} — DDGI probe global illumination (multi-bounce)",
            [](EngineContext& ctx, const SetGiParams& params) -> Result<SetGiResult>
            {
                setDdgi(ctx.renderer, params.mode == GiModeDto::Ddgi);
                return SetGiResult{ .ddgi = ddgiEnabled(ctx.renderer) };
            });

        registerCommand<ToggleParams, SetShadowsResult>(
            reg, "set-shadows", "set-shadows {0|1} — toggle the directional shadow map",
            [](EngineContext& ctx, const ToggleParams& params) -> Result<SetShadowsResult>
            {
                const bool enabled = params.enabled.value_or(true);
                setShadows(ctx.renderer, enabled);
                return SetShadowsResult{ .shadows = enabled };
            });

        registerCommand<ToggleParams, SetSkinningResult>(
            reg, "set-skinning", "set-skinning {0|1} — toggle the GPU skinning path",
            [](EngineContext& ctx, const ToggleParams& params) -> Result<SetSkinningResult>
            {
                const bool enabled = params.enabled.value_or(true);
                setSkinning(ctx.renderer, enabled);
                return SetSkinningResult{ .skinning = enabled };
            });

        registerCommand<SetExposureParams, SetExposureResult>(
            reg, "set-exposure", "set-exposure {ev} — tonemap exposure in stops (exp2)",
            [](EngineContext& ctx, const SetExposureParams& params) -> Result<SetExposureResult>
            {
                setExposure(ctx.renderer, params.ev);
                return SetExposureResult{ .exposureEv = exposureEv(ctx.renderer) };
            });

        registerCommand<ToggleParams, SetDepthPrepassResult>(
            reg, "set-depth-prepass", "set-depth-prepass {0|1} — toggle the depth pre-pass",
            [](EngineContext& ctx, const ToggleParams& params) -> Result<SetDepthPrepassResult>
            {
                const bool enabled = params.enabled.value_or(true);
                setDepthPrepass(ctx.renderer, enabled);
                return SetDepthPrepassResult{ .depthPrepass = enabled };
            });

        registerCommand<EmptyParams, ViewportNativeInfoResult>(
            reg, "viewport-native-info", "native viewport bridge status",
            [](EngineContext& ctx, const EmptyParams&) -> Result<ViewportNativeInfoResult>
            {
                std::string controlPath = controlSocketPath();
                return ViewportNativeInfoResult{ .platform = "linux",
                                                 .transport = "wayland-subsurface",
                                                 .status = "engine-ready",
                                                 .controlSocket = controlPath,
                                                 .width = static_cast<i32>(viewportWidth(ctx.renderer)),
                                                 .height = static_cast<i32>(viewportHeight(ctx.renderer)),
                                                 .message = "engine renders offscreen; the editor presents frames "
                                                            "from shared memory on a wayland subsurface" };
            });

        registerCommand<SetViewportSizeParams, SetViewportSizeResult>(
            reg, "set-viewport-size",
            "set-viewport-size {view, width, height} — set a view's offscreen render size (device pixels)",
            [](EngineContext& ctx, const SetViewportSizeParams& params) -> Result<SetViewportSizeResult>
            {
                auto view = viewIdFromWire(params.view.value_or("scene"));
                if (!view)
                {
                    return Err(view.error());
                }
                const i32 width = std::max(1, params.width.value_or(static_cast<i32>(viewportWidth(ctx.renderer))));
                const i32 height = std::max(1, params.height.value_or(static_cast<i32>(viewportHeight(ctx.renderer))));
                setViewportDesiredSize(ctx.renderer, *view, static_cast<u32>(width), static_cast<u32>(height));
                return SetViewportSizeResult{ .width = width, .height = height };
            });
    }
}
