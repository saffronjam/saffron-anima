module;

#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_NO_SMART_HANDLE
#include <vulkan/vulkan.hpp>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include <stb_image_write.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module Saffron.Rendering:Detail;

import Saffron.Core;
import Saffron.Window;
import Saffron.Geometry;
import :Types;

export namespace se
{
    // Converts a Vulkan-Hpp ResultValue to Result, checked at the call site.
    template <typename T>
    auto checked(vk::ResultValue<T> rv, std::string_view what) -> Result<T>
    {
        if (rv.result != vk::Result::eSuccess)
        {
            return Err(std::format("{}: {}", what, vk::to_string(rv.result)));
        }
        return std::move(rv.value);
    }

    auto checked(vk::Result result, std::string_view what) -> Result<void>
    {
        if (result != vk::Result::eSuccess)
        {
            return Err(std::format("{}: {}", what, vk::to_string(result)));
        }
        return {};
    }

    void transitionImage(
        vk::CommandBuffer cmd,
        vk::Image image,
        vk::ImageLayout oldLayout,
        vk::ImageLayout newLayout,
        vk::PipelineStageFlags2 srcStage,
        vk::AccessFlags2 srcAccess,
        vk::PipelineStageFlags2 dstStage,
        vk::AccessFlags2 dstAccess,
        vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor)
    {
        vk::ImageMemoryBarrier2 barrier{};
        barrier.srcStageMask = srcStage;
        barrier.srcAccessMask = srcAccess;
        barrier.dstStageMask = dstStage;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.image = image;
        barrier.subresourceRange = vk::ImageSubresourceRange{ aspect, 0, 1, 0, 1 };

        vk::DependencyInfo dependency{};
        dependency.setImageMemoryBarriers(barrier);
        cmd.pipelineBarrier2(dependency);
    }

    void destroySwapchainResources(Renderer& renderer)
    {
        for (vk::ImageView view : renderer.swapchain.imageViews)
        {
            renderer.context.device.destroyImageView(view);
        }
        renderer.swapchain.imageViews.clear();

        for (vk::Semaphore semaphore : renderer.swapchain.renderFinished)
        {
            renderer.context.device.destroySemaphore(semaphore);
        }
        renderer.swapchain.renderFinished.clear();

        if (renderer.swapchain.handle)
        {
            renderer.context.device.destroySwapchainKHR(renderer.swapchain.handle);
            renderer.swapchain.handle = nullptr;
        }
    }

    auto buildSwapchain(Renderer& renderer, u32 width, u32 height) -> Result<void>
    {
        // TRANSFER_SRC on swapchain images is not spec-guaranteed (only
        // COLOR_ATTACHMENT is). Query support and only request it when present
        // so an exotic surface disables window screenshots rather than failing
        // the whole swapchain build.
        renderer.swapchain.captureSupported = false;
        vk::ResultValue<vk::SurfaceCapabilitiesKHR> caps =
            renderer.context.physicalDevice.getSurfaceCapabilitiesKHR(renderer.context.surface);
        if (caps.result == vk::Result::eSuccess &&
            (caps.value.supportedUsageFlags & vk::ImageUsageFlagBits::eTransferSrc))
        {
            renderer.swapchain.captureSupported = true;
        }

        VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (renderer.swapchain.captureSupported)
        {
            usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }

        vkb::SwapchainBuilder builder{ renderer.context.vkbDevice };
        builder.set_desired_format(VkSurfaceFormatKHR{
                   .format = VK_FORMAT_B8G8R8A8_UNORM,
                   .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
            .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
            .set_desired_extent(width, height)
            .add_image_usage_flags(usage);

        if (renderer.swapchain.handle)
        {
            builder.set_old_swapchain(static_cast<VkSwapchainKHR>(renderer.swapchain.handle));
        }

        auto result = builder.build();
        if (!result)
        {
            return Err(std::format("swapchain build failed: {}", result.error().message()));
        }

        destroySwapchainResources(renderer);

        vkb::Swapchain swapchain = result.value();
        renderer.swapchain.handle = vk::SwapchainKHR{ swapchain.swapchain };
        renderer.swapchain.format = vk::Format{ static_cast<vk::Format>(swapchain.image_format) };
        renderer.swapchain.extent = vk::Extent2D{ swapchain.extent.width, swapchain.extent.height };

        renderer.swapchain.images.clear();
        for (VkImage image : swapchain.get_images().value())
        {
            renderer.swapchain.images.push_back(vk::Image{ image });
        }

        renderer.swapchain.imageViews.clear();
        for (vk::Image image : renderer.swapchain.images)
        {
            vk::ImageViewCreateInfo viewInfo{};
            viewInfo.image = image;
            viewInfo.viewType = vk::ImageViewType::e2D;
            viewInfo.format = renderer.swapchain.format;
            viewInfo.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
            auto view = checked(renderer.context.device.createImageView(viewInfo), "createImageView");
            if (!view)
            {
                return Err(view.error());
            }
            renderer.swapchain.imageViews.push_back(*view);
        }

        renderer.swapchain.renderFinished.clear();
        for (std::size_t i = 0; i < renderer.swapchain.images.size(); i = i + 1)
        {
            auto semaphore = checked(renderer.context.device.createSemaphore(vk::SemaphoreCreateInfo{}), "createSemaphore");
            if (!semaphore)
            {
                return Err(semaphore.error());
            }
            renderer.swapchain.renderFinished.push_back(*semaphore);
        }

        renderer.swapchain.imagesInFlight.assign(renderer.swapchain.images.size(), vk::Fence{});
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
        static_cast<void>(renderer.context.device.waitIdle());
        auto built = buildSwapchain(renderer, width, height);
        if (!built)
        {
            logError(built.error());
        }
    }

    auto loadShaderModule(vk::Device device, const std::string& path) -> Result<vk::ShaderModule>
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
        {
            return Err(std::format("cannot open shader '{}'", path));
        }
        std::streamsize size = file.tellg();
        if (size <= 0 || (size % 4) != 0)
        {
            return Err(std::format("invalid spir-v size for '{}'", path));
        }
        std::vector<u32> code(static_cast<std::size_t>(size) / 4);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(code.data()), size);

        vk::ShaderModuleCreateInfo info{};
        info.codeSize = static_cast<std::size_t>(size);
        info.pCode = code.data();
        return checked(device.createShaderModule(info), std::format("createShaderModule '{}'", path));
    }

    auto newColorImage(Renderer& renderer, u32 width, u32 height,
                                                    vk::Format format, bool storage = false,
                                                    vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1) -> Result<Image>
    {
        vk::FormatProperties props = renderer.context.physicalDevice.getFormatProperties(format);
        vk::FormatFeatureFlags needed =
            vk::FormatFeatureFlagBits::eColorAttachment | vk::FormatFeatureFlagBits::eSampledImage;
        if (storage)
        {
            needed = needed | vk::FormatFeatureFlagBits::eStorageImage;
        }
        if ((props.optimalTilingFeatures & needed) != needed)
        {
            return Err(std::format("format {} cannot be a sampled color attachment", vk::to_string(format)));
        }

        // A multisampled image is a transient resolve source: color attachment only
        // (never sampled / stored / copied — the resolved 1x image is what we read).
        const bool multisampled = samples != vk::SampleCountFlagBits::e1;
        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = static_cast<VkFormat>(format);
        imageInfo.extent = VkExtent3D{ width, height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = static_cast<VkSampleCountFlagBits>(samples);
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (!multisampled)
        {
            imageInfo.usage = imageInfo.usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (storage)
            {
                imageInfo.usage = imageInfo.usage | VK_IMAGE_USAGE_STORAGE_BIT;
            }
        }
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

        VkImage rawImage = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        if (vmaCreateImage(renderer.context.allocator, &imageInfo, &allocInfo, &rawImage, &allocation, nullptr) != VK_SUCCESS)
        {
            return Err(std::string{ "vmaCreateImage failed for offscreen target" });
        }

        vk::ImageViewCreateInfo viewInfo{};
        viewInfo.image = vk::Image{ rawImage };
        viewInfo.viewType = vk::ImageViewType::e2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
        vk::ResultValue<vk::ImageView> view = renderer.context.device.createImageView(viewInfo);
        if (view.result != vk::Result::eSuccess)
        {
            vmaDestroyImage(renderer.context.allocator, rawImage, allocation);
            return Err(std::format("createImageView (offscreen): {}", vk::to_string(view.result)));
        }

        Image result;
        result.device = renderer.context.device;
        result.allocator = renderer.context.allocator;
        result.image = vk::Image{ rawImage };
        result.view = view.value;
        result.alloc = allocation;
        result.extent = vk::Extent2D{ width, height };
        result.format = format;
        result.layout = vk::ImageLayout::eUndefined;
        return result;
    }

    auto newDepthImage(Renderer& renderer, u32 width, u32 height,
                                                    vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1,
                                                    bool sampled = false) -> Result<Image>
    {
        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = static_cast<VkFormat>(DepthFormat);
        imageInfo.extent = VkExtent3D{ width, height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = static_cast<VkSampleCountFlagBits>(samples);
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (sampled)
        {
            imageInfo.usage = imageInfo.usage | VK_IMAGE_USAGE_SAMPLED_BIT;  // shadow map: sampled in the mesh fragment
        }
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

        VkImage rawImage = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        if (vmaCreateImage(renderer.context.allocator, &imageInfo, &allocInfo, &rawImage, &allocation, nullptr) != VK_SUCCESS)
        {
            return Err(std::string{ "vmaCreateImage failed for depth target" });
        }

        vk::ImageViewCreateInfo viewInfo{};
        viewInfo.image = vk::Image{ rawImage };
        viewInfo.viewType = vk::ImageViewType::e2D;
        viewInfo.format = DepthFormat;
        viewInfo.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1 };
        vk::ResultValue<vk::ImageView> view = renderer.context.device.createImageView(viewInfo);
        if (view.result != vk::Result::eSuccess)
        {
            vmaDestroyImage(renderer.context.allocator, rawImage, allocation);
            return Err(std::format("createImageView (depth): {}", vk::to_string(view.result)));
        }

        Image result;
        result.device = renderer.context.device;
        result.allocator = renderer.context.allocator;
        result.image = vk::Image{ rawImage };
        result.view = view.value;
        result.alloc = allocation;
        result.extent = vk::Extent2D{ width, height };
        result.format = DepthFormat;
        result.layout = vk::ImageLayout::eUndefined;
        return result;
    }

    // A cube-compatible image (6 array layers, N mips) usable both as a sampled cubemap
    // (the default eCube view) and as a 2D-array storage image (compute fills it via
    // per-mip transient views). Used for the IBL environment/irradiance/prefiltered cubes.
    auto newCubeImage(Renderer& renderer, u32 size, u32 mipLevels, vk::Format format) -> Result<Image>
    {
        vk::FormatProperties props = renderer.context.physicalDevice.getFormatProperties(format);
        vk::FormatFeatureFlags needed =
            vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eStorageImage;
        if ((props.optimalTilingFeatures & needed) != needed)
        {
            return Err(std::format("format {} cannot be a sampled+storage cube", vk::to_string(format)));
        }

        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = static_cast<VkFormat>(format);
        imageInfo.extent = VkExtent3D{ size, size, 1 };
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = 6;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VkImage rawImage = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        if (vmaCreateImage(renderer.context.allocator, &imageInfo, &allocInfo, &rawImage, &allocation, nullptr) != VK_SUCCESS)
        {
            return Err(std::string{ "vmaCreateImage failed for cube" });
        }

        vk::ImageViewCreateInfo viewInfo{};
        viewInfo.image = vk::Image{ rawImage };
        viewInfo.viewType = vk::ImageViewType::eCube;
        viewInfo.format = format;
        viewInfo.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 6 };
        vk::ResultValue<vk::ImageView> view = renderer.context.device.createImageView(viewInfo);
        if (view.result != vk::Result::eSuccess)
        {
            vmaDestroyImage(renderer.context.allocator, rawImage, allocation);
            return Err(std::format("createImageView (cube): {}", vk::to_string(view.result)));
        }

        Image result;
        result.device = renderer.context.device;
        result.allocator = renderer.context.allocator;
        result.image = vk::Image{ rawImage };
        result.view = view.value;
        result.alloc = allocation;
        result.extent = vk::Extent2D{ size, size };
        result.format = format;
        result.layout = vk::ImageLayout::eUndefined;
        return result;
    }

