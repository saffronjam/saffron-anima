module;

// Heavy C++ header in the global module fragment (no import std), same Vulkan-Hpp
// configuration as the primary interface unit (renderer.cppm).
#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_NO_SMART_HANDLE
#include <vulkan/vulkan.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module Saffron.Rendering:RenderGraph;

import Saffron.Core;

export namespace se
{
    /// What a pass does with a resource. The single source of truth for barrier and
    /// layout-transition derivation — a pass declares usage, never writes a barrier.
    enum class RgUsage
    {
        ColorWrite,             ///< color attachment write
        DepthWrite,             ///< depth attachment write
        SampledRead,            ///< sampled in a fragment shader
        StorageWriteCompute,    ///< storage buffer written by a compute shader
        StorageReadCompute,     ///< storage buffer read by a compute shader
        StorageReadFragment,    ///< storage buffer read by a fragment shader
        StorageImageRWCompute,  ///< image read+written in place by a compute shader (GENERAL)
        SampledReadCompute,     ///< image sampled in a compute shader (SHADER_READ_ONLY)
        VertexInputRead,        ///< buffer read as a vertex stream (the compute-skinned deformed buffer)
    };

    enum class RgPassKind
    {
        Graphics,
        Compute,
    };

    /// A handle to a graph resource: an index into the graph's resource table.
    struct RgResource
    {
        u32 index = 0;
    };

    /// A declared (resource, usage) pair — a pass's non-attachment reads/writes.
    struct RgAccess
    {
        RgResource resource;
        RgUsage usage = RgUsage::SampledRead;
    };

    /// A color or depth attachment binding for a graphics pass. The write usage and
    /// the layout transition are derived; only the load/store/clear are declared here.
    /// `resolve` is an MSAA resolve target: the multisampled attachment is resolved into it
    /// at end-of-pass (color averaged, depth via sample 0); the graph treats it as a second
    /// write of the matching kind.
    struct RgAttachment
    {
        RgResource resource;
        vk::AttachmentLoadOp loadOp = vk::AttachmentLoadOp::eClear;
        vk::AttachmentStoreOp storeOp = vk::AttachmentStoreOp::eStore;
        vk::ClearValue clearValue{};
        std::optional<RgResource> resolve;
    };

    /// A unit of GPU work: its declared resource usage plus the closure that records
    /// it. The graph derives the barriers/layout transitions the body needs, opens the
    /// rendering scope (graphics passes), runs the body, then closes the scope.
    struct RgPass
    {
        std::string name;
        RgPassKind kind = RgPassKind::Graphics;
        std::vector<RgAccess> accesses;
        std::vector<RgAttachment> colors;  // MRT: index 0 is location 0, etc.
        std::optional<RgAttachment> depth;
        vk::Extent2D renderArea{};
        std::function<void(vk::CommandBuffer)> execute;
    };

    /// Per-resource tracked state, advanced as passes are recorded in order.
    struct RgResourceState
    {
        bool isImage = false;
        vk::Image image;
        vk::ImageView view;
        vk::Buffer buffer;
        vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
        vk::PipelineStageFlags2 lastStage = vk::PipelineStageFlagBits2::eTopOfPipe;
        vk::AccessFlags2 lastAccess = vk::AccessFlagBits2::eNone;
        bool lastWasWrite = false;
        bool touched = false;
        vk::ImageLayout* externalLayout = nullptr;  // cross-frame layout write-back
    };

    /// A frame's render graph: imported resources + the passes over them. Rebuilt
    /// every frame (cheap) and recorded by executeRenderGraph.
    struct RenderGraph
    {
        std::vector<RgResourceState> resources;
        std::vector<RgPass> passes;
    };

    auto newRenderGraph() -> RenderGraph;

    /// Import an external image (offscreen/swapchain target). When externalLayout is
    /// set it seeds the entry layout and receives the resolved layout after execute,
    /// so an image's layout carries across frames.
    auto importImage(RenderGraph& graph, vk::Image image, vk::ImageView view, vk::ImageAspectFlags aspect,
                     vk::ImageLayout initialLayout, vk::ImageLayout* externalLayout) -> RgResource;

