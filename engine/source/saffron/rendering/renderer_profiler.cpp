module;

// Vulkan-Hpp in no-exceptions mode: every call returns a result we convert to
// Result. We never use vk::raii (it throws). Classic includes (no import std).
#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_NO_SMART_HANDLE
#include <vulkan/vulkan.hpp>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module Saffron.Rendering;

import Saffron.Core;
import Saffron.Window;
import Saffron.Geometry;
import :Detail;

namespace sa
{
    // The pipeline-statistics counters captured per pass, in ascending VkQueryPipelineStatisticFlagBits
    // bit order (the order vkGetQueryPoolResults returns them). The decode in readbackGpuTimings
    // reads them positionally, so this order and PipelineStatsCount must stay in lockstep.
    inline constexpr vk::QueryPipelineStatisticFlags PipelineStatsFlags =
        vk::QueryPipelineStatisticFlagBits::eInputAssemblyVertices |
        vk::QueryPipelineStatisticFlagBits::eVertexShaderInvocations |
        vk::QueryPipelineStatisticFlagBits::eClippingInvocations |
        vk::QueryPipelineStatisticFlagBits::eClippingPrimitives |
        vk::QueryPipelineStatisticFlagBits::eFragmentShaderInvocations |
        vk::QueryPipelineStatisticFlagBits::eComputeShaderInvocations;
    inline constexpr u32 PipelineStatsCount = 6;

    auto allocateProfilerPools(Renderer& renderer) -> bool
    {
        GpuProfiler& prof = renderer.profiler;
        if (prof.poolsReady)
        {
            return true;
        }
        for (vk::QueryPool& pool : prof.timestampPools)
        {
            vk::QueryPoolCreateInfo info{};
            info.queryType = vk::QueryType::eTimestamp;
            info.queryCount = 2 * MaxProfiledScopes;
            auto created = checked(renderer.context.device.createQueryPool(info), "createQueryPool(timestamp)");
            if (!created)
            {
                logError(created.error());
                return false;
            }
            pool = *created;
        }
        // Pipeline-statistics pools (one slot per top-level pass), only if the device feature is
        // present. Absent => statsPools stay null and PipelineStats mode falls back to timestamps.
        if (prof.pipelineStatsSupported)
        {
            for (vk::QueryPool& pool : prof.statsPools)
            {
                vk::QueryPoolCreateInfo info{};
                info.queryType = vk::QueryType::ePipelineStatistics;
                info.queryCount = MaxProfiledScopes;
                info.pipelineStatistics = PipelineStatsFlags;
                auto created = checked(renderer.context.device.createQueryPool(info), "createQueryPool(stats)");
                if (!created)
                {
                    logError(created.error());
                    return false;
                }
                pool = *created;
            }
        }
        prof.poolsReady = true;
        return true;
    }

    void destroyProfilerPools(Renderer& renderer)
    {
        GpuProfiler& prof = renderer.profiler;
        for (vk::QueryPool& pool : prof.timestampPools)
        {
            if (pool)
            {
                renderer.context.device.destroyQueryPool(pool);
                pool = nullptr;
            }
        }
        for (vk::QueryPool& pool : prof.statsPools)
        {
            if (pool)
            {
                renderer.context.device.destroyQueryPool(pool);
                pool = nullptr;
            }
        }
        for (std::vector<ScopeRecord>& records : prof.recordedScopes)
        {
            records.clear();
        }
        prof.poolsReady = false;
    }

    // Sample the device and host clocks together and store the offset that maps a GPU tick onto
    // the CPU steady_clock axis. Cheap (no queue work); called periodically to track drift. A
    // no-op when calibration is unavailable, leaving correlated = false (own-axis fallback).
    void calibrateTimestamps(Renderer& renderer)
    {
        GpuProfiler& prof = renderer.profiler;
        if (!prof.calibration.available || renderer.context.calibratedTs.getTimestamps == nullptr)
        {
            return;
        }
        std::array<VkCalibratedTimestampInfoEXT, 2> infos{};
        infos[0].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT;
        infos[0].timeDomain = VK_TIME_DOMAIN_DEVICE_EXT;
        infos[1].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT;
        infos[1].timeDomain = prof.calibration.hostDomain;
        std::array<u64, 2> samples{};
        u64 maxDeviation = 0;
        const VkResult r = renderer.context.calibratedTs.getTimestamps(renderer.context.device, 2, infos.data(),
                                                                       samples.data(), &maxDeviation);
        if (r != VK_SUCCESS)
        {
            return;
        }
        // samples[0] is device ticks (same units as the query pool); samples[1] is host ns
        // (CLOCK_MONOTONIC). offset = hostNs - deviceNs lets readback add a single term.
        const u64 deviceTick = samples[0] & prof.timestampMask;
        const double deviceNs = static_cast<double>(deviceTick) * prof.timestampPeriod;
        prof.calibration.deviceToHostNsOffset = static_cast<i64>(static_cast<double>(samples[1]) - deviceNs);
        prof.calibration.maxDeviationNs = maxDeviation;
        prof.calibration.correlated = true;
        prof.calibration.lastCalibratedSerial = renderer.frameSerial;
    }