    // A cube-compatible COLOR image (6 layers) usable as a sampled cubemap (the eCube view
    // in `out`) and rendered face-by-face (the 6 single-layer e2D attachment views in
    // `outFaces`). Used for the point-light distance shadow cube.
    auto newColorCubeImage(Renderer& renderer, u32 size, vk::Format format,
                           Image& out, std::array<vk::ImageView, 6>& outFaces) -> Result<void>
    {
        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = static_cast<VkFormat>(format);
        imageInfo.extent = VkExtent3D{ size, size, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 6;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VkImage rawImage = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        if (vmaCreateImage(renderer.context.allocator, &imageInfo, &allocInfo, &rawImage, &allocation, nullptr) != VK_SUCCESS)
        {
            return Err(std::string{ "vmaCreateImage failed for color cube" });
        }

        vk::ImageViewCreateInfo cubeView{};
        cubeView.image = vk::Image{ rawImage };
        cubeView.viewType = vk::ImageViewType::eCube;
        cubeView.format = format;
        cubeView.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 6 };
        vk::ResultValue<vk::ImageView> sampleView = renderer.context.device.createImageView(cubeView);
        if (sampleView.result != vk::Result::eSuccess)
        {
            vmaDestroyImage(renderer.context.allocator, rawImage, allocation);
            return Err(std::format("createImageView (color cube): {}", vk::to_string(sampleView.result)));
        }

        for (u32 face = 0; face < 6; face = face + 1)
        {
            vk::ImageViewCreateInfo faceView{};
            faceView.image = vk::Image{ rawImage };
            faceView.viewType = vk::ImageViewType::e2D;
            faceView.format = format;
            faceView.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, face, 1 };
            vk::ResultValue<vk::ImageView> fv = renderer.context.device.createImageView(faceView);
            if (fv.result != vk::Result::eSuccess)
            {
                renderer.context.device.destroyImageView(sampleView.value);
                for (u32 f = 0; f < face; f = f + 1) { renderer.context.device.destroyImageView(outFaces[f]); }
                vmaDestroyImage(renderer.context.allocator, rawImage, allocation);
                return Err(std::format("createImageView (cube face): {}", vk::to_string(fv.result)));
            }
            outFaces[face] = fv.value;
        }