    /// Import an external 3D image (e.g. the DDGI voxel proxy). Only used for compute
    /// storage barriers — the graph tracks its layout exactly like a 2D image.
    auto importImage3D(RenderGraph& graph, vk::Image image, vk::ImageView view, vk::ImageLayout initialLayout,
                       vk::ImageLayout* externalLayout) -> RgResource;

    /// Import an external buffer produced and/or consumed within the frame.
    auto importBuffer(RenderGraph& graph, vk::Buffer buffer) -> RgResource;

    void addPass(RenderGraph& graph, RgPass pass);

    /// One recorded GPU scope: its name and nesting (depth + the enclosing scope's index in
    /// the same list, or -1). Kept flat-and-tagged, not a literal tree — async compute makes a
    /// single nested wall-clock tree ambiguous, so the consumer decodes the tree at read-back.
    /// The i-th record owns query slots 2i (begin) and 2i+1 (end).
    struct ScopeRecord
    {
        std::string name;
        i32 parentIndex = -1;  ///< enclosing scope's record index, or -1 at top level
        u32 depth = 0;
        i32 statsSlot = -1;  ///< pipeline-stats query slot (top-level passes only), or -1
        u64 pixels = 0;      ///< render-area pixels at stats time, for the overdraw ratio
    };

    /// The GPU timestamp recorder for executeRenderGraph and pass-body sub-scopes. A GpuScope
    /// grabs the next free query-slot pair on enter (begin) and exit (end), pushing a
    /// ScopeRecord so the nesting *is* the hierarchy. `pool`/`capacity`/`records` are set per
    /// frame by the renderer; `nextSlot`/`openScope`/`depth` are the transient recording cursor,
    /// reset each frame. A null pool disables recording. The renderer owns it; the graph and
    /// pass bodies only record into it (the "no pass writes a query by hand" analogue).
    struct RgTimestamps
    {
        vk::QueryPool pool;                           ///< null => timestamp capture disabled
        u32 capacity = 0;                             ///< total query slots in the pool (2 per scope)
        std::vector<ScopeRecord>* records = nullptr;  ///< receives one record per scope, in begin order
        u32 nextSlot = 0;                             ///< next free query slot (begin = nextSlot, end = +1)
        i32 openScope = -1;                           ///< innermost open scope's record index, or -1
        u32 depth = 0;                                ///< current nesting depth
        // Pipeline-statistics queries (PipelineStats mode): one per top-level pass, bracketing
        // the pass body *inside* the rendering scope (a stats query cannot straddle
        // beginRendering/endRendering, unlike a timestamp). Null pool => no stats queries.
        vk::QueryPool statsPool;  ///< null => no pipeline-stats queries
        u32 statsCapacity = 0;    ///< total stats query slots
        u32 nextStatsSlot = 0;    ///< next free stats slot
    };

    /// VK_EXT_debug_utils command-buffer label entry points, resolved by the renderer and
    /// handed to executeRenderGraph so every pass body is bracketed by a named, coloured
    /// marker region in external capture tools (RenderDoc/Nsight). Emitted independent of
    /// any profiler mode — labels carry no in-engine number and are free. Null pointers
    /// (extension absent) make emission a silent no-op.
    struct RgDebugLabels
    {
        PFN_vkCmdBeginDebugUtilsLabelEXT begin = nullptr;  ///< null => labels disabled
        PFN_vkCmdEndDebugUtilsLabelEXT end = nullptr;
    };

    /// A recorded CPU span: a [startNs, endNs) interval on the render thread for one
    /// lifecycle phase or pass, with its nesting (depth + the index of the enclosing span
    /// in the same buffer, or -1). steady_clock ns; the origin is arbitrary but shared
    /// within a frame, so spans are directly comparable. Phase 4 maps these onto the GPU axis.
    struct CpuSpan
    {
        u32 marker = 0;  ///< index into CpuMarkerRegistry::names
        u64 startNs = 0;
        u64 endNs = 0;
        u32 depth = 0;
        i32 parent = -1;  ///< enclosing span index in the same buffer, or -1 at top level
    };

    /// Interns scope names to stable integer ids so a CpuSpan stays string-free. Lifecycle
    /// phase names and pass names recur every frame, so the table grows once and then holds;
    /// lookup is an allocation-free linear scan (the set is a few dozen short names).
    struct CpuMarkerRegistry
    {
        std::vector<std::string> names;  ///< id -> name; the index is the marker id
    };