    // Read back slot's timestamp pool (its GPU work completed at the beginFrame fence wait,
    // so this never blocks) into the profiler's last-timings + EMA GPU frame time. Each pass
    // contributes two queries; eWithAvailability gives a [value, available] pair per query.
    void readbackGpuTimings(Renderer& renderer, u32 slot)
    {
        GpuProfiler& prof = renderer.profiler;
        const std::vector<ScopeRecord>& records = prof.recordedScopes[slot];
        if (records.empty() || !prof.timestampPools[slot])
        {
            return;  // nothing recorded into this slot yet (first frames after enabling)
        }
        const u32 scopeCount = static_cast<u32>(records.size());
        const u32 queryCount = 2 * scopeCount;
        std::vector<u64> raw(static_cast<std::size_t>(queryCount) * 2, 0);
        const vk::Result r = renderer.context.device.getQueryPoolResults(
            prof.timestampPools[slot], 0, queryCount, raw.size() * sizeof(u64), raw.data(), 2 * sizeof(u64),
            vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
        if (r != vk::Result::eSuccess && r != vk::Result::eNotReady)
        {
            return;  // keep the last good read-back
        }

        // Pipeline statistics: read the stats pool only if this frame actually recorded stats (a
        // record carries a statsSlot). That implies the frame ran in PipelineStats mode, so the
        // pool was reset in beginFrame — reading an unreset pool is a validation error, so a
        // timestamps-only frame (where the pool exists but was never reset) must not read it.
        std::vector<u64> statsRaw;
        bool statsAvail = false;
        if (prof.statsPools[slot])
        {
            for (const ScopeRecord& rec : records)
            {
                if (rec.statsSlot >= 0)
                {
                    statsAvail = true;
                    break;
                }
            }
        }
        if (statsAvail)
        {
            statsRaw.assign(static_cast<std::size_t>(MaxProfiledScopes) * (PipelineStatsCount + 1), 0);
            static_cast<void>(renderer.context.device.getQueryPoolResults(
                prof.statsPools[slot], 0, MaxProfiledScopes, statsRaw.size() * sizeof(u64), statsRaw.data(),
                (PipelineStatsCount + 1) * sizeof(u64),
                vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability));
        }

        // The i-th record owns query slots 2i (begin) / 2i+1 (end). The frame span is the
        // earliest begin to the latest end across all scopes — NOT a sum, since sibling/async
        // scopes overlap and a parent brackets its children, so a nested last-record-end is wrong.
        u64 spanBegin = 0;
        u64 spanEnd = 0;
        bool spanValid = false;
        for (u32 i = 0; i < scopeCount; i = i + 1)
        {
            if (raw[4 * i + 1] != 0 && raw[4 * i + 3] != 0)
            {
                const u64 b = raw[4 * i] & prof.timestampMask;
                const u64 e = raw[4 * i + 2] & prof.timestampMask;
                if (!spanValid)
                {
                    spanBegin = b;
                    spanEnd = e;
                    spanValid = true;
                }
                spanBegin = std::min(spanBegin, b);
                spanEnd = std::max(spanEnd, e);
            }
        }

        std::vector<PassTiming> timings;
        timings.reserve(scopeCount);
        for (u32 i = 0; i < scopeCount; i = i + 1)
        {
            const ScopeRecord& rec = records[i];
            PassTiming t;
            t.name = rec.name;
            t.parentIndex = rec.parentIndex;
            t.depth = rec.depth;
            if (raw[4 * i + 1] != 0 && raw[4 * i + 3] != 0)
            {
                const u64 b = raw[4 * i] & prof.timestampMask;
                const u64 e = raw[4 * i + 2] & prof.timestampMask;
                u64 ticks = 0;
                if (e >= b)
                {
                    ticks = e - b;
                }
                t.gpuMs = static_cast<f32>(static_cast<double>(ticks) * prof.timestampPeriod / 1.0e6);
                // Correlated: project onto the CPU steady_clock axis (absolute host ns) so the GPU
                // and CPU lanes share one zero. Otherwise stay frame-relative (own-axis fallback).
                if (prof.calibration.correlated)
                {
                    const double offset = static_cast<double>(prof.calibration.deviceToHostNsOffset);
                    t.startNs = static_cast<u64>((static_cast<double>(b) * prof.timestampPeriod) + offset);
                    t.endNs = static_cast<u64>((static_cast<double>(e) * prof.timestampPeriod) + offset);
                }
                else
                {
                    if (spanValid && b >= spanBegin)
                    {
                        t.startNs = static_cast<u64>(static_cast<double>(b - spanBegin) * prof.timestampPeriod);
                    }
                    if (spanValid && e >= spanBegin)
                    {
                        t.endNs = static_cast<u64>(static_cast<double>(e - spanBegin) * prof.timestampPeriod);
                    }
                }
            }
            if (statsAvail && rec.statsSlot >= 0)
            {
                const std::size_t base = static_cast<std::size_t>(rec.statsSlot) * (PipelineStatsCount + 1);
                if (statsRaw[base + PipelineStatsCount] != 0)  // availability word
                {
                    t.hasStats = true;
                    t.stats.inputVertices = statsRaw[base + 0];
                    t.stats.vertexInvocations = statsRaw[base + 1];
                    t.stats.clippingInvocations = statsRaw[base + 2];
                    t.stats.clippingPrimitives = statsRaw[base + 3];
                    t.stats.fragmentInvocations = statsRaw[base + 4];
                    t.stats.computeInvocations = statsRaw[base + 5];
                    t.stats.pixels = rec.pixels;
                }
            }
            timings.push_back(std::move(t));
        }
        prof.lastTimings = std::move(timings);
        if (spanValid && spanEnd >= spanBegin)
        {
            prof.lastGpuTotalMs =
                static_cast<f32>(static_cast<double>(spanEnd - spanBegin) * prof.timestampPeriod / 1.0e6);
        }
        else
        {
            prof.lastGpuTotalMs = 0.0f;
        }
        if (renderer.gpuFrameMs == 0.0f)
        {
            renderer.gpuFrameMs = prof.lastGpuTotalMs;
        }
        else
        {
            renderer.gpuFrameMs = renderer.gpuFrameMs * 0.9f + prof.lastGpuTotalMs * 0.1f;
        }
    }

    // Sum the VMA budget across device-local heaps (cheap, unlike vmaCalculateStatistics).
    void readVramBudget(Renderer& renderer)
    {
        const VkPhysicalDeviceMemoryProperties* memProps = nullptr;
        vmaGetMemoryProperties(renderer.context.allocator, &memProps);
        std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
        vmaGetHeapBudgets(renderer.context.allocator, budgets.data());
        u64 usage = 0;
        u64 budget = 0;
        for (u32 h = 0; h < memProps->memoryHeapCount; h = h + 1)
        {
            if ((memProps->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
            {
                usage = usage + budgets[h].usage;
                budget = budget + budgets[h].budget;
            }
        }
        renderer.stats.vramUsageBytes = usage;
        renderer.stats.vramBudgetBytes = budget;
    }

    void setProfilerMode(Renderer& renderer, ProfilerMode mode)
    {
        // Degrade a request the device cannot satisfy: no timestamps ⇒ off; no pipeline
        // statistics ⇒ fall back to plain timestamps.
        if (mode != ProfilerMode::Off && !renderer.profiler.timestampsSupported)
        {
            mode = ProfilerMode::Off;
        }
        if (mode == ProfilerMode::PipelineStats && !renderer.profiler.pipelineStatsSupported)
        {
            mode = ProfilerMode::Timestamps;
        }
        if (mode != ProfilerMode::Off && !renderer.profiler.poolsReady && !allocateProfilerPools(renderer))
        {
            mode = ProfilerMode::Off;
        }
        renderer.profiler.mode = mode;
        if (mode == ProfilerMode::Off)
        {
            renderer.profiler.lastTimings.clear();
            renderer.profiler.lastGpuTotalMs = 0.0f;
            renderer.gpuFrameMs = 0.0f;
            renderer.profiler.calibration.correlated = false;  // re-enable forces a fresh calibration
        }
    }

    auto profilerMode(const Renderer& renderer) -> ProfilerMode
    {
        return renderer.profiler.mode;
    }

    auto profilerTimestampsSupported(const Renderer& renderer) -> bool
    {
        return renderer.profiler.timestampsSupported;
    }

    auto profilerPipelineStatsSupported(const Renderer& renderer) -> bool
    {
        return renderer.profiler.pipelineStatsSupported;
    }

    auto softwareGpu(const Renderer& renderer) -> bool
    {
        return renderer.softwareGpu;
    }

    auto passTimings(const Renderer& renderer) -> std::vector<PassTiming>
    {
        return renderer.profiler.lastTimings;
    }

    auto passTimingsTotalMs(const Renderer& renderer) -> f32
    {
        return renderer.profiler.lastGpuTotalMs;
    }

    // Copy this slot's finalized frame into the capture: the CPU lane (buffers[slot], still
    // intact at the read-back seam) then the GPU lane (lastTimings, just read). parentIndex is
    // rebased into the merged span vector so each lane's tree stays intact across frames.
    void appendCaptureFrame(Renderer& renderer)
    {
        CaptureRecorder& cap = renderer.captureRecorder;
        const std::vector<std::string>& names = renderer.cpuProfiler.registry.names;
        const u32 base = static_cast<u32>(cap.capture.spans.size());
        u32 cpuCount = 0;
        if (cap.includeCpu)
        {
            for (const CpuSpan& s : renderer.cpuProfiler.buffers[renderer.frame.index].spans)
            {
                ProfileSpan ps;
                ps.name = names[s.marker];
                ps.lane = ProfileLane::Cpu;
                ps.startNs = s.startNs;
                ps.endNs = s.endNs;
                ps.depth = s.depth;
                if (s.parent >= 0)
                {
                    ps.parentIndex = static_cast<i32>(base) + s.parent;
                }
                else
                {
                    ps.parentIndex = -1;
                }
                cap.capture.spans.push_back(std::move(ps));
                cpuCount = cpuCount + 1;
            }
        }
        const u32 gpuBase = base + cpuCount;
        for (const PassTiming& t : renderer.profiler.lastTimings)
        {
            ProfileSpan ps;
            ps.name = t.name;
            ps.lane = ProfileLane::Gpu;
            ps.startNs = t.startNs;
            ps.endNs = t.endNs;
            ps.depth = t.depth;
            if (t.parentIndex >= 0)
            {
                ps.parentIndex = static_cast<i32>(gpuBase) + t.parentIndex;
            }
            else
            {
                ps.parentIndex = -1;
            }
            ps.hasStats = t.hasStats;
            ps.stats = t.stats;
            cap.capture.spans.push_back(std::move(ps));
        }
    }

    // Advance the capture state machine once per finalized frame (at the read-back seam). Arming
    // burns down the read-back delay so recorded frames reflect the arm-time settings.
    void tickCapture(Renderer& renderer)
    {
        CaptureRecorder& cap = renderer.captureRecorder;
        if (cap.state == CaptureState::Arming)
        {
            if (cap.warmup > 0)
            {
                cap.warmup = cap.warmup - 1;
            }
            if (cap.warmup == 0)
            {
                cap.state = CaptureState::Recording;
            }
            return;
        }
        if (cap.state == CaptureState::Recording)
        {
            appendCaptureFrame(renderer);
            cap.capturedFrames = cap.capturedFrames + 1;
            if (cap.capturedFrames >= cap.targetFrames)
            {
                cap.state = CaptureState::Ready;
            }
        }
    }

    auto startProfileCapture(Renderer& renderer, CaptureMode mode, u32 frames, std::string filter, bool includeCpu,
                             bool includeStats) -> u32
    {
        CaptureRecorder& cap = renderer.captureRecorder;
        cap.mode = mode;
        if (mode == CaptureMode::Single)
        {
            cap.targetFrames = 1u;
        }
        else
        {
            cap.targetFrames = std::clamp(frames, 1u, MaxCaptureFrames);
        }
        cap.capturedFrames = 0;
        cap.warmup = MaxFramesInFlight + 1;  // flush the read-back delay before recording
        cap.filter = std::move(filter);
        cap.includeCpu = includeCpu;
        cap.includeStats = includeStats && renderer.profiler.pipelineStatsSupported;
        cap.capture = ProfileCapture{};
        cap.captureId = cap.nextCaptureId;
        cap.nextCaptureId = cap.nextCaptureId + 1;
        cap.priorMode = renderer.profiler.mode;
        cap.priorSubScopes = renderer.profiler.subScopes;
        // PipelineStats mode is the heaviest level (it adds the per-pass statistics queries on
        // top of timestamps); request it only when stats are wanted + supported, else timestamps.
        ProfilerMode wanted = ProfilerMode::Timestamps;
        if (cap.includeStats)
        {
            wanted = ProfilerMode::PipelineStats;
        }
        if (renderer.profiler.mode != wanted)
        {
            setProfilerMode(renderer, wanted);
        }
        renderer.profiler.subScopes = true;  // capture the full nested tree; restored on stop
        cap.state = CaptureState::Arming;
        return cap.captureId;
    }

    auto profileStatsSupported(const Renderer& renderer) -> bool
    {
        return renderer.profiler.pipelineStatsSupported;
    }

    auto profileCaptureState(const Renderer& renderer) -> CaptureState
    {
        return renderer.captureRecorder.state;
    }

    auto profileCaptureMode(const Renderer& renderer) -> CaptureMode
    {
        return renderer.captureRecorder.mode;
    }

    auto profileCaptureCapturedFrames(const Renderer& renderer) -> u32
    {
        return renderer.captureRecorder.capturedFrames;
    }

    auto profileCaptureTargetFrames(const Renderer& renderer) -> u32
    {
        return renderer.captureRecorder.targetFrames;
    }

    auto stopProfileCapture(Renderer& renderer) -> ProfileCapture
    {
        CaptureRecorder& cap = renderer.captureRecorder;
        const bool wasActive = cap.state != CaptureState::Idle;
        ProfileCapture out = std::move(cap.capture);
        out.meta.frameCount = cap.capturedFrames;
        out.meta.filter = cap.filter;
        out.meta.softwareGpu = renderer.softwareGpu;
        out.meta.correlated = renderer.profiler.calibration.correlated;
        out.meta.deviceName = renderer.context.vkbDevice.physical_device.name;
        out.meta.timestampPeriod = renderer.profiler.timestampPeriod;
        out.meta.targetFps = renderer.perfConfig.targetFps;
        out.meta.mode = renderer.profiler.mode;
        if (wasActive)
        {
            renderer.profiler.subScopes = cap.priorSubScopes;
            if (cap.priorMode != renderer.profiler.mode)
            {
                setProfilerMode(renderer, cap.priorMode);
            }
        }
        cap.capture = ProfileCapture{};
        cap.capturedFrames = 0;
        cap.state = CaptureState::Idle;
        return out;
    }

    auto perfBudgetMs(const PerfConfig& config) -> f32
    {
        if (config.targetFps > 0.0f)
        {
            return 1000.0f / config.targetFps;
        }
        return 0.0f;
    }

    auto perfConfig(const Renderer& renderer) -> PerfConfig
    {
        return renderer.perfConfig;
    }

    void setPerfConfig(Renderer& renderer, const PerfConfig& config)
    {
        PerfConfig c = config;
        c.targetFps = std::clamp(c.targetFps, 1.0f, 10000.0f);
        c.greenBudgetFrac = std::clamp(c.greenBudgetFrac, 0.0f, 1.0f);
        c.greenMedianMul = std::max(1.0f, c.greenMedianMul);
        c.amberMedianMul = std::max(c.greenMedianMul, c.amberMedianMul);
        c.frozenMs = std::max(0.0f, c.frozenMs);
        c.vramWarnFrac = std::clamp(c.vramWarnFrac, 0.0f, 1.0f);
        c.vramCritFrac = std::clamp(c.vramCritFrac, c.vramWarnFrac, 1.0f);
        renderer.perfConfig = c;
    }

    // The ring's oldest→newest physical index for logical position i in [0, count).
    auto frameRingIndex(const Renderer& renderer, u32 i) -> u32
    {
        const u32 start =
            (renderer.frameRingHead + FrameHistoryCapacity - renderer.frameRingCount) % FrameHistoryCapacity;
        return (start + i) % FrameHistoryCapacity;
    }

    auto frameHistoryStats(const Renderer& renderer) -> FrameHistoryStats
    {
        FrameHistoryStats out;
        out.sampleCount = renderer.frameRingCount;
        out.stutterCount = renderer.stutterCount;
        if (renderer.frameRingCount == 0)
        {
            return out;
        }
        // Frame time = cpu busy + fence wait (the render-thread wall clock).
        std::vector<f32> times;
        times.reserve(renderer.frameRingCount);
        f32 sum = 0.0f;
        for (u32 i = 0; i < renderer.frameRingCount; i = i + 1)
        {
            const FrameSample& s = renderer.frameRing[frameRingIndex(renderer, i)];
            const f32 t = s.cpuMs + s.cpuWaitMs;
            times.push_back(t);
            sum = sum + t;
        }
        out.meanMs = sum / static_cast<f32>(times.size());
        f32 variance = 0.0f;
        for (const f32 t : times)
        {
            const f32 d = t - out.meanMs;
            variance = variance + d * d;
        }
        out.stddevMs = std::sqrt(variance / static_cast<f32>(times.size()));
        std::sort(times.begin(), times.end());
        const auto percentile = [&times](f32 p) -> f32
        {
            const std::size_t last = times.size() - 1;
            const std::size_t idx = static_cast<std::size_t>(p * static_cast<f32>(last) + 0.5f);
            return times[std::min(idx, last)];
        };
        out.p50Ms = percentile(0.50f);
        out.p95Ms = percentile(0.95f);
        out.p99Ms = percentile(0.99f);
        out.p999Ms = percentile(0.999f);
        out.maxMs = times.back();
        return out;
    }

    auto frameSamples(const Renderer& renderer, u32 maxSamples) -> std::vector<FrameSample>
    {
        const u32 take = std::min(maxSamples, renderer.frameRingCount);
        std::vector<FrameSample> out;
        out.reserve(take);
        for (u32 i = renderer.frameRingCount - take; i < renderer.frameRingCount; i = i + 1)
        {
            out.push_back(renderer.frameRing[frameRingIndex(renderer, i)]);
        }
        return out;
    }

    // FNV-1a over metric + "|" + pass — the alarm fingerprint that coalesces repeats.
    auto alarmFingerprint(std::string_view metric, std::string_view pass) -> u64
    {
        u64 hash = 1469598103934665603ULL;
        const auto mix = [&hash](std::string_view text)
        {
            for (const char c : text)
            {
                hash = hash ^ static_cast<u8>(c);
                hash = hash * 1099511628211ULL;
            }
        };
        mix(metric);
        mix("|");
        mix(pass);
        return hash;
    }

    auto pushAlarmEvent(AlarmState& alarms, AlarmEvent event) -> u64
    {
        const u64 seq = alarms.nextSeq;
        event.seq = seq;
        alarms.nextSeq = alarms.nextSeq + 1;
        alarms.events[alarms.eventHead] = std::move(event);
        alarms.eventHead = (alarms.eventHead + 1) % AlarmEventRingCapacity;
        if (alarms.eventCount < AlarmEventRingCapacity)
        {
            alarms.eventCount = alarms.eventCount + 1;
        }
        return seq;
    }

    // Raise (or refresh) an alarm. The first breach emits one FIRING; while active the
    // count/peak update in place and only a severity escalation emits another FIRING.
    void raiseAlarm(AlarmState& alarms, u64 nowNs, std::string metric, std::string pass, AlarmSeverity severity,
                    f32 value, f32 threshold)
    {
        const u64 fingerprint = alarmFingerprint(metric, pass);
        for (ActiveAlarm& active : alarms.active)
        {
            if (active.fingerprint == fingerprint)
            {
                active.lastSeenNs = nowNs;
                active.count = active.count + 1;
                active.value = value;
                active.threshold = threshold;
                active.peak = std::max(active.peak, value);
                if (static_cast<int>(severity) > static_cast<int>(active.severity))
                {
                    active.severity = severity;
                    AlarmEvent escalation;
                    escalation.fingerprint = fingerprint;
                    escalation.metric = metric;
                    escalation.pass = pass;
                    escalation.severity = severity;
                    escalation.kind = AlarmEventKind::Firing;
                    escalation.value = value;
                    escalation.threshold = threshold;
                    escalation.sinceFrame = active.sinceFrame;
                    escalation.count = active.count;
                    pushAlarmEvent(alarms, std::move(escalation));
                }
                return;
            }
        }
        ActiveAlarm fresh;
        fresh.fingerprint = fingerprint;
        fresh.metric = metric;
        fresh.pass = pass;
        fresh.severity = severity;
        fresh.value = value;
        fresh.threshold = threshold;
        fresh.peak = value;
        fresh.sinceFrame = alarms.frameCounter;
        fresh.sinceNs = nowNs;
        fresh.lastSeenNs = nowNs;
        fresh.count = 1;
        alarms.active.push_back(fresh);
        AlarmEvent firing;
        firing.fingerprint = fingerprint;
        firing.metric = std::move(metric);
        firing.pass = std::move(pass);
        firing.severity = severity;
        firing.kind = AlarmEventKind::Firing;
        firing.value = value;
        firing.threshold = threshold;
        firing.sinceFrame = fresh.sinceFrame;
        firing.count = 1;
        pushAlarmEvent(alarms, std::move(firing));
    }

    // Clear an active alarm if present, emitting one RESOLVED (with duration + peak).
    void clearAlarm(AlarmState& alarms, u64 nowNs, std::string_view metric, std::string_view pass)
    {
        const u64 fingerprint = alarmFingerprint(metric, pass);
        for (auto it = alarms.active.begin(); it != alarms.active.end(); ++it)
        {
            if (it->fingerprint == fingerprint)
            {
                AlarmEvent resolved;
                resolved.fingerprint = fingerprint;
                resolved.metric = it->metric;
                resolved.pass = it->pass;
                resolved.severity = it->severity;
                resolved.kind = AlarmEventKind::Resolved;
                resolved.value = it->peak;
                resolved.threshold = it->threshold;
                resolved.sinceFrame = it->sinceFrame;
                resolved.count = it->count;
                resolved.durationMs = static_cast<f32>(nowNs - it->sinceNs) / 1.0e6f;
                pushAlarmEvent(alarms, std::move(resolved));
                alarms.active.erase(it);
                return;
            }
        }
    }

    // The mean over a recent frame-time window — the fraction-over-budget SLI for burn-rate.
    auto frameWindowOverBudget(const Renderer& renderer, u32 window, f32 budget) -> f32
    {
        const u32 w = std::min(window, renderer.frameRingCount);
        if (w == 0)
        {
            return 0.0f;
        }
        u32 over = 0;
        for (u32 i = renderer.frameRingCount - w; i < renderer.frameRingCount; i = i + 1)
        {
            const FrameSample& s = renderer.frameRing[frameRingIndex(renderer, i)];
            if (s.cpuMs + s.cpuWaitMs > budget)
            {
                over = over + 1;
            }
        }
        return static_cast<f32>(over) / static_cast<f32>(w);
    }

    // One per-frame alarm tick: smooth, then gate. Runs detectors on the smoothed series
    // (never raw per-frame values) and against the shared PerfConfig thresholds.
    void tickAlarms(Renderer& renderer, f32 frameTimeMs, f32 dtSec, u64 nowNs)
    {
        AlarmState& alarms = renderer.alarms;
        alarms.frameCounter = alarms.frameCounter + 1;
        const f32 budget = perfBudgetMs(renderer.perfConfig);

        // Irregular-interval EMA (tau ≈ 300 ms): alpha = 1 − exp(−dt / tau).
        if (dtSec > 0.0f)
        {
            const f32 alpha = 1.0f - std::exp(-dtSec / 0.3f);
            if (alarms.emaFrameMs == 0.0f)
            {
                alarms.emaFrameMs = frameTimeMs;
            }
            else
            {
                alarms.emaFrameMs = alarms.emaFrameMs + alpha * (frameTimeMs - alarms.emaFrameMs);
            }
        }
        else if (alarms.emaFrameMs == 0.0f)
        {
            alarms.emaFrameMs = frameTimeMs;
        }

        // frame-budget: sustained over-budget. Hysteresis (enter 1.2× / exit 1.0× budget) +
        // a debounce so a one-frame breach never fires; escalates to critical at 2× budget.
        if (budget > 0.0f)
        {
            const f32 enterTh = 1.2f * budget;
            const f32 exitTh = 1.0f * budget;
            const f32 criticalTh = 2.0f * budget;  // ~< 30 FPS at a 60 Hz budget
            if (alarms.emaFrameMs > enterTh)
            {
                alarms.budgetWarnHeldSec = alarms.budgetWarnHeldSec + dtSec;
            }
            else
            {
                alarms.budgetWarnHeldSec = 0.0f;
            }
            if (alarms.emaFrameMs > criticalTh)
            {
                alarms.budgetCritHeldSec = alarms.budgetCritHeldSec + dtSec;
            }
            else
            {
                alarms.budgetCritHeldSec = 0.0f;
            }
            const bool warnReady = alarms.budgetWarnHeldSec >= 0.3f;
            const bool critReady = alarms.budgetCritHeldSec >= 0.5f;
            if (warnReady)
            {
                AlarmSeverity severity = AlarmSeverity::Warning;
                if (critReady)
                {
                    severity = AlarmSeverity::Critical;
                }
                raiseAlarm(alarms, nowNs, "frame-budget", "", severity, alarms.emaFrameMs, enterTh);
            }
            else if (alarms.emaFrameMs < exitTh)
            {
                clearAlarm(alarms, nowNs, "frame-budget", "");
            }
        }

        // frame-hitch: a robust spike. Modified z-score over a recent window — median/MAD
        // beat mean/stddev because the outlier inflates stddev and masks itself.
        const u32 window = std::min<u32>(renderer.frameRingCount, 64);
        if (window >= 8)
        {
            std::vector<f32> times;
            times.reserve(window);
            for (u32 i = renderer.frameRingCount - window; i < renderer.frameRingCount; i = i + 1)
            {
                const FrameSample& s = renderer.frameRing[frameRingIndex(renderer, i)];
                times.push_back(s.cpuMs + s.cpuWaitMs);
            }
            std::vector<f32> sorted = times;
            std::sort(sorted.begin(), sorted.end());
            const f32 median = sorted[sorted.size() / 2];
            for (f32& v : sorted)
            {
                v = std::fabs(v - median);
            }
            std::sort(sorted.begin(), sorted.end());
            const f32 mad = std::max(sorted[sorted.size() / 2], 0.05f);  // floor guards MAD == 0
            const f32 modZ = 0.6745f * (frameTimeMs - median) / mad;
            // A spike is only a hitch if it also blew the frame budget in absolute terms —
            // a statistical outlier at 2 ms (≈ 500 FPS) is not worth flagging. Modest
            // single-frame budget misses are info (log only); a hard spike (> 2× budget,
            // ≈ < 30 FPS for one frame) is a warning that toasts.
            if (modZ > 3.5f && budget > 0.0f && frameTimeMs > budget)
            {
                alarms.hitchClearFrames = 0;
                AlarmSeverity severity = AlarmSeverity::Info;
                if (frameTimeMs > 2.0f * budget)
                {
                    severity = AlarmSeverity::Warning;
                }
                raiseAlarm(alarms, nowNs, "frame-hitch", "", severity, frameTimeMs, median + mad * 3.5f / 0.6745f);
            }
            else
            {
                alarms.hitchClearFrames = alarms.hitchClearFrames + 1;
                if (alarms.hitchClearFrames >= 10)
                {
                    clearAlarm(alarms, nowNs, "frame-hitch", "");
                }
            }
        }

        // burn-rate: the sustained-user-pain SLI. A short and a long window must both
        // breach (fast detect, low false-positive, clears quickly when the problem stops).
        if (budget > 0.0f && renderer.frameRingCount >= 60)
        {
            const f32 sliShort = frameWindowOverBudget(renderer, 60, budget);  // ~1 s @ 60 Hz
            const f32 sliLong = frameWindowOverBudget(renderer, 600, budget);  // ~10 s
            if (sliShort > 0.5f && sliLong > 0.5f)
            {
                raiseAlarm(alarms, nowNs, "burn-rate", "", AlarmSeverity::Critical, sliShort * 100.0f, 50.0f);
            }
            else if (sliShort > 0.1f && sliLong > 0.1f)
            {
                raiseAlarm(alarms, nowNs, "burn-rate", "", AlarmSeverity::Warning, sliShort * 100.0f, 10.0f);
            }
            else if (sliShort < 0.05f)
            {
                clearAlarm(alarms, nowNs, "burn-rate", "");
            }
        }

        // vram: usage fraction of the device-local budget (only known when profiling).
        if (renderer.stats.vramBudgetBytes > 0)
        {
            const f32 frac =
                static_cast<f32>(renderer.stats.vramUsageBytes) / static_cast<f32>(renderer.stats.vramBudgetBytes);
            if (frac >= renderer.perfConfig.vramCritFrac)
            {
                raiseAlarm(alarms, nowNs, "vram", "", AlarmSeverity::Critical, frac * 100.0f,
                           renderer.perfConfig.vramCritFrac * 100.0f);
            }
            else if (frac >= renderer.perfConfig.vramWarnFrac)
            {
                raiseAlarm(alarms, nowNs, "vram", "", AlarmSeverity::Warning, frac * 100.0f,
                           renderer.perfConfig.vramWarnFrac * 100.0f);
            }
            else if (frac < renderer.perfConfig.vramWarnFrac * 0.95f)
            {
                clearAlarm(alarms, nowNs, "vram", "");
            }
        }

        // pso-compile: a PSO built mid-frame is a hitch on a steady-state frame (info only).
        if (renderer.stats.pipelinesCreated > 0)
        {
            raiseAlarm(alarms, nowNs, "pso-compile", "", AlarmSeverity::Info,
                       static_cast<f32>(renderer.stats.pipelinesCreated), 0.0f);
        }
        else
        {
            clearAlarm(alarms, nowNs, "pso-compile", "");
        }
    }

    auto drainAlarms(const Renderer& renderer, u64 since) -> AlarmDrain
    {
        const AlarmState& alarms = renderer.alarms;
        AlarmDrain out;
        out.highWaterSeq = alarms.nextSeq - 1;
        u64 oldest = 0;
        if (alarms.eventCount > 0)
        {
            const u32 oldestIdx =
                (alarms.eventHead + AlarmEventRingCapacity - alarms.eventCount) % AlarmEventRingCapacity;
            oldest = alarms.events[oldestIdx].seq;
        }
        out.oldestSeq = oldest;
        // Events (since+1 .. oldest-1) fell off the ring: the client must resync from the active set.
        out.overflowed = oldest > since + 1;
        for (u32 i = 0; i < alarms.eventCount; i = i + 1)
        {
            const u32 idx =
                (alarms.eventHead + AlarmEventRingCapacity - alarms.eventCount + i) % AlarmEventRingCapacity;
            if (alarms.events[idx].seq > since)
            {
                out.events.push_back(alarms.events[idx]);
            }
        }
        return out;
    }

    auto activeAlarms(const Renderer& renderer) -> std::vector<ActiveAlarm>
    {
        return renderer.alarms.active;
    }
}
