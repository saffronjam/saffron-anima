module;

// This module wraps the heavy C/C++ graphics headers, so it uses classic
// includes (no `import std`) to stay clear of the import-std/third-party-header
// friction. The rest of the engine imports it normally.
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <string>
#include <vector>

export module Saffron.Rendering;

import Saffron.Core;
import Saffron.Window;

export namespace se
{
    // A unit of GPU work recorded into the active command buffer. This is the
    // deferred-submission seam carried over from the old engine: the public API
    // records intent as a closure; the backend supplies the command buffer.
    using RenderFn = std::function<void(VkCommandBuffer)>;

    inline constexpr u32 MaxFramesInFlight = 2;

    struct FrameData
    {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
    };

    struct Renderer
    {
        // vk-bootstrap keeps the bits we need for clean teardown.
        vkb::Instance vkbInstance;
        vkb::Device vkbDevice;

        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        VkQueue graphicsQueue = VK_NULL_HANDLE;
        u32 graphicsQueueFamily = 0;
        VmaAllocator allocator = nullptr;

        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D swapchainExtent{};
        std::vector<VkImage> swapchainImages;
        std::vector<VkImageView> swapchainImageViews;
        std::vector<VkSemaphore> renderFinished;  // one per swapchain image
        std::vector<VkFence> imagesInFlight;       // borrowed per-frame fence per image (no ownership)

        std::array<FrameData, MaxFramesInFlight> frames{};
        u32 frameIndex = 0;
        u32 imageIndex = 0;

        std::array<f32, 4> clearColor{ 0.05f, 0.06f, 0.08f, 1.0f };
        std::vector<RenderFn> submissions;  // recorded this frame

        Window* window = nullptr;  // borrowed, not owned
    };

    std::expected<Renderer, std::string> newRenderer(Window& window);
    void destroyRenderer(Renderer& renderer);

    // Frame lifecycle. beginFrame returns false when the frame was skipped
    // (e.g. the swapchain was just recreated); callers should not submit then.
    bool beginFrame(Renderer& renderer);
    void submit(Renderer& renderer, RenderFn fn);
    void endFrame(Renderer& renderer);
}