    /// One frame-in-flight's CPU-span sink plus the open-scope cursor. Recording is on the
    /// single render thread, so the "currently open" parent/depth live here, not in a
    /// thread_local. Plain host memory: readable at end of frame with no GPU sync.
    struct CpuSpanBuffer
    {
        std::vector<CpuSpan> spans;
        i32 openParent = -1;  ///< index of the innermost open span, or -1
        u32 openDepth = 0;    ///< current nesting depth

        void reset()
        {
            spans.clear();
            openParent = -1;
            openDepth = 0;
        }
    };

    /// What a CpuScope writes into: the persistent name registry plus this frame's buffer.
    /// A null buffer means recording is disabled and every scope is a cheap no-op — the CPU
    /// analogue of an unarmed RgTimestamps.
    struct CpuRecorder
    {
        CpuMarkerRegistry* registry = nullptr;
        CpuSpanBuffer* buffer = nullptr;
    };

    /// RAII steady_clock span. Opens on construct, closes on destruct; nesting is derived
    /// from the recorder's buffer cursor. Records nothing (and reads no clock) when the
    /// recorder is inactive, so an `Off` profiler pays only a branch. Non-movable — always a
    /// named local at the seam it measures.
    struct CpuScope
    {
        CpuScope(const CpuRecorder* recorder, std::string_view name);
        ~CpuScope();
        CpuScope(const CpuScope&) = delete;
        auto operator=(const CpuScope&) -> CpuScope& = delete;
        CpuScope(CpuScope&&) = delete;
        auto operator=(CpuScope&&) -> CpuScope& = delete;

        CpuSpanBuffer* buffer = nullptr;  ///< null => inactive
        i32 index = -1;                   ///< this span's slot in buffer->spans
        i32 prevParent = -1;              ///< openParent to restore on close
    };

    /// RAII GPU timestamp scope: writes a begin timestamp on construct and an end timestamp on
    /// destruct into the recorder's pool, pushing a ScopeRecord whose parent/depth come from the
    /// recorder's open-scope cursor. Inactive (a no-op) when the recorder is null, has no pool,
    /// or the pool is full — so overflow past the scope cap truncates gracefully. Non-movable:
    /// a named local bracketing the command range it measures. Pass bodies open sub-scopes on
    /// the same recorder, nesting under their enclosing pass scope.
    struct GpuScope
    {
        GpuScope() = default;
        GpuScope(RgTimestamps* recorder, vk::CommandBuffer cmd, std::string_view name);
        ~GpuScope();
        GpuScope(const GpuScope&) = delete;
        auto operator=(const GpuScope&) -> GpuScope& = delete;
        GpuScope(GpuScope&&) = delete;
        auto operator=(GpuScope&&) -> GpuScope& = delete;

        RgTimestamps* recorder = nullptr;  ///< null => inactive
        vk::CommandBuffer cmd;
        u32 beginSlot = 0;     ///< this scope's begin query slot (end = beginSlot + 1)
        i32 prevParent = -1;   ///< openScope to restore on close
        i32 recordIndex = -1;  ///< this scope's index in records (for the caller's stats query), or -1
    };

    /// Derive and emit each pass's barriers from its declared usage, then record the
    /// pass body inside its rendering scope. Resolves cross-frame layouts on exit. When
    /// `timestamps` is non-null and armed, brackets each pass body with a GPU scope (and the
    /// pass body may open child scopes on the same recorder); `labels` (when resolved) adds a
    /// debug-utils marker region; `cpu` (when active) records a per-pass CPU span.
    void executeRenderGraph(RenderGraph& graph, vk::CommandBuffer cmd, RgTimestamps* timestamps = nullptr,
                            const RgDebugLabels* labels = nullptr, const CpuRecorder* cpu = nullptr);
}

namespace se
{
    struct RgUsageInfo
    {
        vk::PipelineStageFlags2 stage;
        vk::AccessFlags2 access;
        vk::ImageLayout layout;  // eUndefined for buffer usages (no layout)
        bool isWrite;
    };