        out.device = renderer.context.device;
        out.allocator = renderer.context.allocator;
        out.image = vk::Image{ rawImage };
        out.view = sampleView.value;
        out.alloc = allocation;
        out.extent = vk::Extent2D{ size, size };
        out.format = format;
        out.layout = vk::ImageLayout::eUndefined;
        return {};
    }

    // The 6 cube-face world->clip matrices for a point light at `pos` (fovy 90, aspect 1).
    // GL/Vulkan cube convention; the Y flip is folded into the projection so the rendered
    // faces round-trip with a TextureCube sampled by world direction.
    auto pointShadowFaceMatrices(glm::vec3 pos, f32 farPlane) -> std::array<glm::mat4, 6>
    {
        glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.05f, glm::max(farPlane, 0.1f));
        proj[1][1] *= -1.0f;  // Vulkan framebuffer Y is down; flip so faces match cube sampling
        const std::array<glm::vec3, 6> fwd{
            glm::vec3( 1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0),
            glm::vec3( 0,-1, 0), glm::vec3( 0, 0, 1), glm::vec3(0, 0,-1) };
        const std::array<glm::vec3, 6> up{
            glm::vec3(0,-1, 0), glm::vec3(0,-1, 0), glm::vec3(0, 0, 1),
            glm::vec3(0, 0,-1), glm::vec3(0,-1, 0), glm::vec3(0,-1, 0) };
        std::array<glm::mat4, 6> result;
        for (u32 i = 0; i < 6; i = i + 1)
        {
            result[i] = proj * glm::lookAt(pos, pos + fwd[i], up[i]);
        }
        return result;
    }

    // (Re)create the multisampled scene color + depth targets at the offscreen extent
    // when MSAA is on; drop them when off. Called after the offscreen is sized + on a
    // sample-count change. The caller has already idled the GPU.
    void recreateMsaaTargets(Renderer& renderer)
    {
        renderer.targets.msaaColor.reset();
        renderer.targets.msaaDepth.reset();
        if (renderer.targets.sampleCount == vk::SampleCountFlagBits::e1)
        {
            return;
        }
        const u32 w = renderer.targets.offscreen.extent.width;
        const u32 h = renderer.targets.offscreen.extent.height;
        if (w == 0 || h == 0)
        {
            return;
        }
        Result<Image> color =
            newColorImage(renderer, w, h, OffscreenColorFormat, false, renderer.targets.sampleCount);
        if (color)
        {
            renderer.targets.msaaColor = std::move(*color);
        }
        else
        {
            logError(color.error());
        }
        auto depth = newDepthImage(renderer, w, h, renderer.targets.sampleCount);
        if (depth)
        {
            renderer.targets.msaaDepth = std::move(*depth);
        }
        else
        {
            logError(depth.error());
        }
    }

    void updateFxaaSet(Renderer& renderer);  // defined alongside updateTonemapSet below
    void recreateSsaoTargets(Renderer& renderer);  // defined after the SSAO pipeline helpers

    // (Re)create the 1x scratch target FXAA reads from (the scene renders here when
    // FXAA is on); drop it when off. Sized to the offscreen; the GPU is already idle.
    void recreateFxaaTarget(Renderer& renderer)
    {
        renderer.targets.scratch.reset();
        if (!renderer.targets.fxaaEnabled)
        {
            return;
        }
        const u32 w = renderer.targets.offscreen.extent.width;
        const u32 h = renderer.targets.offscreen.extent.height;
        if (w == 0 || h == 0)
        {
            return;
        }
        auto scratch = newColorImage(renderer, w, h, OffscreenColorFormat, false);
        if (scratch)
        {
            renderer.targets.scratch = std::move(*scratch);
            updateFxaaSet(renderer);
        }
        else
        {
            logError(scratch.error());
        }
    }

    // Host-visible, mapped buffer the caller owns (vmaDestroyBuffer when done).
    auto newHostCaptureBuffer(
        Renderer& renderer, vk::DeviceSize bytes,
        VkBuffer& outBuffer, VmaAllocation& outAlloc, VmaAllocationInfo& outInfo) -> Result<void>
    {
        VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(renderer.context.allocator, &bufferInfo, &allocInfo, &outBuffer, &outAlloc, &outInfo) != VK_SUCCESS)
        {
            return Err(std::string{ "capture: vmaCreateBuffer failed" });
        }
        return {};
    }

    // Records a fromLayout->TransferSrc barrier, the image->buffer copy, and a
    // TransferSrc->toLayout barrier into a caller-owned command buffer.
    void captureImageToBuffer(
        vk::CommandBuffer cmd, vk::Image image, vk::Extent2D extent,
        vk::ImageLayout fromLayout, vk::PipelineStageFlags2 fromStage, vk::AccessFlags2 fromAccess,
        vk::ImageLayout toLayout, vk::PipelineStageFlags2 toStage, vk::AccessFlags2 toAccess,
        vk::Buffer destination)
    {
        transitionImage(
            cmd, image, fromLayout, vk::ImageLayout::eTransferSrcOptimal,
            fromStage, fromAccess,
            vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);

        vk::BufferImageCopy region{};
        region.imageSubresource = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
        region.imageExtent = vk::Extent3D{ extent.width, extent.height, 1 };
        cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, destination, region);

        transitionImage(
            cmd, image, vk::ImageLayout::eTransferSrcOptimal, toLayout,
            vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead,
            toStage, toAccess);
    }

    // Bytes per pixel for the capture formats (8-bit RGBA/BGRA, or HDR RGBA16F).
    auto formatPixelBytes(vk::Format format) -> u32
    {
        if (format == vk::Format::eR16G16B16A16Sfloat)
        {
            return 8;
        }
        return 4;
    }

    // Writes a PNG. 8-bit sources reorder BGRA->RGB; the HDR (RGBA16F) offscreen is
    // already tonemapped to display range, so its half floats are clamped to [0,1]*255.
    auto writeBufferToPng(
        const unsigned char* pixels, u32 width, u32 height, vk::Format format, const std::string& path) -> Result<void>
    {
        std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * height * 3);
        if (format == vk::Format::eR16G16B16A16Sfloat)
        {
            const u32* halfs = reinterpret_cast<const u32*>(pixels);  // two halves per u32
            for (u32 i = 0; i < width * height; i = i + 1)
            {
                const glm::vec2 rg = glm::unpackHalf2x16(halfs[i * 2 + 0]);
                const glm::vec2 ba = glm::unpackHalf2x16(halfs[i * 2 + 1]);
                const auto encode = [](f32 c) -> unsigned char
                {
                    const f32 clamped = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
                    return static_cast<unsigned char>(clamped * 255.0f + 0.5f);
                };
                rgb[i * 3 + 0] = encode(rg.x);
                rgb[i * 3 + 1] = encode(rg.y);
                rgb[i * 3 + 2] = encode(ba.x);
            }
        }
        else
        {
            const bool bgr = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
            for (u32 i = 0; i < width * height; i = i + 1)
            {
                u32 r = i * 4 + 0;
                u32 b = i * 4 + 2;
                if (bgr)
                {
                    r = i * 4 + 2;
                    b = i * 4 + 0;
                }
                rgb[i * 3 + 0] = pixels[r];
                rgb[i * 3 + 1] = pixels[i * 4 + 1];
                rgb[i * 3 + 2] = pixels[b];
            }
        }
        const int ok = stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height),
                                      3, rgb.data(), static_cast<int>(width) * 3);
        if (ok == 0)
        {
            return Err(std::format("stbi_write_png failed for '{}'", path));
        }
        return {};
    }

    // Matches the shader's set 1 light uniform (std140).
    struct LightUbo
    {
        glm::vec4 directionAmbient;     // xyz direction, w ambient
        glm::vec4 colorIntensity;       // rgb color, a intensity
        glm::uvec4 counts;              // x=punctual count, y=dir shadow on, z=IBL on
        glm::vec4 eyePosition;          // xyz world-space camera position
        glm::mat4 shadowViewProj;       // directional light-space transform (world -> shadow clip)
        glm::mat4 spotShadowViewProj;   // shadowed spot light-space transform (perspective)
        glm::uvec4 spotShadow;          // x = shadowed spot's light index, y = enabled (0/1)
        glm::vec4 pointShadow;          // xyz = shadowed point light world pos, w = far plane
        glm::uvec4 pointShadowMeta;     // x = shadowed point's light index, y = enabled (0/1)
    };

    // Shadow-map resolution + slope/constant depth bias (units of D32 depth). Tuned on
    // llvmpipe to remove acne without obvious peter-panning.
    inline constexpr u32 ShadowMapSize = 2048;
    inline constexpr f32 ShadowDepthBiasConstant = 1.25f;
    inline constexpr f32 ShadowDepthBiasSlope = 2.0f;

    // Point shadow: per-face resolution of the omnidirectional distance cube + the
    // world-space distance bias used when comparing in the mesh fragment.
    inline constexpr u32 PointShadowSize = 512;
    inline constexpr vk::Format PointShadowColorFormat = vk::Format::eR32Sfloat;
    inline constexpr f32 PointShadowDistanceBias = 0.08f;

    // SSAO: the thin G-buffer stores a view-space normal (rgb) + view-Z (a); the AO map is
    // a single-channel factor. Both viewport-sized.
    inline constexpr vk::Format GNormalFormat = vk::Format::eR16G16B16A16Sfloat;
    inline constexpr vk::Format AoFormat = vk::Format::eR8Unorm;

    // IBL bake sizes. The environment is a procedural HDR sky; it is convolved into a
    // small diffuse-irradiance cube + a roughness-mipped prefiltered specular cube, plus
    // a split-sum BRDF LUT. All HDR float (rgba16f); the LUT uses only RG. Kept modest so
    // the one-time bake is quick on llvmpipe.
    inline constexpr vk::Format IblColorFormat = vk::Format::eR16G16B16A16Sfloat;
    inline constexpr u32 IblEnvSize = 128;
    inline constexpr u32 IblIrradianceSize = 32;
    inline constexpr u32 IblPrefilterSize = 128;
    inline constexpr u32 IblPrefilterMips = 5;  // mesh.slang's IblPrefilterMaxMip must be this - 1
    inline constexpr u32 IblLutSize = 256;

    // Froxel cluster grid (X x Y screen tiles, Z exponential view-space slices) and
    // the per-cluster light cap. Must match light_cull.slang + mesh.slang.
    inline constexpr u32 ClusterGridX = 16;
    inline constexpr u32 ClusterGridY = 9;
    inline constexpr u32 ClusterGridZ = 24;
    inline constexpr u32 ClusterCount = ClusterGridX * ClusterGridY * ClusterGridZ;
    inline constexpr u32 MaxLightsPerCluster = 64;
    // One cluster's light list: a count + a fixed slot of indices. Matches the
    // shader's Cluster struct (std430: tight u32 array).
    inline constexpr vk::DeviceSize ClusterStride = sizeof(u32) * (1 + MaxLightsPerCluster);

    // Matches both shaders' cluster-params UBO (std140).
    struct ClusterParams
    {
        glm::mat4 view;               // world -> view (cull: light positions; fragment: froxel Z)
        glm::mat4 inverseProjection;  // clip -> view (cull: tile AABB build)
        glm::uvec4 gridSize;          // xyz = grid dims, w = punctual light count
        glm::uvec4 screenSize;        // xy = offscreen pixel dims, z = clustered flag
        glm::vec4 zPlanes;            // x = near, y = far
    };

    // Initial punctual-light buffer capacity, grown on demand thereafter.
    inline constexpr u32 LightListInitial = 16;

    // A host-visible, persistently mapped storage buffer for per-frame uploads.
    auto makeMappedStorageBuffer(Renderer& renderer, vk::DeviceSize bytes) -> Result<Ref<Buffer>>
    {
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO;
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer raw = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VmaAllocationInfo mapped{};
        if (vmaCreateBuffer(renderer.context.allocator, &info, &alloc, &raw, &allocation, &mapped) != VK_SUCCESS)
        {
            return Err(std::string{ "makeMappedStorageBuffer: vmaCreateBuffer failed" });
        }
        Buffer buffer;
        buffer.allocator = renderer.context.allocator;
        buffer.buffer = vk::Buffer{ raw };
        buffer.alloc = allocation;
        buffer.mapped = mapped.pMappedData;
        buffer.size = bytes;
        return std::make_shared<Buffer>(std::move(buffer));
    }

    // A device-local storage buffer (no host access) — for GPU-only scratch like
    // the compute-written cluster light lists.
    auto makeDeviceStorageBuffer(Renderer& renderer, vk::DeviceSize bytes) -> Result<Ref<Buffer>>
    {
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO;
        VkBuffer raw = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        if (vmaCreateBuffer(renderer.context.allocator, &info, &alloc, &raw, &allocation, nullptr) != VK_SUCCESS)
        {
            return Err(std::string{ "makeDeviceStorageBuffer: vmaCreateBuffer failed" });
        }
        Buffer buffer;
        buffer.allocator = renderer.context.allocator;
        buffer.buffer = vk::Buffer{ raw };
        buffer.alloc = allocation;
        buffer.size = bytes;
        return std::make_shared<Buffer>(std::move(buffer));
    }

    // Builds a compute pipeline from a SPIR-V module (entry "computeMain") + a single
    // descriptor set layout, optionally with a compute-stage push constant of the given
    // size. Returned as a Ref<Pipeline> (move-only RAII).
    auto newComputePipeline(
        Renderer& renderer, std::string_view shaderName, vk::DescriptorSetLayout setLayout,
        u32 pushConstantSize = 0) -> Result<Ref<Pipeline>>
    {
        auto moduleResult = loadShaderModule(renderer.context.device, assetPath(shaderName));
        if (!moduleResult)
        {
            return Err(moduleResult.error());
        }
        vk::ShaderModule shaderModule = *moduleResult;

        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setSetLayouts(setLayout);
        vk::PushConstantRange pushRange{};
        if (pushConstantSize > 0)
        {
            pushRange.stageFlags = vk::ShaderStageFlagBits::eCompute;
            pushRange.offset = 0;
            pushRange.size = pushConstantSize;
            layoutInfo.setPushConstantRanges(pushRange);
        }
        auto layoutResult = checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (compute)");
        if (!layoutResult)
        {
            renderer.context.device.destroyShaderModule(shaderModule);
            return Err(layoutResult.error());
        }

        vk::PipelineShaderStageCreateInfo stage{};
        stage.stage = vk::ShaderStageFlagBits::eCompute;
        stage.module = shaderModule;
        stage.pName = "computeMain";

        vk::ComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.stage = stage;
        pipelineInfo.layout = *layoutResult;
        vk::ResultValue<vk::Pipeline> created = renderer.context.device.createComputePipeline(nullptr, pipelineInfo);
        renderer.context.device.destroyShaderModule(shaderModule);
        if (created.result != vk::Result::eSuccess)
        {
            renderer.context.device.destroyPipelineLayout(*layoutResult);
            return Err(std::format("createComputePipeline: {}", vk::to_string(created.result)));
        }

        Pipeline pipeline;
        pipeline.device = renderer.context.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;
        return std::make_shared<Pipeline>(std::move(pipeline));
    }

    // A vertex-only graphics pipeline for the depth pre-pass: it reuses the mesh
    // vertex shader (instance set 2 + viewProj push constant) but has no fragment
    // shader and no color attachment, so it only lays down depth (test+write LESS).
    // Its pipeline layout matches the mesh layout (so the same set 2 + push bind).
    auto makeDepthPrepassPipeline(Renderer& renderer, std::string_view shaderName) -> Result<Ref<Pipeline>>
    {
        auto moduleResult = loadShaderModule(renderer.context.device, assetPath(shaderName));
        if (!moduleResult)
        {
            return Err(moduleResult.error());
        }
        vk::ShaderModule shaderModule = *moduleResult;

        vk::PipelineShaderStageCreateInfo stage{};
        stage.stage = vk::ShaderStageFlagBits::eVertex;
        stage.module = shaderModule;
        stage.pName = "vertexMain";

        vk::VertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(Vertex);
        binding.inputRate = vk::VertexInputRate::eVertex;
        std::array<vk::VertexInputAttributeDescription, 3> attributes{
            vk::VertexInputAttributeDescription{ 0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position) },
            vk::VertexInputAttributeDescription{ 1, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, normal) },
            vk::VertexInputAttributeDescription{ 2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv0) } };
        vk::PipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.setVertexBindingDescriptions(binding);
        vertexInput.setVertexAttributeDescriptions(attributes);

        vk::PipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.topology = vk::PrimitiveTopology::eTriangleList;
        vk::PipelineViewportStateCreateInfo viewportState{};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;
        raster.frontFace = vk::FrontFace::eCounterClockwise;
        raster.lineWidth = 1.0f;
        vk::PipelineMultisampleStateCreateInfo multisample{};
        multisample.rasterizationSamples = renderer.targets.sampleCount;  // match the MSAA target
        vk::PipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = vk::CompareOp::eLess;
        vk::PipelineColorBlendStateCreateInfo colorBlend{};  // no color attachments
        std::array<vk::DynamicState, 2> dynamicStates{ vk::DynamicState::eViewport, vk::DynamicState::eScissor };
        vk::PipelineDynamicStateCreateInfo dynamic{};
        dynamic.setDynamicStates(dynamicStates);

        vk::PipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.depthAttachmentFormat = DepthFormat;

        vk::PushConstantRange pushConstant{};
        pushConstant.stageFlags = vk::ShaderStageFlagBits::eVertex;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(glm::mat4);
        std::array<vk::DescriptorSetLayout, 3> setLayouts{
            renderer.descriptors.bindlessSetLayout, renderer.descriptors.lightSetLayout, renderer.descriptors.instanceSetLayout };
        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setSetLayouts(setLayouts);
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult = checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (depth-prepass)");
        if (!layoutResult)
        {
            renderer.context.device.destroyShaderModule(shaderModule);
            return Err(layoutResult.error());
        }

        vk::GraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.setStages(stage);
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = *layoutResult;

        vk::ResultValue<vk::Pipeline> created = renderer.context.device.createGraphicsPipeline(nullptr, pipelineInfo);
        renderer.context.device.destroyShaderModule(shaderModule);
        if (created.result != vk::Result::eSuccess)
        {
            renderer.context.device.destroyPipelineLayout(*layoutResult);
            return Err(std::format("createGraphicsPipeline (depth-prepass): {}", vk::to_string(created.result)));
        }

        Pipeline pipeline;
        pipeline.device = renderer.context.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;
        return std::make_shared<Pipeline>(std::move(pipeline));
    }

    // A depth-only pipeline that renders the scene from a light's point of view into the
    // shadow map. Like makeDepthPrepassPipeline but always single-sampled (the shadow map
    // is 1x) and depth-biased (dynamic) to kill shadow acne. Reuses the mesh vertex shader.
    auto makeShadowPipeline(Renderer& renderer, std::string_view shaderName) -> Result<Ref<Pipeline>>
    {
        auto moduleResult = loadShaderModule(renderer.context.device, assetPath(shaderName));
        if (!moduleResult)
        {
            return Err(moduleResult.error());
        }
        vk::ShaderModule shaderModule = *moduleResult;

        vk::PipelineShaderStageCreateInfo stage{};
        stage.stage = vk::ShaderStageFlagBits::eVertex;
        stage.module = shaderModule;
        stage.pName = "vertexMain";

        vk::VertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(Vertex);
        binding.inputRate = vk::VertexInputRate::eVertex;
        std::array<vk::VertexInputAttributeDescription, 3> attributes{
            vk::VertexInputAttributeDescription{ 0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position) },
            vk::VertexInputAttributeDescription{ 1, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, normal) },
            vk::VertexInputAttributeDescription{ 2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv0) } };
        vk::PipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.setVertexBindingDescriptions(binding);
        vertexInput.setVertexAttributeDescriptions(attributes);

        vk::PipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.topology = vk::PrimitiveTopology::eTriangleList;
        vk::PipelineViewportStateCreateInfo viewportState{};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;
        raster.frontFace = vk::FrontFace::eCounterClockwise;
        raster.depthBiasEnable = VK_TRUE;  // set dynamically per shadow pass
        raster.lineWidth = 1.0f;
        vk::PipelineMultisampleStateCreateInfo multisample{};
        multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;  // the shadow map is never multisampled
        vk::PipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = vk::CompareOp::eLess;
        vk::PipelineColorBlendStateCreateInfo colorBlend{};  // no color attachments
        std::array<vk::DynamicState, 3> dynamicStates{
            vk::DynamicState::eViewport, vk::DynamicState::eScissor, vk::DynamicState::eDepthBias };
        vk::PipelineDynamicStateCreateInfo dynamic{};
        dynamic.setDynamicStates(dynamicStates);

        vk::PipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.depthAttachmentFormat = DepthFormat;

        vk::PushConstantRange pushConstant{};
        pushConstant.stageFlags = vk::ShaderStageFlagBits::eVertex;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(glm::mat4);
        std::array<vk::DescriptorSetLayout, 3> setLayouts{
            renderer.descriptors.bindlessSetLayout, renderer.descriptors.lightSetLayout, renderer.descriptors.instanceSetLayout };
        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setSetLayouts(setLayouts);
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult = checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (shadow)");
        if (!layoutResult)
        {
            renderer.context.device.destroyShaderModule(shaderModule);
            return Err(layoutResult.error());
        }

        vk::GraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.setStages(stage);
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = *layoutResult;

        vk::ResultValue<vk::Pipeline> created = renderer.context.device.createGraphicsPipeline(nullptr, pipelineInfo);
        renderer.context.device.destroyShaderModule(shaderModule);
        if (created.result != vk::Result::eSuccess)
        {
            renderer.context.device.destroyPipelineLayout(*layoutResult);
            return Err(std::format("createGraphicsPipeline (shadow): {}", vk::to_string(created.result)));
        }

        Pipeline pipeline;
        pipeline.device = renderer.context.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;
        return std::make_shared<Pipeline>(std::move(pipeline));
    }

    // A color+depth pipeline that renders world-space distance-to-light into one point
    // shadow cube face. Reuses point_shadow.slang (instanced vertex + distance fragment);
    // push constant = face viewProj (mat4) + light world pos (vec4). Layout uses only the
    // instance set (set 2), like the depth pre-pass.
    auto makePointShadowPipeline(Renderer& renderer, std::string_view shaderName) -> Result<Ref<Pipeline>>
    {
        auto moduleResult = loadShaderModule(renderer.context.device, assetPath(shaderName));
        if (!moduleResult)
        {
            return Err(moduleResult.error());
        }
        vk::ShaderModule shaderModule = *moduleResult;

        std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
        stages[0].stage = vk::ShaderStageFlagBits::eVertex;
        stages[0].module = shaderModule;
        stages[0].pName = "vertexMain";
        stages[1].stage = vk::ShaderStageFlagBits::eFragment;
        stages[1].module = shaderModule;
        stages[1].pName = "fragmentMain";

        vk::VertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(Vertex);
        binding.inputRate = vk::VertexInputRate::eVertex;
        std::array<vk::VertexInputAttributeDescription, 3> attributes{
            vk::VertexInputAttributeDescription{ 0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position) },
            vk::VertexInputAttributeDescription{ 1, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, normal) },
            vk::VertexInputAttributeDescription{ 2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv0) } };
        vk::PipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.setVertexBindingDescriptions(binding);
        vertexInput.setVertexAttributeDescriptions(attributes);

        vk::PipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.topology = vk::PrimitiveTopology::eTriangleList;
        vk::PipelineViewportStateCreateInfo viewportState{};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;
        raster.frontFace = vk::FrontFace::eCounterClockwise;
        raster.lineWidth = 1.0f;
        vk::PipelineMultisampleStateCreateInfo multisample{};
        multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
        vk::PipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = vk::CompareOp::eLess;
        vk::PipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.setAttachments(blendAttachment);
        std::array<vk::DynamicState, 2> dynamicStates{ vk::DynamicState::eViewport, vk::DynamicState::eScissor };
        vk::PipelineDynamicStateCreateInfo dynamic{};
        dynamic.setDynamicStates(dynamicStates);

        vk::PipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.setColorAttachmentFormats(PointShadowColorFormat);
        renderingInfo.depthAttachmentFormat = DepthFormat;

        vk::PushConstantRange pushConstant{};
        pushConstant.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(glm::mat4) + sizeof(glm::vec4);
        std::array<vk::DescriptorSetLayout, 3> setLayouts{
            renderer.descriptors.bindlessSetLayout, renderer.descriptors.lightSetLayout, renderer.descriptors.instanceSetLayout };
        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setSetLayouts(setLayouts);
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult = checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (point-shadow)");
        if (!layoutResult)
        {
            renderer.context.device.destroyShaderModule(shaderModule);
            return Err(layoutResult.error());
        }

        vk::GraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.setStages(stages);
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = *layoutResult;

        vk::ResultValue<vk::Pipeline> created = renderer.context.device.createGraphicsPipeline(nullptr, pipelineInfo);
        renderer.context.device.destroyShaderModule(shaderModule);
        if (created.result != vk::Result::eSuccess)
        {
            renderer.context.device.destroyPipelineLayout(*layoutResult);
            return Err(std::format("createGraphicsPipeline (point-shadow): {}", vk::to_string(created.result)));
        }
        Pipeline pipeline;
        pipeline.device = renderer.context.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;
        return std::make_shared<Pipeline>(std::move(pipeline));
    }

    // The thin G-buffer prepass pipeline: writes view-space normal (rgb) + view-Z (a) into
    // an rgba16f color target + its own depth. Instanced (set 2); push = viewProj + view.
    // Reuses gbuffer.slang. The layout declares sets {bindless, light, instance} for set-2
    // binding compatibility, like the depth pre-pass.
    auto makeGbufferPipeline(Renderer& renderer, std::string_view shaderName) -> Result<Ref<Pipeline>>
    {
        auto moduleResult = loadShaderModule(renderer.context.device, assetPath(shaderName));
        if (!moduleResult)
        {
            return Err(moduleResult.error());
        }
        vk::ShaderModule shaderModule = *moduleResult;

        std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
        stages[0].stage = vk::ShaderStageFlagBits::eVertex;
        stages[0].module = shaderModule;
        stages[0].pName = "vertexMain";
        stages[1].stage = vk::ShaderStageFlagBits::eFragment;
        stages[1].module = shaderModule;
        stages[1].pName = "fragmentMain";

        vk::VertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(Vertex);
        binding.inputRate = vk::VertexInputRate::eVertex;
        std::array<vk::VertexInputAttributeDescription, 3> attributes{
            vk::VertexInputAttributeDescription{ 0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position) },
            vk::VertexInputAttributeDescription{ 1, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, normal) },
            vk::VertexInputAttributeDescription{ 2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv0) } };
        vk::PipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.setVertexBindingDescriptions(binding);
        vertexInput.setVertexAttributeDescriptions(attributes);

        vk::PipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.topology = vk::PrimitiveTopology::eTriangleList;
        vk::PipelineViewportStateCreateInfo viewportState{};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        raster.cullMode = vk::CullModeFlagBits::eNone;
        raster.frontFace = vk::FrontFace::eCounterClockwise;
        raster.lineWidth = 1.0f;
        vk::PipelineMultisampleStateCreateInfo multisample{};
        multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;  // the G-buffer is 1x (SSAO is post-resolve)
        vk::PipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = vk::CompareOp::eLess;
        vk::PipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.setAttachments(blendAttachment);
        std::array<vk::DynamicState, 2> dynamicStates{ vk::DynamicState::eViewport, vk::DynamicState::eScissor };
        vk::PipelineDynamicStateCreateInfo dynamic{};
        dynamic.setDynamicStates(dynamicStates);

        vk::PipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.setColorAttachmentFormats(GNormalFormat);
        renderingInfo.depthAttachmentFormat = DepthFormat;

        vk::PushConstantRange pushConstant{};
        pushConstant.stageFlags = vk::ShaderStageFlagBits::eVertex;
        pushConstant.offset = 0;
        pushConstant.size = 2 * sizeof(glm::mat4);
        std::array<vk::DescriptorSetLayout, 3> setLayouts{
            renderer.descriptors.bindlessSetLayout, renderer.descriptors.lightSetLayout, renderer.descriptors.instanceSetLayout };
        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setSetLayouts(setLayouts);
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult = checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (gbuffer)");
        if (!layoutResult)
        {
            renderer.context.device.destroyShaderModule(shaderModule);
            return Err(layoutResult.error());
        }

        vk::GraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.setStages(stages);
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = *layoutResult;

        vk::ResultValue<vk::Pipeline> created = renderer.context.device.createGraphicsPipeline(nullptr, pipelineInfo);
        renderer.context.device.destroyShaderModule(shaderModule);
        if (created.result != vk::Result::eSuccess)
        {
            renderer.context.device.destroyPipelineLayout(*layoutResult);
            return Err(std::format("createGraphicsPipeline (gbuffer): {}", vk::to_string(created.result)));
        }
        Pipeline pipeline;
        pipeline.device = renderer.context.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;
        return std::make_shared<Pipeline>(std::move(pipeline));
    }

    // (Re)creates the SSAO targets (G-buffer normal+viewZ, its depth, AO map) at the
    // viewport extent and rewrites the GTAO + mesh-AO descriptor sets. Called at init and
    // after the offscreen resizes (the caller has idled the GPU). The AO map is cleared to
    // white so a fresh map reads "fully open" before the first GTAO dispatch.
    void recreateSsaoTargets(Renderer& renderer)
    {
        renderer.ssao.ready = false;
        renderer.targets.gNormal.reset();
        renderer.targets.gDepth.reset();
        renderer.targets.aoMap.reset();
        const u32 w = renderer.targets.offscreen.extent.width;
        const u32 h = renderer.targets.offscreen.extent.height;
        if (w == 0 || h == 0)
        {
            return;
        }
        auto gNormal = newColorImage(renderer, w, h, GNormalFormat, false);
        if (!gNormal) { logError(gNormal.error()); return; }
        renderer.targets.gNormal = std::move(*gNormal);
        auto gDepth = newDepthImage(renderer, w, h);
        if (!gDepth) { logError(gDepth.error()); return; }
        renderer.targets.gDepth = std::move(*gDepth);
        auto aoMap = newColorImage(renderer, w, h, AoFormat, true);  // storage (GTAO writes it)
        if (!aoMap) { logError(aoMap.error()); return; }
        renderer.targets.aoMap = std::move(*aoMap);

        // Transition the AO map to ShaderReadOnly so the mesh's set-4 sample is valid even
        // before GTAO first runs. No clear is needed: the shader only samples it when SSAO
        // is enabled (counts.w), and GTAO writes the whole map earlier in that same frame.
        {
            vk::CommandBufferAllocateInfo allocInfo{};
            allocInfo.commandPool = renderer.frame.frames[0].commandPool;
            allocInfo.level = vk::CommandBufferLevel::ePrimary;
            allocInfo.commandBufferCount = 1;
            auto cmds = checked(renderer.context.device.allocateCommandBuffers(allocInfo), "ssao init cmd");
            if (!cmds) { logError(cmds.error()); return; }
            vk::CommandBuffer cmd = (*cmds)[0];
            vk::CommandBufferBeginInfo begin{};
            begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
            static_cast<void>(cmd.begin(begin));
            transitionImage(cmd, renderer.targets.aoMap.image,
                vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
            static_cast<void>(cmd.end());
            vk::CommandBufferSubmitInfo cmdInfo{};
            cmdInfo.commandBuffer = cmd;
            vk::SubmitInfo2 submitInfo{};
            submitInfo.setCommandBufferInfos(cmdInfo);
            static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, nullptr));
            static_cast<void>(renderer.context.device.waitIdle());
            renderer.context.device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);
            renderer.targets.aoMap.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
        }

        // GTAO compute set: g-buffer (sampler) + AO map (storage).
        vk::DescriptorImageInfo gInfo{ renderer.ssao.sampler, renderer.targets.gNormal.view, vk::ImageLayout::eShaderReadOnlyOptimal };
        vk::DescriptorImageInfo aoStore{};
        aoStore.imageView = renderer.targets.aoMap.view;
        aoStore.imageLayout = vk::ImageLayout::eGeneral;
        std::array<vk::WriteDescriptorSet, 2> gtaoWrites{};
        gtaoWrites[0].dstSet = renderer.ssao.gtaoSet;
        gtaoWrites[0].dstBinding = 0;
        gtaoWrites[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        gtaoWrites[0].setImageInfo(gInfo);
        gtaoWrites[1].dstSet = renderer.ssao.gtaoSet;
        gtaoWrites[1].dstBinding = 1;
        gtaoWrites[1].descriptorType = vk::DescriptorType::eStorageImage;
        gtaoWrites[1].setImageInfo(aoStore);
        renderer.context.device.updateDescriptorSets(gtaoWrites, {});

        // Mesh AO set (set 4): the AO map sampled with the linear sampler.
        vk::DescriptorImageInfo aoSampled{ renderer.descriptors.linearSampler, renderer.targets.aoMap.view, vk::ImageLayout::eShaderReadOnlyOptimal };
        vk::WriteDescriptorSet aoWrite{};
        aoWrite.dstSet = renderer.ssao.aoSet;
        aoWrite.dstBinding = 0;
        aoWrite.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        aoWrite.setImageInfo(aoSampled);
        renderer.context.device.updateDescriptorSets(aoWrite, {});

        renderer.ssao.generation = renderer.ssao.generation + 1;
        renderer.ssao.ready = true;
    }

    // Write a texture into the bindless array at the given slot (set 0, binding 0).
    // update-after-bind: safe to write a live set between draws.
    void writeBindlessTexture(Renderer& renderer, vk::ImageView view, u32 index)
    {
        vk::DescriptorImageInfo info{ renderer.descriptors.linearSampler, view, vk::ImageLayout::eShaderReadOnlyOptimal };
        vk::WriteDescriptorSet write{};
        write.dstSet = renderer.descriptors.bindlessSet;
        write.dstBinding = 0;
        write.dstArrayElement = index;
        write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        write.setImageInfo(info);
        renderer.context.device.updateDescriptorSets(write, {});
    }

    // Point the tonemap set's storage-image binding at the current offscreen color
    // view (GENERAL layout). Called after the offscreen color is (re)created.
    void updateTonemapSet(Renderer& renderer)
    {
        if (!renderer.descriptors.tonemapSet)
        {
            return;
        }
        vk::DescriptorImageInfo imageInfo{};
        imageInfo.imageView = renderer.targets.offscreen.view;
        imageInfo.imageLayout = vk::ImageLayout::eGeneral;
        vk::WriteDescriptorSet write{};
        write.dstSet = renderer.descriptors.tonemapSet;
        write.dstBinding = 0;
        write.descriptorType = vk::DescriptorType::eStorageImage;
        write.setImageInfo(imageInfo);
        renderer.context.device.updateDescriptorSets(write, {});
    }

    // Point the FXAA set at the current scratch (sampled source) + offscreen (storage
    // target) views. Called after either is (re)created.
    void updateFxaaSet(Renderer& renderer)
    {
        if (!renderer.descriptors.fxaaSet || !renderer.targets.scratch.view)
        {
            return;
        }
        vk::DescriptorImageInfo sourceInfo{ renderer.descriptors.linearSampler, renderer.targets.scratch.view,
                                            vk::ImageLayout::eShaderReadOnlyOptimal };
        vk::DescriptorImageInfo targetInfo{};
        targetInfo.imageView = renderer.targets.offscreen.view;
        targetInfo.imageLayout = vk::ImageLayout::eGeneral;
        std::array<vk::WriteDescriptorSet, 2> writes{};
        writes[0].dstSet = renderer.descriptors.fxaaSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[0].setImageInfo(sourceInfo);
        writes[1].dstSet = renderer.descriptors.fxaaSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = vk::DescriptorType::eStorageImage;
        writes[1].setImageInfo(targetInfo);
        renderer.context.device.updateDescriptorSets(writes, {});
    }

    // The shared sampler, material/light set layouts, descriptor pool, and the
    // per-frame light UBO + its set. Called once in newRenderer.
    auto initDescriptorResources(Renderer& renderer) -> Result<void>
    {
        vk::SamplerCreateInfo samplerInfo{};
        samplerInfo.magFilter = vk::Filter::eLinear;
        samplerInfo.minFilter = vk::Filter::eLinear;
        samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
        samplerInfo.addressModeU = vk::SamplerAddressMode::eRepeat;
        samplerInfo.addressModeV = vk::SamplerAddressMode::eRepeat;
        samplerInfo.addressModeW = vk::SamplerAddressMode::eRepeat;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        auto sampler = checked(renderer.context.device.createSampler(samplerInfo), "createSampler");
        if (!sampler)
        {
            return Err(sampler.error());
        }
        renderer.descriptors.linearSampler = *sampler;

        // Depth-compare sampler for PCF shadow lookups: linear filtering across the 2x2
        // compare results, clamp to a white (lit) border so off-map samples are unshadowed.
        vk::SamplerCreateInfo shadowSamplerInfo{};
        shadowSamplerInfo.magFilter = vk::Filter::eLinear;
        shadowSamplerInfo.minFilter = vk::Filter::eLinear;
        shadowSamplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
        shadowSamplerInfo.addressModeU = vk::SamplerAddressMode::eClampToBorder;
        shadowSamplerInfo.addressModeV = vk::SamplerAddressMode::eClampToBorder;
        shadowSamplerInfo.addressModeW = vk::SamplerAddressMode::eClampToBorder;
        shadowSamplerInfo.borderColor = vk::BorderColor::eFloatOpaqueWhite;
        shadowSamplerInfo.compareEnable = VK_TRUE;
        shadowSamplerInfo.compareOp = vk::CompareOp::eLessOrEqual;
        auto shadowSampler = checked(renderer.context.device.createSampler(shadowSamplerInfo), "createSampler (shadow)");
        if (!shadowSampler)
        {
            return Err(shadowSampler.error());
        }
        renderer.descriptors.shadowSampler = *shadowSampler;

        // Directional + spot shadow maps: persistent sampled depth targets. Transition both
        // once to ShaderReadOnly so their descriptors are valid even on frames where a
        // shadow pass doesn't run (toggled off / no caster) — the shader gates the sample.
        auto shadowMap = newDepthImage(renderer, ShadowMapSize, ShadowMapSize, vk::SampleCountFlagBits::e1, true);
        if (!shadowMap)
        {
            return Err(shadowMap.error());
        }
        renderer.targets.shadowMap = std::move(*shadowMap);
        auto spotShadowMap = newDepthImage(renderer, ShadowMapSize, ShadowMapSize, vk::SampleCountFlagBits::e1, true);
        if (!spotShadowMap)
        {
            return Err(spotShadowMap.error());
        }
        renderer.targets.spotShadowMap = std::move(*spotShadowMap);

        // Point shadow: a color distance cube (+ per-face render views) + a depth scratch.
        if (Result<void> cube = newColorCubeImage(renderer, PointShadowSize, PointShadowColorFormat,
                                                  renderer.targets.pointShadowCube, renderer.targets.pointShadowFaces); !cube)
        {
            return Err(cube.error());
        }
        auto pointDepth = newDepthImage(renderer, PointShadowSize, PointShadowSize);
        if (!pointDepth)
        {
            return Err(pointDepth.error());
        }
        renderer.targets.pointShadowDepth = std::move(*pointDepth);
        {
            vk::CommandBufferAllocateInfo allocInfo{};
            allocInfo.commandPool = renderer.frame.frames[0].commandPool;
            allocInfo.level = vk::CommandBufferLevel::ePrimary;
            allocInfo.commandBufferCount = 1;
            auto cmds = checked(renderer.context.device.allocateCommandBuffers(allocInfo), "shadow init cmd");
            if (!cmds)
            {
                return Err(cmds.error());
            }
            vk::CommandBuffer cmd = (*cmds)[0];
            vk::CommandBufferBeginInfo begin{};
            begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
            static_cast<void>(cmd.begin(begin));
            for (vk::Image map : { renderer.targets.shadowMap.image, renderer.targets.spotShadowMap.image })
            {
                transitionImage(cmd, map,
                    vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                    vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
                    vk::ImageAspectFlagBits::eDepth);
            }
            // The point distance cube starts ShaderReadOnly too (all 6 layers).
            {
                vk::ImageMemoryBarrier2 b{};
                b.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
                b.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
                b.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
                b.oldLayout = vk::ImageLayout::eUndefined;
                b.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
                b.image = renderer.targets.pointShadowCube.image;
                b.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 6 };
                vk::DependencyInfo d{};
                d.setImageMemoryBarriers(b);
                cmd.pipelineBarrier2(d);
            }
            static_cast<void>(cmd.end());
            vk::CommandBufferSubmitInfo cmdInfo{};
            cmdInfo.commandBuffer = cmd;
            vk::SubmitInfo2 submitInfo{};
            submitInfo.setCommandBufferInfos(cmdInfo);
            static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, nullptr));
            static_cast<void>(renderer.context.device.waitIdle());
            renderer.context.device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);
            renderer.targets.shadowMap.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
            renderer.targets.spotShadowMap.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
            renderer.targets.pointShadowCube.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
        }

        // Set 0 is the bindless albedo array: a runtime-sized combined-image-sampler
        // array, partially bound (not every slot filled) + update-after-bind (new
        // textures written into live slots). Indexed per-instance in the shader.
        vk::DescriptorSetLayoutBinding albedoBinding{};
        albedoBinding.binding = 0;
        albedoBinding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        albedoBinding.descriptorCount = MaxBindlessTextures;
        albedoBinding.stageFlags = vk::ShaderStageFlagBits::eFragment;
        vk::DescriptorBindingFlags bindlessFlags =
            vk::DescriptorBindingFlagBits::ePartiallyBound | vk::DescriptorBindingFlagBits::eUpdateAfterBind;
        vk::DescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
        bindingFlagsInfo.setBindingFlags(bindlessFlags);
        vk::DescriptorSetLayoutCreateInfo materialLayoutInfo{};
        materialLayoutInfo.setBindings(albedoBinding);
        materialLayoutInfo.flags = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
        materialLayoutInfo.pNext = &bindingFlagsInfo;
        auto materialLayout = checked(renderer.context.device.createDescriptorSetLayout(materialLayoutInfo), "bindlessSetLayout");
        if (!materialLayout)
        {
            return Err(materialLayout.error());
        }
        renderer.descriptors.bindlessSetLayout = *materialLayout;

        std::array<vk::DescriptorSetLayoutBinding, 7> lightBindings{};
        lightBindings[0].binding = 0;  // directional + ambient + counts UBO
        lightBindings[0].descriptorType = vk::DescriptorType::eUniformBuffer;
        lightBindings[0].descriptorCount = 1;
        lightBindings[0].stageFlags = vk::ShaderStageFlagBits::eFragment;
        lightBindings[1].binding = 1;  // punctual light storage buffer
        lightBindings[1].descriptorType = vk::DescriptorType::eStorageBuffer;
        lightBindings[1].descriptorCount = 1;
        lightBindings[1].stageFlags = vk::ShaderStageFlagBits::eFragment;
        lightBindings[2].binding = 2;  // per-cluster light lists (read)
        lightBindings[2].descriptorType = vk::DescriptorType::eStorageBuffer;
        lightBindings[2].descriptorCount = 1;
        lightBindings[2].stageFlags = vk::ShaderStageFlagBits::eFragment;
        lightBindings[3].binding = 3;  // cluster params UBO
        lightBindings[3].descriptorType = vk::DescriptorType::eUniformBuffer;
        lightBindings[3].descriptorCount = 1;
        lightBindings[3].stageFlags = vk::ShaderStageFlagBits::eFragment;
        lightBindings[4].binding = 4;  // directional shadow map (depth-compare sampler)
        lightBindings[4].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        lightBindings[4].descriptorCount = 1;
        lightBindings[4].stageFlags = vk::ShaderStageFlagBits::eFragment;
        lightBindings[5].binding = 5;  // spot shadow map (depth-compare sampler)
        lightBindings[5].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        lightBindings[5].descriptorCount = 1;
        lightBindings[5].stageFlags = vk::ShaderStageFlagBits::eFragment;
        lightBindings[6].binding = 6;  // point shadow distance cube (linear sampler)
        lightBindings[6].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        lightBindings[6].descriptorCount = 1;
        lightBindings[6].stageFlags = vk::ShaderStageFlagBits::eFragment;
        vk::DescriptorSetLayoutCreateInfo lightLayoutInfo{};
        lightLayoutInfo.setBindings(lightBindings);
        auto lightLayout = checked(renderer.context.device.createDescriptorSetLayout(lightLayoutInfo), "lightSetLayout");
        if (!lightLayout)
        {
            return Err(lightLayout.error());
        }
        renderer.descriptors.lightSetLayout = *lightLayout;

        std::array<vk::DescriptorSetLayoutBinding, 3> clusterBindings{};
        clusterBindings[0].binding = 0;  // cluster params UBO
        clusterBindings[0].descriptorType = vk::DescriptorType::eUniformBuffer;
        clusterBindings[0].descriptorCount = 1;
        clusterBindings[0].stageFlags = vk::ShaderStageFlagBits::eCompute;
        clusterBindings[1].binding = 1;  // punctual light storage buffer (read)
        clusterBindings[1].descriptorType = vk::DescriptorType::eStorageBuffer;
        clusterBindings[1].descriptorCount = 1;
        clusterBindings[1].stageFlags = vk::ShaderStageFlagBits::eCompute;
        clusterBindings[2].binding = 2;  // per-cluster light lists (write)
        clusterBindings[2].descriptorType = vk::DescriptorType::eStorageBuffer;
        clusterBindings[2].descriptorCount = 1;
        clusterBindings[2].stageFlags = vk::ShaderStageFlagBits::eCompute;
        vk::DescriptorSetLayoutCreateInfo clusterLayoutInfo{};
        clusterLayoutInfo.setBindings(clusterBindings);
        auto clusterLayout = checked(renderer.context.device.createDescriptorSetLayout(clusterLayoutInfo), "clusterSetLayout");
        if (!clusterLayout)
        {
            return Err(clusterLayout.error());
        }
        renderer.descriptors.clusterSetLayout = *clusterLayout;

        vk::DescriptorSetLayoutBinding instanceBinding{};
        instanceBinding.binding = 0;
        instanceBinding.descriptorType = vk::DescriptorType::eStorageBuffer;
        instanceBinding.descriptorCount = 1;
        instanceBinding.stageFlags = vk::ShaderStageFlagBits::eVertex;
        vk::DescriptorSetLayoutCreateInfo instanceLayoutInfo{};
        instanceLayoutInfo.setBindings(instanceBinding);
        auto instanceLayout = checked(renderer.context.device.createDescriptorSetLayout(instanceLayoutInfo), "instanceSetLayout");
        if (!instanceLayout)
        {
            return Err(instanceLayout.error());
        }
        renderer.descriptors.instanceSetLayout = *instanceLayout;

        vk::DescriptorSetLayoutBinding tonemapBinding{};
        tonemapBinding.binding = 0;  // the offscreen color as a storage image
        tonemapBinding.descriptorType = vk::DescriptorType::eStorageImage;
        tonemapBinding.descriptorCount = 1;
        tonemapBinding.stageFlags = vk::ShaderStageFlagBits::eCompute;
        vk::DescriptorSetLayoutCreateInfo tonemapLayoutInfo{};
        tonemapLayoutInfo.setBindings(tonemapBinding);
        auto tonemapLayout = checked(renderer.context.device.createDescriptorSetLayout(tonemapLayoutInfo), "tonemapSetLayout");
        if (!tonemapLayout)
        {
            return Err(tonemapLayout.error());
        }
        renderer.descriptors.tonemapSetLayout = *tonemapLayout;

        std::array<vk::DescriptorSetLayoutBinding, 2> fxaaBindings{};
        fxaaBindings[0].binding = 0;  // source (scene scratch) sampler
        fxaaBindings[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        fxaaBindings[0].descriptorCount = 1;
        fxaaBindings[0].stageFlags = vk::ShaderStageFlagBits::eCompute;
        fxaaBindings[1].binding = 1;  // target (offscreen) storage image
        fxaaBindings[1].descriptorType = vk::DescriptorType::eStorageImage;
        fxaaBindings[1].descriptorCount = 1;
        fxaaBindings[1].stageFlags = vk::ShaderStageFlagBits::eCompute;
        vk::DescriptorSetLayoutCreateInfo fxaaLayoutInfo{};
        fxaaLayoutInfo.setBindings(fxaaBindings);
        auto fxaaLayout = checked(renderer.context.device.createDescriptorSetLayout(fxaaLayoutInfo), "fxaaSetLayout");
        if (!fxaaLayout)
        {
            return Err(fxaaLayout.error());
        }
        renderer.descriptors.fxaaSetLayout = *fxaaLayout;

        std::array<vk::DescriptorPoolSize, 4> poolSizes{
            vk::DescriptorPoolSize{ vk::DescriptorType::eCombinedImageSampler, 1024 },
            vk::DescriptorPoolSize{ vk::DescriptorType::eUniformBuffer, 4 * MaxFramesInFlight + 8 },
            vk::DescriptorPoolSize{ vk::DescriptorType::eStorageBuffer, 8 * MaxFramesInFlight + 8 },
            vk::DescriptorPoolSize{ vk::DescriptorType::eStorageImage, 4 } };
        vk::DescriptorPoolCreateInfo poolInfo{};
        poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;  // texture sets freed on Ref drop
        poolInfo.maxSets = 1024 + 8 * MaxFramesInFlight + 16;
        poolInfo.setPoolSizes(poolSizes);
        auto pool = checked(renderer.context.device.createDescriptorPool(poolInfo), "descriptorPool");
        if (!pool)
        {
            return Err(pool.error());
        }
        renderer.descriptors.descriptorPool = *pool;

        // The bindless set comes from its own update-after-bind pool.
        vk::DescriptorPoolSize bindlessPoolSize{ vk::DescriptorType::eCombinedImageSampler, MaxBindlessTextures };
        vk::DescriptorPoolCreateInfo bindlessPoolInfo{};
        bindlessPoolInfo.flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind;
        bindlessPoolInfo.maxSets = 1;
        bindlessPoolInfo.setPoolSizes(bindlessPoolSize);
        auto bindlessPoolResult = checked(renderer.context.device.createDescriptorPool(bindlessPoolInfo), "bindlessPool");
        if (!bindlessPoolResult)
        {
            return Err(bindlessPoolResult.error());
        }
        renderer.descriptors.bindlessPool = *bindlessPoolResult;

        vk::DescriptorSetAllocateInfo bindlessAlloc{};
        bindlessAlloc.descriptorPool = renderer.descriptors.bindlessPool;
        bindlessAlloc.setSetLayouts(renderer.descriptors.bindlessSetLayout);
        auto bindlessAllocated = checked(renderer.context.device.allocateDescriptorSets(bindlessAlloc), "allocate bindlessSet");
        if (!bindlessAllocated)
        {
            return Err(bindlessAllocated.error());
        }
        renderer.descriptors.bindlessSet = (*bindlessAllocated)[0];

        for (u32 i = 0; i < MaxFramesInFlight; i = i + 1)
        {
            VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bufferInfo.size = sizeof(LightUbo);
            bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo mapped{};
            if (vmaCreateBuffer(renderer.context.allocator, &bufferInfo, &allocInfo, &renderer.lighting.lightBuffers[i], &renderer.lighting.lightAllocs[i], &mapped) != VK_SUCCESS)
            {
                return Err(std::string{ "light UBO vmaCreateBuffer failed" });
            }
            renderer.lighting.lightMapped[i] = mapped.pMappedData;

            vk::DescriptorSetAllocateInfo setAlloc{};
            setAlloc.descriptorPool = renderer.descriptors.descriptorPool;
            setAlloc.setSetLayouts(renderer.descriptors.lightSetLayout);
            auto allocated = checked(renderer.context.device.allocateDescriptorSets(setAlloc), "allocate lightSet");
            if (!allocated)
            {
                return Err(allocated.error());
            }
            renderer.lighting.lightSets[i] = (*allocated)[0];

            vk::DescriptorBufferInfo lightBufferInfo{ vk::Buffer{ renderer.lighting.lightBuffers[i] }, 0, sizeof(LightUbo) };
            vk::WriteDescriptorSet lightWrite{};
            lightWrite.dstSet = renderer.lighting.lightSets[i];
            lightWrite.dstBinding = 0;
            lightWrite.descriptorType = vk::DescriptorType::eUniformBuffer;
            lightWrite.setBufferInfo(lightBufferInfo);
            renderer.context.device.updateDescriptorSets(lightWrite, {});

            // Bind the directional (binding 4) + spot (binding 5) shadow maps with the
            // compare sampler. The graph guarantees ShaderReadOnly when the scene samples.
            vk::DescriptorImageInfo shadowInfo{ renderer.descriptors.shadowSampler,
                renderer.targets.shadowMap.view, vk::ImageLayout::eShaderReadOnlyOptimal };
            vk::DescriptorImageInfo spotShadowInfo{ renderer.descriptors.shadowSampler,
                renderer.targets.spotShadowMap.view, vk::ImageLayout::eShaderReadOnlyOptimal };
            // The point distance cube samples with the plain linear sampler (no compare).
            vk::DescriptorImageInfo pointShadowInfo{ renderer.descriptors.linearSampler,
                renderer.targets.pointShadowCube.view, vk::ImageLayout::eShaderReadOnlyOptimal };
            std::array<vk::WriteDescriptorSet, 3> shadowWrites{};
            shadowWrites[0].dstSet = renderer.lighting.lightSets[i];
            shadowWrites[0].dstBinding = 4;
            shadowWrites[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            shadowWrites[0].setImageInfo(shadowInfo);
            shadowWrites[1].dstSet = renderer.lighting.lightSets[i];
            shadowWrites[1].dstBinding = 5;
            shadowWrites[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            shadowWrites[1].setImageInfo(spotShadowInfo);
            shadowWrites[2].dstSet = renderer.lighting.lightSets[i];
            shadowWrites[2].dstBinding = 6;
            shadowWrites[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            shadowWrites[2].setImageInfo(pointShadowInfo);
            renderer.context.device.updateDescriptorSets(shadowWrites, {});

            // The punctual-light buffer starts at LightListInitial capacity and is
            // grown by ensureLightCapacity; bind it now so the set is complete.
            Result<Ref<Buffer>> lightList =
                makeMappedStorageBuffer(renderer, static_cast<vk::DeviceSize>(LightListInitial) * sizeof(GpuLight));
            if (!lightList)
            {
                return Err(lightList.error());
            }
            renderer.lighting.lightListBuffers[i] = *lightList;
            renderer.lighting.lightListCapacity[i] = LightListInitial;
            vk::DescriptorBufferInfo lightListInfo{ (*lightList)->buffer, 0, (*lightList)->size };
            vk::WriteDescriptorSet lightListWrite{};
            lightListWrite.dstSet = renderer.lighting.lightSets[i];
            lightListWrite.dstBinding = 1;
            lightListWrite.descriptorType = vk::DescriptorType::eStorageBuffer;
            lightListWrite.setBufferInfo(lightListInfo);
            renderer.context.device.updateDescriptorSets(lightListWrite, {});

            // The instance buffer is created lazily (ensureInstanceCapacity), which
            // also writes this set; allocate the set up front so it is stable.
            vk::DescriptorSetAllocateInfo instanceAlloc{};
            instanceAlloc.descriptorPool = renderer.descriptors.descriptorPool;
            instanceAlloc.setSetLayouts(renderer.descriptors.instanceSetLayout);
            auto instanceAllocated = checked(renderer.context.device.allocateDescriptorSets(instanceAlloc), "allocate instanceSet");
            if (!instanceAllocated)
            {
                return Err(instanceAllocated.error());
            }
            renderer.instancing.sets[i] = (*instanceAllocated)[0];

            // Cluster light-list buffer (device-local, compute-written) + the
            // cluster params UBO (host-mapped, written each frame).
            Result<Ref<Buffer>> clusterBuffer =
                makeDeviceStorageBuffer(renderer, ClusterCount * ClusterStride);
            if (!clusterBuffer)
            {
                return Err(clusterBuffer.error());
            }
            renderer.lighting.clusterBuffers[i] = *clusterBuffer;

            VkBufferCreateInfo paramInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            paramInfo.size = sizeof(ClusterParams);
            paramInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo paramAlloc{};
            paramAlloc.usage = VMA_MEMORY_USAGE_AUTO;
            paramAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo paramMapped{};
            if (vmaCreateBuffer(renderer.context.allocator, &paramInfo, &paramAlloc, &renderer.lighting.clusterParamBuffers[i],
                                &renderer.lighting.clusterParamAllocs[i], &paramMapped) != VK_SUCCESS)
            {
                return Err(std::string{ "cluster params vmaCreateBuffer failed" });
            }
            renderer.lighting.clusterParamMapped[i] = paramMapped.pMappedData;

            // Lighting set bindings 2 (cluster lists) + 3 (cluster params).
            vk::DescriptorBufferInfo clusterInfo{ (*clusterBuffer)->buffer, 0, (*clusterBuffer)->size };
            vk::DescriptorBufferInfo paramBufferInfo{ vk::Buffer{ renderer.lighting.clusterParamBuffers[i] }, 0, sizeof(ClusterParams) };
            std::array<vk::WriteDescriptorSet, 2> lightClusterWrites{};
            lightClusterWrites[0].dstSet = renderer.lighting.lightSets[i];
            lightClusterWrites[0].dstBinding = 2;
            lightClusterWrites[0].descriptorType = vk::DescriptorType::eStorageBuffer;
            lightClusterWrites[0].setBufferInfo(clusterInfo);
            lightClusterWrites[1].dstSet = renderer.lighting.lightSets[i];
            lightClusterWrites[1].dstBinding = 3;
            lightClusterWrites[1].descriptorType = vk::DescriptorType::eUniformBuffer;
            lightClusterWrites[1].setBufferInfo(paramBufferInfo);
            renderer.context.device.updateDescriptorSets(lightClusterWrites, {});

            // Compute cluster set: params UBO + light list (read) + cluster lists (write).
            vk::DescriptorSetAllocateInfo clusterAlloc{};
            clusterAlloc.descriptorPool = renderer.descriptors.descriptorPool;
            clusterAlloc.setSetLayouts(renderer.descriptors.clusterSetLayout);
            auto clusterAllocated = checked(renderer.context.device.allocateDescriptorSets(clusterAlloc), "allocate clusterSet");
            if (!clusterAllocated)
            {
                return Err(clusterAllocated.error());
            }
            renderer.lighting.clusterSets[i] = (*clusterAllocated)[0];
            std::array<vk::WriteDescriptorSet, 3> clusterWrites{};
            clusterWrites[0].dstSet = renderer.lighting.clusterSets[i];
            clusterWrites[0].dstBinding = 0;
            clusterWrites[0].descriptorType = vk::DescriptorType::eUniformBuffer;
            clusterWrites[0].setBufferInfo(paramBufferInfo);
            clusterWrites[1].dstSet = renderer.lighting.clusterSets[i];
            clusterWrites[1].dstBinding = 1;
            clusterWrites[1].descriptorType = vk::DescriptorType::eStorageBuffer;
            clusterWrites[1].setBufferInfo(lightListInfo);
            clusterWrites[2].dstSet = renderer.lighting.clusterSets[i];
            clusterWrites[2].dstBinding = 2;
            clusterWrites[2].descriptorType = vk::DescriptorType::eStorageBuffer;
            clusterWrites[2].setBufferInfo(clusterInfo);
            renderer.context.device.updateDescriptorSets(clusterWrites, {});
        }

        // The cull compute pipeline reads/writes the cluster set layout.
        Result<Ref<Pipeline>> cull =
            newComputePipeline(renderer, "shaders/light_cull.spv", renderer.descriptors.clusterSetLayout);
        if (!cull)
        {
            return Err(cull.error());
        }
        renderer.pipelines.cull = *cull;

        // Tonemap: a compute pipeline + a set binding the offscreen color as a storage
        // image (read+written in place). beginFrameGraph adds the pass every frame; the
        // exposure multiplier is a compute-stage push constant.
        Result<Ref<Pipeline>> tonemap =
            newComputePipeline(renderer, "shaders/tonemap.spv", renderer.descriptors.tonemapSetLayout,
                               static_cast<u32>(sizeof(f32)));
        if (!tonemap)
        {
            return Err(tonemap.error());
        }
        renderer.pipelines.tonemap = *tonemap;

        vk::DescriptorSetAllocateInfo tonemapAlloc{};
        tonemapAlloc.descriptorPool = renderer.descriptors.descriptorPool;
        tonemapAlloc.setSetLayouts(renderer.descriptors.tonemapSetLayout);
        auto tonemapAllocated = checked(renderer.context.device.allocateDescriptorSets(tonemapAlloc), "allocate tonemapSet");
        if (!tonemapAllocated)
        {
            return Err(tonemapAllocated.error());
        }
        renderer.descriptors.tonemapSet = (*tonemapAllocated)[0];
        updateTonemapSet(renderer);

        // FXAA: a compute pipeline + a set (source sampler + offscreen storage). The
        // scratch source is created on demand when FXAA is enabled (recreateFxaaTarget).
        Result<Ref<Pipeline>> fxaa =
            newComputePipeline(renderer, "shaders/fxaa.spv", renderer.descriptors.fxaaSetLayout);
        if (!fxaa)
        {
            return Err(fxaa.error());
        }
        renderer.pipelines.fxaa = *fxaa;
        vk::DescriptorSetAllocateInfo fxaaAlloc{};
        fxaaAlloc.descriptorPool = renderer.descriptors.descriptorPool;
        fxaaAlloc.setSetLayouts(renderer.descriptors.fxaaSetLayout);
        auto fxaaAllocated = checked(renderer.context.device.allocateDescriptorSets(fxaaAlloc), "allocate fxaaSet");
        if (!fxaaAllocated)
        {
            return Err(fxaaAllocated.error());
        }
        renderer.descriptors.fxaaSet = (*fxaaAllocated)[0];

        // Depth pre-pass: a vertex-only pipeline that reuses the mesh vertex shader.
        Result<Ref<Pipeline>> depthPrepass =
            makeDepthPrepassPipeline(renderer, "shaders/mesh.spv");
        if (!depthPrepass)
        {
            return Err(depthPrepass.error());
        }
        renderer.pipelines.depthPrepass = *depthPrepass;

        // Directional shadow depth pass: depth-only + depth-biased, reuses the mesh vertex shader.
        Result<Ref<Pipeline>> shadowDepth = makeShadowPipeline(renderer, "shaders/mesh.spv");
        if (!shadowDepth)
        {
            return Err(shadowDepth.error());
        }
        renderer.pipelines.shadowDepth = *shadowDepth;

        // Point shadow pass: color (distance) + depth, instanced.
        Result<Ref<Pipeline>> pointShadow = makePointShadowPipeline(renderer, "shaders/point_shadow.spv");
        if (!pointShadow)
        {
            return Err(pointShadow.error());
        }
        renderer.pipelines.pointShadow = *pointShadow;

        // IBL set (set 3 in the mesh pipeline): irradiance cube + prefiltered cube + BRDF
        // LUT, all sampled in the fragment. Created here so the mesh PSO layout + the bind
        // can reference it; bakeEnvironment fills the images + writes the descriptor.
        vk::SamplerCreateInfo iblSamplerInfo{};
        iblSamplerInfo.magFilter = vk::Filter::eLinear;
        iblSamplerInfo.minFilter = vk::Filter::eLinear;
        iblSamplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
        iblSamplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        iblSamplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        iblSamplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        iblSamplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        auto iblSampler = checked(renderer.context.device.createSampler(iblSamplerInfo), "createSampler (ibl)");
        if (!iblSampler)
        {
            return Err(iblSampler.error());
        }
        renderer.ibl.sampler = *iblSampler;

        std::array<vk::DescriptorSetLayoutBinding, 3> iblBindings{};
        for (u32 b = 0; b < 3; b = b + 1)
        {
            iblBindings[b].binding = b;
            iblBindings[b].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            iblBindings[b].descriptorCount = 1;
            iblBindings[b].stageFlags = vk::ShaderStageFlagBits::eFragment;
        }
        vk::DescriptorSetLayoutCreateInfo iblLayoutInfo{};
        iblLayoutInfo.setBindings(iblBindings);
        auto iblLayout = checked(renderer.context.device.createDescriptorSetLayout(iblLayoutInfo), "iblSetLayout");
        if (!iblLayout)
        {
            return Err(iblLayout.error());
        }
        renderer.ibl.setLayout = *iblLayout;

        vk::DescriptorSetAllocateInfo iblAlloc{};
        iblAlloc.descriptorPool = renderer.descriptors.descriptorPool;
        iblAlloc.setSetLayouts(renderer.ibl.setLayout);
        auto iblSet = checked(renderer.context.device.allocateDescriptorSets(iblAlloc), "allocate iblSet");
        if (!iblSet)
        {
            return Err(iblSet.error());
        }
        renderer.ibl.set = (*iblSet)[0];

        // SSAO: a nearest sampler, the GTAO compute set (g-buffer sampler + AO storage), the
        // mesh-AO set (set 4 = AO map sampler), both pipelines, and the viewport-sized targets.
        vk::SamplerCreateInfo ssaoSamplerInfo{};
        ssaoSamplerInfo.magFilter = vk::Filter::eNearest;
        ssaoSamplerInfo.minFilter = vk::Filter::eNearest;
        ssaoSamplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
        ssaoSamplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        ssaoSamplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        ssaoSamplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        auto ssaoSampler = checked(renderer.context.device.createSampler(ssaoSamplerInfo), "createSampler (ssao)");
        if (!ssaoSampler)
        {
            return Err(ssaoSampler.error());
        }
        renderer.ssao.sampler = *ssaoSampler;

        std::array<vk::DescriptorSetLayoutBinding, 2> gtaoBindings{};
        gtaoBindings[0].binding = 0;  // g-buffer sampler
        gtaoBindings[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        gtaoBindings[0].descriptorCount = 1;
        gtaoBindings[0].stageFlags = vk::ShaderStageFlagBits::eCompute;
        gtaoBindings[1].binding = 1;  // AO storage image
        gtaoBindings[1].descriptorType = vk::DescriptorType::eStorageImage;
        gtaoBindings[1].descriptorCount = 1;
        gtaoBindings[1].stageFlags = vk::ShaderStageFlagBits::eCompute;
        vk::DescriptorSetLayoutCreateInfo gtaoLayoutInfo{};
        gtaoLayoutInfo.setBindings(gtaoBindings);
        auto gtaoLayout = checked(renderer.context.device.createDescriptorSetLayout(gtaoLayoutInfo), "gtaoSetLayout");
        if (!gtaoLayout)
        {
            return Err(gtaoLayout.error());
        }
        renderer.ssao.gtaoSetLayout = *gtaoLayout;

        vk::DescriptorSetLayoutBinding aoBinding{};
        aoBinding.binding = 0;  // AO map sampler (mesh fragment set 4)
        aoBinding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        aoBinding.descriptorCount = 1;
        aoBinding.stageFlags = vk::ShaderStageFlagBits::eFragment;
        vk::DescriptorSetLayoutCreateInfo aoLayoutInfo{};
        aoLayoutInfo.setBindings(aoBinding);
        auto aoLayout = checked(renderer.context.device.createDescriptorSetLayout(aoLayoutInfo), "aoSetLayout");
        if (!aoLayout)
        {
            return Err(aoLayout.error());
        }
        renderer.ssao.aoSetLayout = *aoLayout;

        vk::DescriptorSetAllocateInfo gtaoAlloc{};
        gtaoAlloc.descriptorPool = renderer.descriptors.descriptorPool;
        gtaoAlloc.setSetLayouts(renderer.ssao.gtaoSetLayout);
        auto gtaoSet = checked(renderer.context.device.allocateDescriptorSets(gtaoAlloc), "allocate gtaoSet");
        if (!gtaoSet)
        {
            return Err(gtaoSet.error());
        }
        renderer.ssao.gtaoSet = (*gtaoSet)[0];

        vk::DescriptorSetAllocateInfo aoAlloc{};
        aoAlloc.descriptorPool = renderer.descriptors.descriptorPool;
        aoAlloc.setSetLayouts(renderer.ssao.aoSetLayout);
        auto aoSet = checked(renderer.context.device.allocateDescriptorSets(aoAlloc), "allocate aoSet");
        if (!aoSet)
        {
            return Err(aoSet.error());
        }
        renderer.ssao.aoSet = (*aoSet)[0];

        Result<Ref<Pipeline>> gbufferPipe = makeGbufferPipeline(renderer, "shaders/gbuffer.spv");
        if (!gbufferPipe)
        {
            return Err(gbufferPipe.error());
        }
        renderer.pipelines.gbuffer = *gbufferPipe;

        Result<Ref<Pipeline>> gtaoPipe =
            newComputePipeline(renderer, "shaders/gtao.spv", renderer.ssao.gtaoSetLayout,
                               static_cast<u32>(sizeof(glm::mat4) + sizeof(glm::vec4)));
        if (!gtaoPipe)
        {
            return Err(gtaoPipe.error());
        }
        renderer.pipelines.gtao = *gtaoPipe;

        recreateSsaoTargets(renderer);
        return {};
    }

    // Bakes the IBL environment: generates the procedural sky cube, convolves it into a
    // diffuse irradiance cube + a roughness-mipped prefiltered specular cube, integrates
    // the split-sum BRDF LUT, and writes the persistent set 3. Synchronous one-time work
    // (own command buffer + waitIdle), like uploadTexture/renderMeshThumbnail. Run once at
    // startup after initDescriptorResources.
    auto bakeEnvironment(Renderer& renderer) -> Result<void>
    {
        const u32 preMips = IblPrefilterMips;
        vk::Device device = renderer.context.device;

        auto env = newCubeImage(renderer, IblEnvSize, 1, IblColorFormat);
        if (!env) { return Err(env.error()); }
        renderer.ibl.envCube = std::move(*env);
        auto irr = newCubeImage(renderer, IblIrradianceSize, 1, IblColorFormat);
        if (!irr) { return Err(irr.error()); }
        renderer.ibl.irradianceCube = std::move(*irr);
        auto pre = newCubeImage(renderer, IblPrefilterSize, preMips, IblColorFormat);
        if (!pre) { return Err(pre.error()); }
        renderer.ibl.prefilteredCube = std::move(*pre);
        auto lut = newColorImage(renderer, IblLutSize, IblLutSize, IblColorFormat, true);
        if (!lut) { return Err(lut.error()); }
        renderer.ibl.brdfLut = std::move(*lut);
        renderer.ibl.prefilterMips = preMips;

        // A transient pool + layouts + sets used only for this bake (freed at the end).
        std::array<vk::DescriptorPoolSize, 2> poolSizes{
            vk::DescriptorPoolSize{ vk::DescriptorType::eStorageImage, 16 },
            vk::DescriptorPoolSize{ vk::DescriptorType::eCombinedImageSampler, 16 } };
        vk::DescriptorPoolCreateInfo poolInfo{};
        poolInfo.maxSets = 32;
        poolInfo.setPoolSizes(poolSizes);
        auto poolR = checked(device.createDescriptorPool(poolInfo), "ibl bake pool");
        if (!poolR) { return Err(poolR.error()); }
        vk::DescriptorPool pool = *poolR;

        vk::DescriptorSetLayoutBinding bindA{};
        bindA.binding = 0;
        bindA.descriptorType = vk::DescriptorType::eStorageImage;
        bindA.descriptorCount = 1;
        bindA.stageFlags = vk::ShaderStageFlagBits::eCompute;
        vk::DescriptorSetLayoutCreateInfo layoutAInfo{};
        layoutAInfo.setBindings(bindA);
        auto layoutAR = checked(device.createDescriptorSetLayout(layoutAInfo), "ibl layoutA");
        if (!layoutAR) { device.destroyDescriptorPool(pool); return Err(layoutAR.error()); }
        vk::DescriptorSetLayout layoutA = *layoutAR;

        std::array<vk::DescriptorSetLayoutBinding, 2> bindB{};
        bindB[0].binding = 0;
        bindB[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        bindB[0].descriptorCount = 1;
        bindB[0].stageFlags = vk::ShaderStageFlagBits::eCompute;
        bindB[1].binding = 1;
        bindB[1].descriptorType = vk::DescriptorType::eStorageImage;
        bindB[1].descriptorCount = 1;
        bindB[1].stageFlags = vk::ShaderStageFlagBits::eCompute;
        vk::DescriptorSetLayoutCreateInfo layoutBInfo{};
        layoutBInfo.setBindings(bindB);
        auto layoutBR = checked(device.createDescriptorSetLayout(layoutBInfo), "ibl layoutB");
        if (!layoutBR)
        {
            device.destroyDescriptorSetLayout(layoutA);
            device.destroyDescriptorPool(pool);
            return Err(layoutBR.error());
        }
        vk::DescriptorSetLayout layoutB = *layoutBR;

        auto cleanupLayouts = [&]()
        {
            device.destroyDescriptorSetLayout(layoutA);
            device.destroyDescriptorSetLayout(layoutB);
            device.destroyDescriptorPool(pool);
        };

        auto skygenP = newComputePipeline(renderer, "shaders/ibl_skygen.spv", layoutA);
        if (!skygenP) { cleanupLayouts(); return Err(skygenP.error()); }
        auto irrP = newComputePipeline(renderer, "shaders/ibl_irradiance.spv", layoutB);
        if (!irrP) { cleanupLayouts(); return Err(irrP.error()); }
        auto preP = newComputePipeline(renderer, "shaders/ibl_prefilter.spv", layoutB, static_cast<u32>(sizeof(f32)));
        if (!preP) { cleanupLayouts(); return Err(preP.error()); }
        auto lutP = newComputePipeline(renderer, "shaders/ibl_brdf.spv", layoutA);
        if (!lutP) { cleanupLayouts(); return Err(lutP.error()); }

        // Transient 2D-array storage views (one per cube mip we write) + the per-set allocs.
        std::vector<vk::ImageView> transientViews;
        auto makeStorageView = [&](vk::Image image, u32 mip) -> vk::ImageView
        {
            vk::ImageViewCreateInfo v{};
            v.image = image;
            v.viewType = vk::ImageViewType::e2DArray;
            v.format = IblColorFormat;
            v.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, mip, 1, 0, 6 };
            vk::ImageView view = device.createImageView(v).value;
            transientViews.push_back(view);
            return view;
        };
        vk::ImageView envStore = makeStorageView(renderer.ibl.envCube.image, 0);
        vk::ImageView irrStore = makeStorageView(renderer.ibl.irradianceCube.image, 0);
        std::vector<vk::ImageView> preStore;
        for (u32 m = 0; m < preMips; m = m + 1)
        {
            preStore.push_back(makeStorageView(renderer.ibl.prefilteredCube.image, m));
        }

        auto allocSet = [&](vk::DescriptorSetLayout layout) -> vk::DescriptorSet
        {
            vk::DescriptorSetAllocateInfo ai{};
            ai.descriptorPool = pool;
            ai.setSetLayouts(layout);
            return device.allocateDescriptorSets(ai).value[0];
        };
        vk::DescriptorSet skygenSet = allocSet(layoutA);
        vk::DescriptorSet brdfSet = allocSet(layoutA);
        vk::DescriptorSet irrSet = allocSet(layoutB);
        std::vector<vk::DescriptorSet> preSets;
        for (u32 m = 0; m < preMips; m = m + 1)
        {
            preSets.push_back(allocSet(layoutB));
        }

        auto writeStorage = [&](vk::DescriptorSet set, u32 binding, vk::ImageView view)
        {
            vk::DescriptorImageInfo ii{};
            ii.imageView = view;
            ii.imageLayout = vk::ImageLayout::eGeneral;
            vk::WriteDescriptorSet w{};
            w.dstSet = set;
            w.dstBinding = binding;
            w.descriptorType = vk::DescriptorType::eStorageImage;
            w.setImageInfo(ii);
            device.updateDescriptorSets(w, {});
        };
        auto writeSampler = [&](vk::DescriptorSet set, u32 binding, vk::ImageView view)
        {
            vk::DescriptorImageInfo ii{ renderer.ibl.sampler, view, vk::ImageLayout::eShaderReadOnlyOptimal };
            vk::WriteDescriptorSet w{};
            w.dstSet = set;
            w.dstBinding = binding;
            w.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            w.setImageInfo(ii);
            device.updateDescriptorSets(w, {});
        };
        writeStorage(skygenSet, 0, envStore);
        writeStorage(brdfSet, 0, renderer.ibl.brdfLut.view);
        writeSampler(irrSet, 0, renderer.ibl.envCube.view);
        writeStorage(irrSet, 1, irrStore);
        for (u32 m = 0; m < preMips; m = m + 1)
        {
            writeSampler(preSets[m], 0, renderer.ibl.envCube.view);
            writeStorage(preSets[m], 1, preStore[m]);
        }

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = renderer.frame.frames[0].commandPool;
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(device.allocateCommandBuffers(cmdAlloc), "ibl bake cmd");
        if (!cmds)
        {
            for (vk::ImageView v : transientViews) { device.destroyImageView(v); }
            cleanupLayouts();
            return Err(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];
        vk::CommandBufferBeginInfo begin{};
        begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(begin));

        auto barrier = [&](vk::Image image, vk::ImageLayout oldL, vk::ImageLayout newL,
                           vk::PipelineStageFlags2 srcS, vk::AccessFlags2 srcA,
                           vk::PipelineStageFlags2 dstS, vk::AccessFlags2 dstA,
                           u32 baseMip, u32 mipCount, u32 layerCount)
        {
            vk::ImageMemoryBarrier2 b{};
            b.srcStageMask = srcS;
            b.srcAccessMask = srcA;
            b.dstStageMask = dstS;
            b.dstAccessMask = dstA;
            b.oldLayout = oldL;
            b.newLayout = newL;
            b.image = image;
            b.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, baseMip, mipCount, 0, layerCount };
            vk::DependencyInfo d{};
            d.setImageMemoryBarriers(b);
            cmd.pipelineBarrier2(d);
        };
        const auto group = [](u32 n) -> u32 { return (n + 7) / 8; };

        // Environment sky -> general, dispatch skygen, -> shader-read for the convolutions.
        barrier(renderer.ibl.envCube.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite, 0, 1, 6);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, skygenP.value()->pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, skygenP.value()->layout, 0, skygenSet, {});
        cmd.dispatch(group(IblEnvSize), group(IblEnvSize), 6);
        barrier(renderer.ibl.envCube.image, vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderSampledRead, 0, 1, 6);

        // Diffuse irradiance.
        barrier(renderer.ibl.irradianceCube.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite, 0, 1, 6);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, irrP.value()->pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, irrP.value()->layout, 0, irrSet, {});
        cmd.dispatch(group(IblIrradianceSize), group(IblIrradianceSize), 6);
        barrier(renderer.ibl.irradianceCube.image, vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead, 0, 1, 6);

        // Prefiltered specular: one dispatch per mip (roughness = mip / (mips-1)).
        barrier(renderer.ibl.prefilteredCube.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite, 0, preMips, 6);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, preP.value()->pipeline);
        for (u32 m = 0; m < preMips; m = m + 1)
        {
            u32 mipSize = IblPrefilterSize >> m;
            if (mipSize == 0) { mipSize = 1; }
            f32 roughness = preMips > 1 ? static_cast<f32>(m) / static_cast<f32>(preMips - 1) : 0.0f;
            cmd.pushConstants(preP.value()->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(f32), &roughness);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, preP.value()->layout, 0, preSets[m], {});
            cmd.dispatch(group(mipSize), group(mipSize), 6);
        }
        barrier(renderer.ibl.prefilteredCube.image, vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead, 0, preMips, 6);

        // Split-sum BRDF LUT (2D, single layer).
        barrier(renderer.ibl.brdfLut.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite, 0, 1, 1);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, lutP.value()->pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, lutP.value()->layout, 0, brdfSet, {});
        cmd.dispatch(group(IblLutSize), group(IblLutSize), 1);
        barrier(renderer.ibl.brdfLut.image, vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead, 0, 1, 1);

        static_cast<void>(cmd.end());
        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = cmd;
        vk::SubmitInfo2 submitInfo{};
        submitInfo.setCommandBufferInfos(cmdInfo);
        static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, nullptr));
        static_cast<void>(device.waitIdle());
        device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);

        renderer.ibl.envCube.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
        renderer.ibl.irradianceCube.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
        renderer.ibl.prefilteredCube.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
        renderer.ibl.brdfLut.layout = vk::ImageLayout::eShaderReadOnlyOptimal;

        // Write the persistent set 3 the mesh fragment samples.
        std::array<vk::DescriptorImageInfo, 3> setImages{
            vk::DescriptorImageInfo{ renderer.ibl.sampler, renderer.ibl.irradianceCube.view, vk::ImageLayout::eShaderReadOnlyOptimal },
            vk::DescriptorImageInfo{ renderer.ibl.sampler, renderer.ibl.prefilteredCube.view, vk::ImageLayout::eShaderReadOnlyOptimal },
            vk::DescriptorImageInfo{ renderer.ibl.sampler, renderer.ibl.brdfLut.view, vk::ImageLayout::eShaderReadOnlyOptimal } };
        std::array<vk::WriteDescriptorSet, 3> setWrites{};
        for (u32 b = 0; b < 3; b = b + 1)
        {
            setWrites[b].dstSet = renderer.ibl.set;
            setWrites[b].dstBinding = b;
            setWrites[b].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            setWrites[b].setImageInfo(setImages[b]);
        }
        device.updateDescriptorSets(setWrites, {});
        renderer.ibl.ready = true;

        for (vk::ImageView v : transientViews) { device.destroyImageView(v); }
        cleanupLayouts();
        logInfo(std::format("ibl baked — env {}^2, irradiance {}^2, prefiltered {}^2 x{} mips, lut {}^2",
                            IblEnvSize, IblIrradianceSize, IblPrefilterSize, preMips, IblLutSize));
        return {};
    }
}