// ---------------------------------------------------------------------------
// Implementation (kept in this same unit to avoid std-module mixing across
// translation units of the module).
// ---------------------------------------------------------------------------
namespace se
{
    namespace
    {
        // Records a synchronization2 image layout transition.
        void transitionImage(
            VkCommandBuffer cmd,
            VkImage image,
            VkImageLayout oldLayout,
            VkImageLayout newLayout,
            VkPipelineStageFlags2 srcStage,
            VkAccessFlags2 srcAccess,
            VkPipelineStageFlags2 dstStage,
            VkAccessFlags2 dstAccess)
        {
            VkImageMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
            barrier.srcStageMask = srcStage;
            barrier.srcAccessMask = srcAccess;
            barrier.dstStageMask = dstStage;
            barrier.dstAccessMask = dstAccess;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;

            VkDependencyInfo dependency{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &barrier;

            vkCmdPipelineBarrier2(cmd, &dependency);
        }

        void destroySwapchainResources(Renderer& renderer)
        {
            for (VkImageView view : renderer.swapchainImageViews)
            {
                vkDestroyImageView(renderer.device, view, nullptr);
            }
            renderer.swapchainImageViews.clear();

            for (VkSemaphore semaphore : renderer.renderFinished)
            {
                vkDestroySemaphore(renderer.device, semaphore, nullptr);
            }
            renderer.renderFinished.clear();

            if (renderer.swapchain != VK_NULL_HANDLE)
            {
                vkDestroySwapchainKHR(renderer.device, renderer.swapchain, nullptr);
                renderer.swapchain = VK_NULL_HANDLE;
            }
        }

        std::expected<void, std::string> buildSwapchain(Renderer& renderer, u32 width, u32 height)
        {
            vkb::SwapchainBuilder builder{ renderer.vkbDevice };
            builder.set_desired_format(VkSurfaceFormatKHR{
                       .format = VK_FORMAT_B8G8R8A8_UNORM,
                       .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
                .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                .set_desired_extent(width, height)
                .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT);

            if (renderer.swapchain != VK_NULL_HANDLE)
            {
                builder.set_old_swapchain(renderer.swapchain);
            }

            auto result = builder.build();
            if (!result)
            {
                return std::unexpected(std::format("swapchain build failed: {}", result.error().message()));
            }

            // Old swapchain handle is consumed by the builder; drop our resources.
            destroySwapchainResources(renderer);

            vkb::Swapchain swapchain = result.value();
            renderer.swapchain = swapchain.swapchain;
            renderer.swapchainFormat = swapchain.image_format;
            renderer.swapchainExtent = swapchain.extent;
            renderer.swapchainImages = swapchain.get_images().value();
            renderer.swapchainImageViews = swapchain.get_image_views().value();

            renderer.renderFinished.resize(renderer.swapchainImages.size());
            VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            for (VkSemaphore& semaphore : renderer.renderFinished)
            {
                vkCreateSemaphore(renderer.device, &semaphoreInfo, nullptr, &semaphore);
            }
            renderer.imagesInFlight.assign(renderer.swapchainImages.size(), VK_NULL_HANDLE);

            return {};
        }

        void recreateSwapchain(Renderer& renderer)
        {
            u32 width = renderer.window->width;
            u32 height = renderer.window->height;
            if (width == 0 || height == 0)
            {
                return;  // minimized — keep the old swapchain, retry once restored
            }
            vkDeviceWaitIdle(renderer.device);
            auto built = buildSwapchain(renderer, width, height);
            if (!built)
            {
                logError(built.error());
            }
        }
    }

    std::expected<Renderer, std::string> newRenderer(Window& window)
    {
        Renderer renderer;
        renderer.window = &window;

        // Instance extensions SDL needs to create a Vulkan surface for this platform.
        u32 sdlExtensionCount = 0;
        const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);

        vkb::InstanceBuilder instanceBuilder;
        instanceBuilder
            .set_app_name("Saffron Editor")
            .set_engine_name("Saffron Engine")
            .require_api_version(1, 3, 0)
            .request_validation_layers(true)
            .use_default_debug_messenger();
        for (u32 i = 0; i < sdlExtensionCount; i = i + 1)
        {
            instanceBuilder.enable_extension(sdlExtensions[i]);
        }
        auto instanceResult = instanceBuilder.build();
        if (!instanceResult)
        {
            return std::unexpected(std::format("instance creation failed: {}", instanceResult.error().message()));
        }
        renderer.vkbInstance = instanceResult.value();

        if (!SDL_Vulkan_CreateSurface(window.handle, renderer.vkbInstance.instance, nullptr, &renderer.surface))
        {
            return std::unexpected(std::format("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError()));
        }

        VkPhysicalDeviceVulkan13Features features13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;

        vkb::PhysicalDeviceSelector selector{ renderer.vkbInstance };
        auto physicalResult = selector
                                  .set_minimum_version(1, 3)
                                  .set_required_features_13(features13)
                                  .set_surface(renderer.surface)
                                  .select();
        if (!physicalResult)
        {
            return std::unexpected(std::format("no suitable GPU: {}", physicalResult.error().message()));
        }

        vkb::DeviceBuilder deviceBuilder{ physicalResult.value() };
        auto deviceResult = deviceBuilder.build();
        if (!deviceResult)
        {
            return std::unexpected(std::format("device creation failed: {}", deviceResult.error().message()));
        }
        renderer.vkbDevice = deviceResult.value();
        renderer.physicalDevice = physicalResult.value().physical_device;
        renderer.device = renderer.vkbDevice.device;

        auto queueResult = renderer.vkbDevice.get_queue(vkb::QueueType::graphics);
        if (!queueResult)
        {
            return std::unexpected(std::format("no graphics queue: {}", queueResult.error().message()));
        }
        renderer.graphicsQueue = queueResult.value();
        renderer.graphicsQueueFamily = renderer.vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.instance = renderer.vkbInstance.instance;
        allocatorInfo.physicalDevice = renderer.physicalDevice;
        allocatorInfo.device = renderer.device;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        if (vmaCreateAllocator(&allocatorInfo, &renderer.allocator) != VK_SUCCESS)
        {
            return std::unexpected(std::string{ "vmaCreateAllocator failed" });
        }

        auto swapchainBuilt = buildSwapchain(renderer, window.width, window.height);
        if (!swapchainBuilt)
        {
            return std::unexpected(swapchainBuilt.error());
        }

        // Per-frame command buffers + sync.
        for (FrameData& frame : renderer.frames)
        {
            VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = renderer.graphicsQueueFamily;
            vkCreateCommandPool(renderer.device, &poolInfo, nullptr, &frame.commandPool);

            VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            allocInfo.commandPool = frame.commandPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            vkAllocateCommandBuffers(renderer.device, &allocInfo, &frame.commandBuffer);

            VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            vkCreateSemaphore(renderer.device, &semaphoreInfo, nullptr, &frame.imageAvailable);

            VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(renderer.device, &fenceInfo, nullptr, &frame.inFlight);
        }

        logInfo(std::format("vulkan ready — gpu '{}', {} swapchain images",
                            renderer.vkbDevice.physical_device.name,
                            renderer.swapchainImages.size()));
        return renderer;
    }

    void destroyRenderer(Renderer& renderer)
    {
        if (renderer.device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(renderer.device);
        }

        for (FrameData& frame : renderer.frames)
        {
            vkDestroyFence(renderer.device, frame.inFlight, nullptr);
            vkDestroySemaphore(renderer.device, frame.imageAvailable, nullptr);
            vkDestroyCommandPool(renderer.device, frame.commandPool, nullptr);
        }

        destroySwapchainResources(renderer);

        if (renderer.allocator != nullptr)
        {
            vmaDestroyAllocator(renderer.allocator);
            renderer.allocator = nullptr;
        }
        if (renderer.surface != VK_NULL_HANDLE)
        {
            vkb::destroy_surface(renderer.vkbInstance, renderer.surface);
        }
        vkb::destroy_device(renderer.vkbDevice);
        vkb::destroy_instance(renderer.vkbInstance);
    }

    bool beginFrame(Renderer& renderer)
    {
        FrameData& frame = renderer.frames[renderer.frameIndex];

        vkWaitForFences(renderer.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX);

        VkResult acquire = vkAcquireNextImageKHR(
            renderer.device, renderer.swapchain, UINT64_MAX,
            frame.imageAvailable, VK_NULL_HANDLE, &renderer.imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
        {
            recreateSwapchain(renderer);
            return false;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
        {
            logError(std::format("vkAcquireNextImageKHR failed ({})", static_cast<i32>(acquire)));
            return false;
        }
        // VK_SUBOPTIMAL_KHR: render this frame anyway; present will trigger the recreate.

        // Ensure the previous frame that used THIS image has finished before we
        // reuse the image's renderFinished semaphore (per-image present semaphore
        // throttled by a per-image fence, not just the per-frame fence).
        if (renderer.imagesInFlight[renderer.imageIndex] != VK_NULL_HANDLE)
        {
            vkWaitForFences(renderer.device, 1, &renderer.imagesInFlight[renderer.imageIndex], VK_TRUE, UINT64_MAX);
        }
        renderer.imagesInFlight[renderer.imageIndex] = frame.inFlight;

        vkResetFences(renderer.device, 1, &frame.inFlight);
        vkResetCommandBuffer(frame.commandBuffer, 0);
        renderer.submissions.clear();

        VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);

        transitionImage(
            frame.commandBuffer, renderer.swapchainImages[renderer.imageIndex],
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        VkRenderingAttachmentInfo colorAttachment{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        colorAttachment.imageView = renderer.swapchainImageViews[renderer.imageIndex];
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = VkClearColorValue{
            { renderer.clearColor[0], renderer.clearColor[1], renderer.clearColor[2], renderer.clearColor[3] }
        };

        VkRenderingInfo renderingInfo{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        renderingInfo.renderArea.extent = renderer.swapchainExtent;
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;
        vkCmdBeginRendering(frame.commandBuffer, &renderingInfo);

        VkViewport viewport{ 0.0f, 0.0f,
                             static_cast<f32>(renderer.swapchainExtent.width),
                             static_cast<f32>(renderer.swapchainExtent.height),
                             0.0f, 1.0f };
        VkRect2D scissor{ { 0, 0 }, renderer.swapchainExtent };
        vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);

        return true;
    }

    void submit(Renderer& renderer, RenderFn fn)
    {
        renderer.submissions.push_back(std::move(fn));
    }

    void endFrame(Renderer& renderer)
    {
        FrameData& frame = renderer.frames[renderer.frameIndex];

        for (RenderFn& fn : renderer.submissions)
        {
            fn(frame.commandBuffer);
        }

        vkCmdEndRendering(frame.commandBuffer);

        transitionImage(
            frame.commandBuffer, renderer.swapchainImages[renderer.imageIndex],
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);

        vkEndCommandBuffer(frame.commandBuffer);

        VkSemaphore signalSemaphore = renderer.renderFinished[renderer.imageIndex];

        VkSemaphoreSubmitInfo waitInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        waitInfo.semaphore = frame.imageAvailable;
        waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSemaphoreSubmitInfo signalInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
        signalInfo.semaphore = signalSemaphore;
        signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkCommandBufferSubmitInfo cmdInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
        cmdInfo.commandBuffer = frame.commandBuffer;

        VkSubmitInfo2 submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
        submitInfo.waitSemaphoreInfoCount = 1;
        submitInfo.pWaitSemaphoreInfos = &waitInfo;
        submitInfo.commandBufferInfoCount = 1;
        submitInfo.pCommandBufferInfos = &cmdInfo;
        submitInfo.signalSemaphoreInfoCount = 1;
        submitInfo.pSignalSemaphoreInfos = &signalInfo;
        vkQueueSubmit2(renderer.graphicsQueue, 1, &submitInfo, frame.inFlight);

        VkPresentInfoKHR presentInfo{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &signalSemaphore;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &renderer.swapchain;
        presentInfo.pImageIndices = &renderer.imageIndex;
        VkResult present = vkQueuePresentKHR(renderer.graphicsQueue, &presentInfo);
        if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR)
        {
            recreateSwapchain(renderer);
        }

        renderer.frameIndex = (renderer.frameIndex + 1) % MaxFramesInFlight;
    }
}