    auto usageInfo(RgUsage usage) -> RgUsageInfo
    {
        switch (usage)
        {
        case RgUsage::ColorWrite:
            return { vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                     vk::ImageLayout::eColorAttachmentOptimal, true };
        case RgUsage::DepthWrite:
            return { vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
                     vk::AccessFlagBits2::eDepthStencilAttachmentWrite, vk::ImageLayout::eDepthAttachmentOptimal,
                     true };
        case RgUsage::SampledRead:
            return { vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
                     vk::ImageLayout::eShaderReadOnlyOptimal, false };
        case RgUsage::StorageWriteCompute:
            return { vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                     vk::ImageLayout::eUndefined, true };
        case RgUsage::StorageReadCompute:
            return { vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageRead,
                     vk::ImageLayout::eUndefined, false };
        case RgUsage::StorageReadFragment:
            return { vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderStorageRead,
                     vk::ImageLayout::eUndefined, false };
        case RgUsage::StorageImageRWCompute:
            return { vk::PipelineStageFlagBits2::eComputeShader,
                     vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
                     vk::ImageLayout::eGeneral, true };
        case RgUsage::SampledReadCompute:
            return { vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderSampledRead,
                     vk::ImageLayout::eShaderReadOnlyOptimal, false };
        case RgUsage::VertexInputRead:
            return { vk::PipelineStageFlagBits2::eVertexAttributeInput, vk::AccessFlagBits2::eVertexAttributeRead,
                     vk::ImageLayout::eUndefined, false };
        }
        return { vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone, vk::ImageLayout::eUndefined,
                 false };
    }

    // The src scope a freshly-imported image presents, given its entry layout: a
    // ShaderReadOnly image was last sampled by a fragment shader (the WAR source);
    // any other entry layout has no prior in-frame work to wait on.
    void seedImageState(RgResourceState& r)
    {
        if (r.layout == vk::ImageLayout::eShaderReadOnlyOptimal)
        {
            r.lastStage = vk::PipelineStageFlagBits2::eFragmentShader;
            r.lastAccess = vk::AccessFlagBits2::eShaderSampledRead;
        }
        else
        {
            r.lastStage = vk::PipelineStageFlagBits2::eTopOfPipe;
            r.lastAccess = vk::AccessFlagBits2::eNone;
        }
    }

    // Derive a barrier for one (resource, usage), append it, and advance the resource
    // state. Images barrier on a layout change or a hazard; buffers on a hazard only
    // (read-after-write, or a write after any prior use).
    void applyAccess(RgResourceState& r, RgUsageInfo target, std::vector<vk::ImageMemoryBarrier2>& imageBarriers,
                     std::vector<vk::MemoryBarrier2>& memoryBarriers)
    {
        const bool hazard = (target.isWrite && r.touched) || (!target.isWrite && r.lastWasWrite);
        if (r.isImage)
        {
            const bool layoutChange = target.layout != vk::ImageLayout::eUndefined && r.layout != target.layout;
            if (layoutChange || hazard)
            {
                vk::ImageMemoryBarrier2 barrier{};
                barrier.srcStageMask = r.lastStage;
                barrier.srcAccessMask = r.lastAccess;
                barrier.dstStageMask = target.stage;
                barrier.dstAccessMask = target.access;
                barrier.oldLayout = r.layout;
                barrier.newLayout = r.layout;
                if (layoutChange)
                {
                    barrier.newLayout = target.layout;
                }
                barrier.image = r.image;
                barrier.subresourceRange = vk::ImageSubresourceRange{ r.aspect, 0, 1, 0, 1 };
                imageBarriers.push_back(barrier);
            }
            if (layoutChange)
            {
                r.layout = target.layout;
            }
        }
        else if (hazard)
        {
            vk::MemoryBarrier2 barrier{};
            barrier.srcStageMask = r.lastStage;
            barrier.srcAccessMask = r.lastAccess;
            barrier.dstStageMask = target.stage;
            barrier.dstAccessMask = target.access;
            memoryBarriers.push_back(barrier);
        }

        r.lastStage = target.stage;
        r.lastAccess = target.access;
        r.lastWasWrite = target.isWrite;
        r.touched = true;
    }

    auto newRenderGraph() -> RenderGraph
    {
        return RenderGraph{};
    }

