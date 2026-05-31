module;

// Heavy C++ header in the global module fragment (no import std), same Vulkan-Hpp
// configuration as the primary interface unit (renderer.cppm).
#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_NO_SMART_HANDLE
#include <vulkan/vulkan.hpp>

#include <functional>
#include <optional>
#include <string>
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
        ColorWrite,           ///< color attachment write
        DepthWrite,           ///< depth attachment write
        SampledRead,          ///< sampled in a fragment shader
        StorageWriteCompute,  ///< storage buffer written by a compute shader
        StorageReadCompute,   ///< storage buffer read by a compute shader
        StorageReadFragment,  ///< storage buffer read by a fragment shader
        StorageImageRWCompute,  ///< image read+written in place by a compute shader (GENERAL)
        SampledReadCompute,     ///< image sampled in a compute shader (SHADER_READ_ONLY)
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
    /// `resolve` (color only) is an MSAA resolve target: the multisampled attachment is
    /// resolved into it at end-of-pass; the graph treats it as a second color write.
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
        std::optional<RgAttachment> color;
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

    RenderGraph newRenderGraph();

    /// Import an external image (offscreen/swapchain target). When externalLayout is
    /// set it seeds the entry layout and receives the resolved layout after execute,
    /// so an image's layout carries across frames.
    RgResource importImage(RenderGraph& graph, vk::Image image, vk::ImageView view,
                           vk::ImageAspectFlags aspect, vk::ImageLayout initialLayout,
                           vk::ImageLayout* externalLayout);

    /// Import an external buffer produced and/or consumed within the frame.
    RgResource importBuffer(RenderGraph& graph, vk::Buffer buffer);

    void addPass(RenderGraph& graph, RgPass pass);

    /// Derive and emit each pass's barriers from its declared usage, then record the
    /// pass body inside its rendering scope. Resolves cross-frame layouts on exit.
    void executeRenderGraph(RenderGraph& graph, vk::CommandBuffer cmd);
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

    RgUsageInfo usageInfo(RgUsage usage)
    {
        switch (usage)
        {
        case RgUsage::ColorWrite:
            return { vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                     vk::AccessFlagBits2::eColorAttachmentWrite,
                     vk::ImageLayout::eColorAttachmentOptimal, true };
        case RgUsage::DepthWrite:
            return { vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
                     vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
                     vk::ImageLayout::eDepthAttachmentOptimal, true };
        case RgUsage::SampledRead:
            return { vk::PipelineStageFlagBits2::eFragmentShader,
                     vk::AccessFlagBits2::eShaderSampledRead,
                     vk::ImageLayout::eShaderReadOnlyOptimal, false };
        case RgUsage::StorageWriteCompute:
            return { vk::PipelineStageFlagBits2::eComputeShader,
                     vk::AccessFlagBits2::eShaderStorageWrite,
                     vk::ImageLayout::eUndefined, true };
        case RgUsage::StorageReadCompute:
            return { vk::PipelineStageFlagBits2::eComputeShader,
                     vk::AccessFlagBits2::eShaderStorageRead,
                     vk::ImageLayout::eUndefined, false };
        case RgUsage::StorageReadFragment:
            return { vk::PipelineStageFlagBits2::eFragmentShader,
                     vk::AccessFlagBits2::eShaderStorageRead,
                     vk::ImageLayout::eUndefined, false };
        case RgUsage::StorageImageRWCompute:
            return { vk::PipelineStageFlagBits2::eComputeShader,
                     vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
                     vk::ImageLayout::eGeneral, true };
        case RgUsage::SampledReadCompute:
            return { vk::PipelineStageFlagBits2::eComputeShader,
                     vk::AccessFlagBits2::eShaderSampledRead,
                     vk::ImageLayout::eShaderReadOnlyOptimal, false };
        }
        return { vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                 vk::ImageLayout::eUndefined, false };
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
    void applyAccess(RgResourceState& r, RgUsageInfo target,
                     std::vector<vk::ImageMemoryBarrier2>& imageBarriers,
                     std::vector<vk::MemoryBarrier2>& memoryBarriers)
    {
        const bool hazard = (target.isWrite && r.touched) || (!target.isWrite && r.lastWasWrite);
        if (r.isImage)
        {
            const bool layoutChange =
                target.layout != vk::ImageLayout::eUndefined && r.layout != target.layout;
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

    RenderGraph newRenderGraph()
    {
        return RenderGraph{};
    }

    RgResource importImage(RenderGraph& graph, vk::Image image, vk::ImageView view,
                           vk::ImageAspectFlags aspect, vk::ImageLayout initialLayout,
                           vk::ImageLayout* externalLayout)
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

    RgResource importBuffer(RenderGraph& graph, vk::Buffer buffer)
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

    void executeRenderGraph(RenderGraph& graph, vk::CommandBuffer cmd)
    {
        for (RgPass& pass : graph.passes)
        {
            std::vector<vk::ImageMemoryBarrier2> imageBarriers;
            std::vector<vk::MemoryBarrier2> memoryBarriers;

            for (RgAccess& access : pass.accesses)
            {
                applyAccess(graph.resources[access.resource.index], usageInfo(access.usage),
                            imageBarriers, memoryBarriers);
            }
            if (pass.color)
            {
                applyAccess(graph.resources[pass.color->resource.index], usageInfo(RgUsage::ColorWrite),
                            imageBarriers, memoryBarriers);
                if (pass.color->resolve)
                {
                    // The resolve target is written at end-of-pass — a second color write.
                    applyAccess(graph.resources[pass.color->resolve->index], usageInfo(RgUsage::ColorWrite),
                                imageBarriers, memoryBarriers);
                }
            }
            if (pass.depth)
            {
                applyAccess(graph.resources[pass.depth->resource.index], usageInfo(RgUsage::DepthWrite),
                            imageBarriers, memoryBarriers);
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
                vk::RenderingAttachmentInfo colorInfo{};
                vk::RenderingAttachmentInfo depthInfo{};
                vk::RenderingInfo rendering{};
                rendering.renderArea = vk::Rect2D{ vk::Offset2D{ 0, 0 }, pass.renderArea };
                rendering.layerCount = 1;
                if (pass.color)
                {
                    const RgResourceState& r = graph.resources[pass.color->resource.index];
                    colorInfo.imageView = r.view;
                    colorInfo.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
                    colorInfo.loadOp = pass.color->loadOp;
                    colorInfo.storeOp = pass.color->storeOp;
                    colorInfo.clearValue = pass.color->clearValue;
                    if (pass.color->resolve)
                    {
                        colorInfo.resolveMode = vk::ResolveModeFlagBits::eAverage;
                        colorInfo.resolveImageView = graph.resources[pass.color->resolve->index].view;
                        colorInfo.resolveImageLayout = vk::ImageLayout::eColorAttachmentOptimal;
                    }
                    rendering.setColorAttachments(colorInfo);
                }
                if (pass.depth)
                {
                    const RgResourceState& r = graph.resources[pass.depth->resource.index];
                    depthInfo.imageView = r.view;
                    depthInfo.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
                    depthInfo.loadOp = pass.depth->loadOp;
                    depthInfo.storeOp = pass.depth->storeOp;
                    depthInfo.clearValue = pass.depth->clearValue;
                    rendering.setPDepthAttachment(&depthInfo);
                }
                cmd.beginRendering(rendering);

                vk::Viewport viewport{ 0.0f, 0.0f,
                                       static_cast<f32>(pass.renderArea.width),
                                       static_cast<f32>(pass.renderArea.height), 0.0f, 1.0f };
                vk::Rect2D scissor{ vk::Offset2D{ 0, 0 }, pass.renderArea };
                cmd.setViewport(0, viewport);
                cmd.setScissor(0, scissor);

                if (pass.execute)
                {
                    pass.execute(cmd);
                }
                cmd.endRendering();
            }
            else if (pass.execute)
            {
                pass.execute(cmd);
            }
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