    auto importImage(RenderGraph& graph, vk::Image image, vk::ImageView view, vk::ImageAspectFlags aspect,
                     vk::ImageLayout initialLayout, vk::ImageLayout* externalLayout) -> RgResource
    {
        RgResourceState r;
        r.isImage = true;
        r.image = image;
        r.view = view;
        r.aspect = aspect;
        r.layout = initialLayout;
        if (externalLayout != nullptr)
        {
            r.layout = *externalLayout;
        }
        r.externalLayout = externalLayout;
        seedImageState(r);
        graph.resources.push_back(r);
        return RgResource{ static_cast<u32>(graph.resources.size() - 1) };
    }

    auto importImage3D(RenderGraph& graph, vk::Image image, vk::ImageView view, vk::ImageLayout initialLayout,
                       vk::ImageLayout* externalLayout) -> RgResource
    {
        // A 3D image is tracked identically to a 2D one for barrier purposes (the barrier
        // transitions the whole image; dimensionality is irrelevant here).
        return importImage(graph, image, view, vk::ImageAspectFlagBits::eColor, initialLayout, externalLayout);
    }

    auto importBuffer(RenderGraph& graph, vk::Buffer buffer) -> RgResource
    {
        RgResourceState r;
        r.isImage = false;
        r.buffer = buffer;
        graph.resources.push_back(r);
        return RgResource{ static_cast<u32>(graph.resources.size() - 1) };
    }

    void addPass(RenderGraph& graph, RgPass pass)
    {
        graph.passes.push_back(std::move(pass));
    }

    auto cpuScopeNowNs() -> u64
    {
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    // Map a scope name to its stable id, interning it on first sight. Linear scan over the
    // few dozen names; allocation-free once a name is known (only a new name grows the table).
    auto cpuMarkerId(CpuMarkerRegistry& registry, std::string_view name) -> u32
    {
        for (u32 i = 0; i < registry.names.size(); i = i + 1)
        {
            if (std::string_view{ registry.names[i] } == name)
            {
                return i;
            }
        }
        registry.names.emplace_back(name);
        return static_cast<u32>(registry.names.size() - 1);
    }

    CpuScope::CpuScope(const CpuRecorder* recorder, std::string_view name)
    {
        if (recorder == nullptr || recorder->buffer == nullptr || recorder->registry == nullptr)
        {
            return;
        }
        buffer = recorder->buffer;
        const u32 marker = cpuMarkerId(*recorder->registry, name);
        index = static_cast<i32>(buffer->spans.size());
        prevParent = buffer->openParent;
        CpuSpan span;
        span.marker = marker;
        span.depth = buffer->openDepth;
        span.parent = buffer->openParent;
        span.startNs = cpuScopeNowNs();
        buffer->spans.push_back(span);
        buffer->openParent = index;
        buffer->openDepth = buffer->openDepth + 1;
    }

    CpuScope::~CpuScope()
    {
        if (buffer == nullptr)
        {
            return;
        }
        buffer->spans[static_cast<u32>(index)].endNs = cpuScopeNowNs();
        buffer->openParent = prevParent;
        buffer->openDepth = buffer->openDepth - 1;
    }

    GpuScope::GpuScope(RgTimestamps* rec, vk::CommandBuffer commandBuffer, std::string_view name)
    {
        // Inactive unless the recorder is armed and a begin/end slot pair is still free. Once
        // full, every later scope is inactive too (nextSlot only grows), so the recorded set is
        // a begin-order prefix of the full tree — parents always precede their children.
        if (rec == nullptr || !rec->pool || rec->nextSlot + 1 >= rec->capacity)
        {
            return;
        }
        recorder = rec;
        cmd = commandBuffer;
        beginSlot = rec->nextSlot;
        rec->nextSlot = rec->nextSlot + 2;
        prevParent = rec->openScope;
        recordIndex = static_cast<i32>(rec->records->size());
        ScopeRecord record;
        record.name = name;
        record.parentIndex = rec->openScope;
        record.depth = rec->depth;
        rec->records->push_back(std::move(record));
        rec->openScope = recordIndex;
        rec->depth = rec->depth + 1;
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe, rec->pool, beginSlot);
    }

    GpuScope::~GpuScope()
    {
        if (recorder == nullptr)
        {
            return;
        }
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, recorder->pool, beginSlot + 1);
        recorder->openScope = prevParent;
        recorder->depth = recorder->depth - 1;
    }

    // Bright-ish, stable RGBA from a pass name (FNV-1a) so a pass group reads as one colour
    // across captures. Cosmetic — RenderDoc/Nsight honour VkDebugUtilsLabelEXT::color.
    void beginPassLabel(const RgDebugLabels& labels, vk::CommandBuffer cmd, const std::string& name)
    {
        VkDebugUtilsLabelEXT label{};
        label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        label.pLabelName = name.c_str();
        u32 h = 2166136261u;
        for (char c : name)
        {
            h = (h ^ static_cast<u8>(c)) * 16777619u;
        }
        label.color[0] = 0.45f + (0.55f * static_cast<f32>(h & 0xFFu) / 255.0f);
        label.color[1] = 0.45f + (0.55f * static_cast<f32>((h >> 8) & 0xFFu) / 255.0f);
        label.color[2] = 0.45f + (0.55f * static_cast<f32>((h >> 16) & 0xFFu) / 255.0f);
        label.color[3] = 1.0f;
        labels.begin(static_cast<VkCommandBuffer>(cmd), &label);
    }

    void executeRenderGraph(RenderGraph& graph, vk::CommandBuffer cmd, RgTimestamps* timestamps,
                            const RgDebugLabels* labels, const CpuRecorder* cpu)
    {
        const bool labelling = labels != nullptr && labels->begin != nullptr;

        for (RgPass& pass : graph.passes)
        {
            // Name the whole pass region (barriers + body + its timestamps) for capture
            // tools, regardless of whether timing is on.
            if (labelling)
            {
                beginPassLabel(*labels, cmd, pass.name);
            }

            // CPU span for the cost of recording this pass on the render thread — nested
            // under the caller's open scope, paired with the pass's GPU span by name.
            const CpuScope passScope(cpu, pass.name);

            // Top-level GPU scope for the pass (its barriers + body). The body may open child
            // scopes on the same recorder, which nest under this one. No-op when timing is off
            // or the pool is full.
            const GpuScope gpuScope(timestamps, cmd, pass.name);

            // One pipeline-statistics query per top-level pass (PipelineStats mode), recorded
            // against the pass's scope. Issued inside the rendering scope below (graphics) or
            // around the body (compute) — it must not straddle beginRendering/endRendering.
            const bool statsThisPass = timestamps != nullptr && static_cast<bool>(timestamps->statsPool) &&
                                       gpuScope.recordIndex >= 0 &&
                                       timestamps->nextStatsSlot < timestamps->statsCapacity;
            u32 statsSlot = 0;
            if (statsThisPass)
            {
                statsSlot = timestamps->nextStatsSlot;
                timestamps->nextStatsSlot = timestamps->nextStatsSlot + 1;
                ScopeRecord& rec = (*timestamps->records)[static_cast<u32>(gpuScope.recordIndex)];
                rec.statsSlot = static_cast<i32>(statsSlot);
                rec.pixels = static_cast<u64>(pass.renderArea.width) * pass.renderArea.height;
            }

            std::vector<vk::ImageMemoryBarrier2> imageBarriers;
            std::vector<vk::MemoryBarrier2> memoryBarriers;

            for (RgAccess& access : pass.accesses)
            {
                applyAccess(graph.resources[access.resource.index], usageInfo(access.usage), imageBarriers,
                            memoryBarriers);
            }
            for (const RgAttachment& att : pass.colors)
            {
                applyAccess(graph.resources[att.resource.index], usageInfo(RgUsage::ColorWrite), imageBarriers,
                            memoryBarriers);
                if (att.resolve)
                {
                    // The resolve target is written at end-of-pass — a second color write.
                    applyAccess(graph.resources[att.resolve->index], usageInfo(RgUsage::ColorWrite), imageBarriers,
                                memoryBarriers);
                }
            }
            if (pass.depth)
            {
                applyAccess(graph.resources[pass.depth->resource.index], usageInfo(RgUsage::DepthWrite), imageBarriers,
                            memoryBarriers);
                if (pass.depth->resolve)
                {
                    // The depth resolve target is written at end-of-pass — a second depth write.
                    applyAccess(graph.resources[pass.depth->resolve->index], usageInfo(RgUsage::DepthWrite),
                                imageBarriers, memoryBarriers);
                }
            }

            if (!imageBarriers.empty() || !memoryBarriers.empty())
            {
                vk::DependencyInfo dependency{};
                dependency.setImageMemoryBarriers(imageBarriers);
                dependency.setMemoryBarriers(memoryBarriers);
                cmd.pipelineBarrier2(dependency);
            }

            if (pass.kind == RgPassKind::Graphics)
            {
                std::vector<vk::RenderingAttachmentInfo> colorInfos;
                vk::RenderingAttachmentInfo depthInfo{};
                vk::RenderingInfo rendering{};
                rendering.renderArea = vk::Rect2D{ vk::Offset2D{ 0, 0 }, pass.renderArea };
                rendering.layerCount = 1;
                for (const RgAttachment& att : pass.colors)
                {
                    const RgResourceState& r = graph.resources[att.resource.index];
                    vk::RenderingAttachmentInfo colorInfo{};
                    colorInfo.imageView = r.view;
                    colorInfo.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
                    colorInfo.loadOp = att.loadOp;
                    colorInfo.storeOp = att.storeOp;
                    colorInfo.clearValue = att.clearValue;
                    if (att.resolve)
                    {
                        colorInfo.resolveMode = vk::ResolveModeFlagBits::eAverage;
                        colorInfo.resolveImageView = graph.resources[att.resolve->index].view;
                        colorInfo.resolveImageLayout = vk::ImageLayout::eColorAttachmentOptimal;
                    }
                    colorInfos.push_back(colorInfo);
                }
                if (!colorInfos.empty())
                {
                    rendering.setColorAttachments(colorInfos);
                }
                if (pass.depth)
                {
                    const RgResourceState& r = graph.resources[pass.depth->resource.index];
                    depthInfo.imageView = r.view;
                    depthInfo.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
                    depthInfo.loadOp = pass.depth->loadOp;
                    depthInfo.storeOp = pass.depth->storeOp;
                    depthInfo.clearValue = pass.depth->clearValue;
                    if (pass.depth->resolve)
                    {
                        // Resolve the multisampled depth into a 1x target (sample 0 is enough
                        // for editor occlusion and is always a supported resolve mode).
                        depthInfo.resolveMode = vk::ResolveModeFlagBits::eSampleZero;
                        depthInfo.resolveImageView = graph.resources[pass.depth->resolve->index].view;
                        depthInfo.resolveImageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
                    }
                    rendering.setPDepthAttachment(&depthInfo);
                }
                cmd.beginRendering(rendering);

                vk::Viewport viewport{
                    0.0f, 0.0f, static_cast<f32>(pass.renderArea.width), static_cast<f32>(pass.renderArea.height),
                    0.0f, 1.0f
                };
                vk::Rect2D scissor{ vk::Offset2D{ 0, 0 }, pass.renderArea };
                cmd.setViewport(0, viewport);
                cmd.setScissor(0, scissor);

                if (statsThisPass)
                {
                    cmd.beginQuery(timestamps->statsPool, statsSlot, vk::QueryControlFlags{});
                }
                if (pass.execute)
                {
                    pass.execute(cmd);
                }
                if (statsThisPass)
                {
                    cmd.endQuery(timestamps->statsPool, statsSlot);
                }
                cmd.endRendering();
            }
            else
            {
                if (statsThisPass)
                {
                    cmd.beginQuery(timestamps->statsPool, statsSlot, vk::QueryControlFlags{});
                }
                if (pass.execute)
                {
                    pass.execute(cmd);
                }
                if (statsThisPass)
                {
                    cmd.endQuery(timestamps->statsPool, statsSlot);
                }
            }

            if (labelling)
            {
                labels->end(static_cast<VkCommandBuffer>(cmd));
            }
            // gpuScope + passScope close here, writing the pass's end timestamp / CPU span end.
        }

        for (RgResourceState& r : graph.resources)
        {
            if (r.externalLayout != nullptr)
            {
                *r.externalLayout = r.layout;
            }
        }
    }
}
