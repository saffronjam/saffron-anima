module;

#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_NO_SMART_HANDLE
#include <vulkan/vulkan.hpp>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include <stb_image_write.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

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
        for (vk::ImageView view : renderer.swapchainImageViews)
        {
            renderer.device.destroyImageView(view);
        }
        renderer.swapchainImageViews.clear();

        for (vk::Semaphore semaphore : renderer.renderFinished)
        {
            renderer.device.destroySemaphore(semaphore);
        }
        renderer.renderFinished.clear();

        if (renderer.swapchain)
        {
            renderer.device.destroySwapchainKHR(renderer.swapchain);
            renderer.swapchain = nullptr;
        }
    }

    auto buildSwapchain(Renderer& renderer, u32 width, u32 height) -> Result<void>
    {
        // TRANSFER_SRC on swapchain images is not spec-guaranteed (only
        // COLOR_ATTACHMENT is). Query support and only request it when present
        // so an exotic surface disables window screenshots rather than failing
        // the whole swapchain build.
        renderer.swapchainCaptureSupported = false;
        vk::ResultValue<vk::SurfaceCapabilitiesKHR> caps =
            renderer.physicalDevice.getSurfaceCapabilitiesKHR(renderer.surface);
        if (caps.result == vk::Result::eSuccess &&
            (caps.value.supportedUsageFlags & vk::ImageUsageFlagBits::eTransferSrc))
        {
            renderer.swapchainCaptureSupported = true;
        }

        VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (renderer.swapchainCaptureSupported)
        {
            usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }

        vkb::SwapchainBuilder builder{ renderer.vkbDevice };
        builder.set_desired_format(VkSurfaceFormatKHR{
                   .format = VK_FORMAT_B8G8R8A8_UNORM,
                   .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
            .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
            .set_desired_extent(width, height)
            .add_image_usage_flags(usage);

        if (renderer.swapchain)
        {
            builder.set_old_swapchain(static_cast<VkSwapchainKHR>(renderer.swapchain));
        }

        auto result = builder.build();
        if (!result)
        {
            return Err(std::format("swapchain build failed: {}", result.error().message()));
        }

        destroySwapchainResources(renderer);

        vkb::Swapchain swapchain = result.value();
        renderer.swapchain = vk::SwapchainKHR{ swapchain.swapchain };
        renderer.swapchainFormat = vk::Format{ static_cast<vk::Format>(swapchain.image_format) };
        renderer.swapchainExtent = vk::Extent2D{ swapchain.extent.width, swapchain.extent.height };

        renderer.swapchainImages.clear();
        for (VkImage image : swapchain.get_images().value())
        {
            renderer.swapchainImages.push_back(vk::Image{ image });
        }

        renderer.swapchainImageViews.clear();
        for (vk::Image image : renderer.swapchainImages)
        {
            vk::ImageViewCreateInfo viewInfo{};
            viewInfo.image = image;
            viewInfo.viewType = vk::ImageViewType::e2D;
            viewInfo.format = renderer.swapchainFormat;
            viewInfo.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
            auto view = checked(renderer.device.createImageView(viewInfo), "createImageView");
            if (!view)
            {
                return Err(view.error());
            }
            renderer.swapchainImageViews.push_back(*view);
        }

        renderer.renderFinished.clear();
        for (std::size_t i = 0; i < renderer.swapchainImages.size(); i = i + 1)
        {
            auto semaphore = checked(renderer.device.createSemaphore(vk::SemaphoreCreateInfo{}), "createSemaphore");
            if (!semaphore)
            {
                return Err(semaphore.error());
            }
            renderer.renderFinished.push_back(*semaphore);
        }

        renderer.imagesInFlight.assign(renderer.swapchainImages.size(), vk::Fence{});
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
        static_cast<void>(renderer.device.waitIdle());
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
        vk::FormatProperties props = renderer.physicalDevice.getFormatProperties(format);
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
        if (vmaCreateImage(renderer.allocator, &imageInfo, &allocInfo, &rawImage, &allocation, nullptr) != VK_SUCCESS)
        {
            return Err(std::string{ "vmaCreateImage failed for offscreen target" });
        }

        vk::ImageViewCreateInfo viewInfo{};
        viewInfo.image = vk::Image{ rawImage };
        viewInfo.viewType = vk::ImageViewType::e2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
        vk::ResultValue<vk::ImageView> view = renderer.device.createImageView(viewInfo);
        if (view.result != vk::Result::eSuccess)
        {
            vmaDestroyImage(renderer.allocator, rawImage, allocation);
            return Err(std::format("createImageView (offscreen): {}", vk::to_string(view.result)));
        }

        Image result;
        result.device = renderer.device;
        result.allocator = renderer.allocator;
        result.image = vk::Image{ rawImage };
        result.view = view.value;
        result.alloc = allocation;
        result.extent = vk::Extent2D{ width, height };
        result.format = format;
        result.layout = vk::ImageLayout::eUndefined;
        return result;
    }

    auto newDepthImage(Renderer& renderer, u32 width, u32 height,
                                                    vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1) -> Result<Image>
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
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

        VkImage rawImage = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        if (vmaCreateImage(renderer.allocator, &imageInfo, &allocInfo, &rawImage, &allocation, nullptr) != VK_SUCCESS)
        {
            return Err(std::string{ "vmaCreateImage failed for depth target" });
        }

        vk::ImageViewCreateInfo viewInfo{};
        viewInfo.image = vk::Image{ rawImage };
        viewInfo.viewType = vk::ImageViewType::e2D;
        viewInfo.format = DepthFormat;
        viewInfo.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1 };
        vk::ResultValue<vk::ImageView> view = renderer.device.createImageView(viewInfo);
        if (view.result != vk::Result::eSuccess)
        {
            vmaDestroyImage(renderer.allocator, rawImage, allocation);
            return Err(std::format("createImageView (depth): {}", vk::to_string(view.result)));
        }

        Image result;
        result.device = renderer.device;
        result.allocator = renderer.allocator;
        result.image = vk::Image{ rawImage };
        result.view = view.value;
        result.alloc = allocation;
        result.extent = vk::Extent2D{ width, height };
        result.format = DepthFormat;
        result.layout = vk::ImageLayout::eUndefined;
        return result;
    }

    // (Re)create the multisampled scene color + depth targets at the offscreen extent
    // when MSAA is on; drop them when off. Called after the offscreen is sized + on a
    // sample-count change. The caller has already idled the GPU.
    void recreateMsaaTargets(Renderer& renderer)
    {
        renderer.msaaColor.reset();
        renderer.msaaDepth.reset();
        if (renderer.sampleCount == vk::SampleCountFlagBits::e1)
        {
            return;
        }
        const u32 w = renderer.offscreenViewport.extent.width;
        const u32 h = renderer.offscreenViewport.extent.height;
        if (w == 0 || h == 0)
        {
            return;
        }
        Result<Image> color =
            newColorImage(renderer, w, h, OffscreenColorFormat, false, renderer.sampleCount);
        if (color)
        {
            renderer.msaaColor = std::move(*color);
        }
        else
        {
            logError(color.error());
        }
        auto depth = newDepthImage(renderer, w, h, renderer.sampleCount);
        if (depth)
        {
            renderer.msaaDepth = std::move(*depth);
        }
        else
        {
            logError(depth.error());
        }
    }

    void updateFxaaSet(Renderer& renderer);  // defined alongside updateTonemapSet below

    // (Re)create the 1x scratch target FXAA reads from (the scene renders here when
    // FXAA is on); drop it when off. Sized to the offscreen; the GPU is already idle.
    void recreateFxaaTarget(Renderer& renderer)
    {
        renderer.offscreenScratch.reset();
        if (!renderer.fxaaEnabled)
        {
            return;
        }
        const u32 w = renderer.offscreenViewport.extent.width;
        const u32 h = renderer.offscreenViewport.extent.height;
        if (w == 0 || h == 0)
        {
            return;
        }
        auto scratch = newColorImage(renderer, w, h, OffscreenColorFormat, false);
        if (scratch)
        {
            renderer.offscreenScratch = std::move(*scratch);
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
        if (vmaCreateBuffer(renderer.allocator, &bufferInfo, &allocInfo, &outBuffer, &outAlloc, &outInfo) != VK_SUCCESS)
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

    // Writes a PNG, reordering BGRA source pixels to RGB.
    auto writeBufferToPng(
        const unsigned char* pixels, u32 width, u32 height, vk::Format format, const std::string& path) -> Result<void>
    {
        const bool bgr = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
        std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * height * 3);
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
        glm::vec4 directionAmbient;  // xyz direction, w ambient
        glm::vec4 colorIntensity;    // rgb color, a intensity
        glm::uvec4 counts;           // x = punctual light count
    };

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
        if (vmaCreateBuffer(renderer.allocator, &info, &alloc, &raw, &allocation, &mapped) != VK_SUCCESS)
        {
            return Err(std::string{ "makeMappedStorageBuffer: vmaCreateBuffer failed" });
        }
        Buffer buffer;
        buffer.allocator = renderer.allocator;
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
        if (vmaCreateBuffer(renderer.allocator, &info, &alloc, &raw, &allocation, nullptr) != VK_SUCCESS)
        {
            return Err(std::string{ "makeDeviceStorageBuffer: vmaCreateBuffer failed" });
        }
        Buffer buffer;
        buffer.allocator = renderer.allocator;
        buffer.buffer = vk::Buffer{ raw };
        buffer.alloc = allocation;
        buffer.size = bytes;
        return std::make_shared<Buffer>(std::move(buffer));
    }

    // Builds a compute pipeline from a SPIR-V module (entry "computeMain") + a single
    // descriptor set layout. Returned as a Ref<Pipeline> (move-only RAII).
    auto newComputePipeline(
        Renderer& renderer, std::string_view shaderName, vk::DescriptorSetLayout setLayout) -> Result<Ref<Pipeline>>
    {
        auto moduleResult = loadShaderModule(renderer.device, assetPath(shaderName));
        if (!moduleResult)
        {
            return Err(moduleResult.error());
        }
        vk::ShaderModule shaderModule = *moduleResult;

        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setSetLayouts(setLayout);
        auto layoutResult = checked(renderer.device.createPipelineLayout(layoutInfo), "createPipelineLayout (compute)");
        if (!layoutResult)
        {
            renderer.device.destroyShaderModule(shaderModule);
            return Err(layoutResult.error());
        }

        vk::PipelineShaderStageCreateInfo stage{};
        stage.stage = vk::ShaderStageFlagBits::eCompute;
        stage.module = shaderModule;
        stage.pName = "computeMain";

        vk::ComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.stage = stage;
        pipelineInfo.layout = *layoutResult;
        vk::ResultValue<vk::Pipeline> created = renderer.device.createComputePipeline(nullptr, pipelineInfo);
        renderer.device.destroyShaderModule(shaderModule);
        if (created.result != vk::Result::eSuccess)
        {
            renderer.device.destroyPipelineLayout(*layoutResult);
            return Err(std::format("createComputePipeline: {}", vk::to_string(created.result)));
        }

        Pipeline pipeline;
        pipeline.device = renderer.device;
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
        auto moduleResult = loadShaderModule(renderer.device, assetPath(shaderName));
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
        multisample.rasterizationSamples = renderer.sampleCount;  // match the MSAA target
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
            renderer.bindlessSetLayout, renderer.lightSetLayout, renderer.instanceSetLayout };
        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setSetLayouts(setLayouts);
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult = checked(renderer.device.createPipelineLayout(layoutInfo), "createPipelineLayout (depth-prepass)");
        if (!layoutResult)
        {
            renderer.device.destroyShaderModule(shaderModule);
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

        vk::ResultValue<vk::Pipeline> created = renderer.device.createGraphicsPipeline(nullptr, pipelineInfo);
        renderer.device.destroyShaderModule(shaderModule);
        if (created.result != vk::Result::eSuccess)
        {
            renderer.device.destroyPipelineLayout(*layoutResult);
            return Err(std::format("createGraphicsPipeline (depth-prepass): {}", vk::to_string(created.result)));
        }

        Pipeline pipeline;
        pipeline.device = renderer.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;
        return std::make_shared<Pipeline>(std::move(pipeline));
    }

    // Write a texture into the bindless array at the given slot (set 0, binding 0).
    // update-after-bind: safe to write a live set between draws.
    void writeBindlessTexture(Renderer& renderer, vk::ImageView view, u32 index)
    {
        vk::DescriptorImageInfo info{ renderer.linearSampler, view, vk::ImageLayout::eShaderReadOnlyOptimal };
        vk::WriteDescriptorSet write{};
        write.dstSet = renderer.bindlessSet;
        write.dstBinding = 0;
        write.dstArrayElement = index;
        write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        write.setImageInfo(info);
        renderer.device.updateDescriptorSets(write, {});
    }

    // Point the tonemap set's storage-image binding at the current offscreen color
    // view (GENERAL layout). Called after the offscreen color is (re)created.
    void updateTonemapSet(Renderer& renderer)
    {
        if (!renderer.tonemapSet)
        {
            return;
        }
        vk::DescriptorImageInfo imageInfo{};
        imageInfo.imageView = renderer.offscreenViewport.view;
        imageInfo.imageLayout = vk::ImageLayout::eGeneral;
        vk::WriteDescriptorSet write{};
        write.dstSet = renderer.tonemapSet;
        write.dstBinding = 0;
        write.descriptorType = vk::DescriptorType::eStorageImage;
        write.setImageInfo(imageInfo);
        renderer.device.updateDescriptorSets(write, {});
    }

    // Point the FXAA set at the current scratch (sampled source) + offscreen (storage
    // target) views. Called after either is (re)created.
    void updateFxaaSet(Renderer& renderer)
    {
        if (!renderer.fxaaSet || !renderer.offscreenScratch.view)
        {
            return;
        }
        vk::DescriptorImageInfo sourceInfo{ renderer.linearSampler, renderer.offscreenScratch.view,
                                            vk::ImageLayout::eShaderReadOnlyOptimal };
        vk::DescriptorImageInfo targetInfo{};
        targetInfo.imageView = renderer.offscreenViewport.view;
        targetInfo.imageLayout = vk::ImageLayout::eGeneral;
        std::array<vk::WriteDescriptorSet, 2> writes{};
        writes[0].dstSet = renderer.fxaaSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[0].setImageInfo(sourceInfo);
        writes[1].dstSet = renderer.fxaaSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = vk::DescriptorType::eStorageImage;
        writes[1].setImageInfo(targetInfo);
        renderer.device.updateDescriptorSets(writes, {});
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
        auto sampler = checked(renderer.device.createSampler(samplerInfo), "createSampler");
        if (!sampler)
        {
            return Err(sampler.error());
        }
        renderer.linearSampler = *sampler;

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
        auto materialLayout = checked(renderer.device.createDescriptorSetLayout(materialLayoutInfo), "bindlessSetLayout");
        if (!materialLayout)
        {
            return Err(materialLayout.error());
        }
        renderer.bindlessSetLayout = *materialLayout;

        std::array<vk::DescriptorSetLayoutBinding, 4> lightBindings{};
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
        vk::DescriptorSetLayoutCreateInfo lightLayoutInfo{};
        lightLayoutInfo.setBindings(lightBindings);
        auto lightLayout = checked(renderer.device.createDescriptorSetLayout(lightLayoutInfo), "lightSetLayout");
        if (!lightLayout)
        {
            return Err(lightLayout.error());
        }
        renderer.lightSetLayout = *lightLayout;

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
        auto clusterLayout = checked(renderer.device.createDescriptorSetLayout(clusterLayoutInfo), "clusterSetLayout");
        if (!clusterLayout)
        {
            return Err(clusterLayout.error());
        }
        renderer.clusterSetLayout = *clusterLayout;

        vk::DescriptorSetLayoutBinding instanceBinding{};
        instanceBinding.binding = 0;
        instanceBinding.descriptorType = vk::DescriptorType::eStorageBuffer;
        instanceBinding.descriptorCount = 1;
        instanceBinding.stageFlags = vk::ShaderStageFlagBits::eVertex;
        vk::DescriptorSetLayoutCreateInfo instanceLayoutInfo{};
        instanceLayoutInfo.setBindings(instanceBinding);
        auto instanceLayout = checked(renderer.device.createDescriptorSetLayout(instanceLayoutInfo), "instanceSetLayout");
        if (!instanceLayout)
        {
            return Err(instanceLayout.error());
        }
        renderer.instanceSetLayout = *instanceLayout;

        vk::DescriptorSetLayoutBinding tonemapBinding{};
        tonemapBinding.binding = 0;  // the offscreen color as a storage image
        tonemapBinding.descriptorType = vk::DescriptorType::eStorageImage;
        tonemapBinding.descriptorCount = 1;
        tonemapBinding.stageFlags = vk::ShaderStageFlagBits::eCompute;
        vk::DescriptorSetLayoutCreateInfo tonemapLayoutInfo{};
        tonemapLayoutInfo.setBindings(tonemapBinding);
        auto tonemapLayout = checked(renderer.device.createDescriptorSetLayout(tonemapLayoutInfo), "tonemapSetLayout");
        if (!tonemapLayout)
        {
            return Err(tonemapLayout.error());
        }
        renderer.tonemapSetLayout = *tonemapLayout;

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
        auto fxaaLayout = checked(renderer.device.createDescriptorSetLayout(fxaaLayoutInfo), "fxaaSetLayout");
        if (!fxaaLayout)
        {
            return Err(fxaaLayout.error());
        }
        renderer.fxaaSetLayout = *fxaaLayout;

        std::array<vk::DescriptorPoolSize, 4> poolSizes{
            vk::DescriptorPoolSize{ vk::DescriptorType::eCombinedImageSampler, 1024 },
            vk::DescriptorPoolSize{ vk::DescriptorType::eUniformBuffer, 4 * MaxFramesInFlight + 8 },
            vk::DescriptorPoolSize{ vk::DescriptorType::eStorageBuffer, 8 * MaxFramesInFlight + 8 },
            vk::DescriptorPoolSize{ vk::DescriptorType::eStorageImage, 4 } };
        vk::DescriptorPoolCreateInfo poolInfo{};
        poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;  // texture sets freed on Ref drop
        poolInfo.maxSets = 1024 + 8 * MaxFramesInFlight + 16;
        poolInfo.setPoolSizes(poolSizes);
        auto pool = checked(renderer.device.createDescriptorPool(poolInfo), "descriptorPool");
        if (!pool)
        {
            return Err(pool.error());
        }
        renderer.descriptorPool = *pool;

        // The bindless set comes from its own update-after-bind pool.
        vk::DescriptorPoolSize bindlessPoolSize{ vk::DescriptorType::eCombinedImageSampler, MaxBindlessTextures };
        vk::DescriptorPoolCreateInfo bindlessPoolInfo{};
        bindlessPoolInfo.flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind;
        bindlessPoolInfo.maxSets = 1;
        bindlessPoolInfo.setPoolSizes(bindlessPoolSize);
        auto bindlessPoolResult = checked(renderer.device.createDescriptorPool(bindlessPoolInfo), "bindlessPool");
        if (!bindlessPoolResult)
        {
            return Err(bindlessPoolResult.error());
        }
        renderer.bindlessPool = *bindlessPoolResult;

        vk::DescriptorSetAllocateInfo bindlessAlloc{};
        bindlessAlloc.descriptorPool = renderer.bindlessPool;
        bindlessAlloc.setSetLayouts(renderer.bindlessSetLayout);
        auto bindlessAllocated = checked(renderer.device.allocateDescriptorSets(bindlessAlloc), "allocate bindlessSet");
        if (!bindlessAllocated)
        {
            return Err(bindlessAllocated.error());
        }
        renderer.bindlessSet = (*bindlessAllocated)[0];

        for (u32 i = 0; i < MaxFramesInFlight; i = i + 1)
        {
            VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bufferInfo.size = sizeof(LightUbo);
            bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo mapped{};
            if (vmaCreateBuffer(renderer.allocator, &bufferInfo, &allocInfo, &renderer.lightBuffers[i], &renderer.lightAllocs[i], &mapped) != VK_SUCCESS)
            {
                return Err(std::string{ "light UBO vmaCreateBuffer failed" });
            }
            renderer.lightMapped[i] = mapped.pMappedData;

            vk::DescriptorSetAllocateInfo setAlloc{};
            setAlloc.descriptorPool = renderer.descriptorPool;
            setAlloc.setSetLayouts(renderer.lightSetLayout);
            auto allocated = checked(renderer.device.allocateDescriptorSets(setAlloc), "allocate lightSet");
            if (!allocated)
            {
                return Err(allocated.error());
            }
            renderer.lightSets[i] = (*allocated)[0];

            vk::DescriptorBufferInfo lightBufferInfo{ vk::Buffer{ renderer.lightBuffers[i] }, 0, sizeof(LightUbo) };
            vk::WriteDescriptorSet lightWrite{};
            lightWrite.dstSet = renderer.lightSets[i];
            lightWrite.dstBinding = 0;
            lightWrite.descriptorType = vk::DescriptorType::eUniformBuffer;
            lightWrite.setBufferInfo(lightBufferInfo);
            renderer.device.updateDescriptorSets(lightWrite, {});

            // The punctual-light buffer starts at LightListInitial capacity and is
            // grown by ensureLightCapacity; bind it now so the set is complete.
            Result<Ref<Buffer>> lightList =
                makeMappedStorageBuffer(renderer, static_cast<vk::DeviceSize>(LightListInitial) * sizeof(GpuLight));
            if (!lightList)
            {
                return Err(lightList.error());
            }
            renderer.lightListBuffers[i] = *lightList;
            renderer.lightListCapacity[i] = LightListInitial;
            vk::DescriptorBufferInfo lightListInfo{ (*lightList)->buffer, 0, (*lightList)->size };
            vk::WriteDescriptorSet lightListWrite{};
            lightListWrite.dstSet = renderer.lightSets[i];
            lightListWrite.dstBinding = 1;
            lightListWrite.descriptorType = vk::DescriptorType::eStorageBuffer;
            lightListWrite.setBufferInfo(lightListInfo);
            renderer.device.updateDescriptorSets(lightListWrite, {});

            // The instance buffer is created lazily (ensureInstanceCapacity), which
            // also writes this set; allocate the set up front so it is stable.
            vk::DescriptorSetAllocateInfo instanceAlloc{};
            instanceAlloc.descriptorPool = renderer.descriptorPool;
            instanceAlloc.setSetLayouts(renderer.instanceSetLayout);
            auto instanceAllocated = checked(renderer.device.allocateDescriptorSets(instanceAlloc), "allocate instanceSet");
            if (!instanceAllocated)
            {
                return Err(instanceAllocated.error());
            }
            renderer.instanceSets[i] = (*instanceAllocated)[0];

            // Cluster light-list buffer (device-local, compute-written) + the
            // cluster params UBO (host-mapped, written each frame).
            Result<Ref<Buffer>> clusterBuffer =
                makeDeviceStorageBuffer(renderer, ClusterCount * ClusterStride);
            if (!clusterBuffer)
            {
                return Err(clusterBuffer.error());
            }
            renderer.clusterBuffers[i] = *clusterBuffer;

            VkBufferCreateInfo paramInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            paramInfo.size = sizeof(ClusterParams);
            paramInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo paramAlloc{};
            paramAlloc.usage = VMA_MEMORY_USAGE_AUTO;
            paramAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo paramMapped{};
            if (vmaCreateBuffer(renderer.allocator, &paramInfo, &paramAlloc, &renderer.clusterParamBuffers[i],
                                &renderer.clusterParamAllocs[i], &paramMapped) != VK_SUCCESS)
            {
                return Err(std::string{ "cluster params vmaCreateBuffer failed" });
            }
            renderer.clusterParamMapped[i] = paramMapped.pMappedData;

            // Lighting set bindings 2 (cluster lists) + 3 (cluster params).
            vk::DescriptorBufferInfo clusterInfo{ (*clusterBuffer)->buffer, 0, (*clusterBuffer)->size };
            vk::DescriptorBufferInfo paramBufferInfo{ vk::Buffer{ renderer.clusterParamBuffers[i] }, 0, sizeof(ClusterParams) };
            std::array<vk::WriteDescriptorSet, 2> lightClusterWrites{};
            lightClusterWrites[0].dstSet = renderer.lightSets[i];
            lightClusterWrites[0].dstBinding = 2;
            lightClusterWrites[0].descriptorType = vk::DescriptorType::eStorageBuffer;
            lightClusterWrites[0].setBufferInfo(clusterInfo);
            lightClusterWrites[1].dstSet = renderer.lightSets[i];
            lightClusterWrites[1].dstBinding = 3;
            lightClusterWrites[1].descriptorType = vk::DescriptorType::eUniformBuffer;
            lightClusterWrites[1].setBufferInfo(paramBufferInfo);
            renderer.device.updateDescriptorSets(lightClusterWrites, {});

            // Compute cluster set: params UBO + light list (read) + cluster lists (write).
            vk::DescriptorSetAllocateInfo clusterAlloc{};
            clusterAlloc.descriptorPool = renderer.descriptorPool;
            clusterAlloc.setSetLayouts(renderer.clusterSetLayout);
            auto clusterAllocated = checked(renderer.device.allocateDescriptorSets(clusterAlloc), "allocate clusterSet");
            if (!clusterAllocated)
            {
                return Err(clusterAllocated.error());
            }
            renderer.clusterSets[i] = (*clusterAllocated)[0];
            std::array<vk::WriteDescriptorSet, 3> clusterWrites{};
            clusterWrites[0].dstSet = renderer.clusterSets[i];
            clusterWrites[0].dstBinding = 0;
            clusterWrites[0].descriptorType = vk::DescriptorType::eUniformBuffer;
            clusterWrites[0].setBufferInfo(paramBufferInfo);
            clusterWrites[1].dstSet = renderer.clusterSets[i];
            clusterWrites[1].dstBinding = 1;
            clusterWrites[1].descriptorType = vk::DescriptorType::eStorageBuffer;
            clusterWrites[1].setBufferInfo(lightListInfo);
            clusterWrites[2].dstSet = renderer.clusterSets[i];
            clusterWrites[2].dstBinding = 2;
            clusterWrites[2].descriptorType = vk::DescriptorType::eStorageBuffer;
            clusterWrites[2].setBufferInfo(clusterInfo);
            renderer.device.updateDescriptorSets(clusterWrites, {});
        }

        // The cull compute pipeline reads/writes the cluster set layout.
        Result<Ref<Pipeline>> cull =
            newComputePipeline(renderer, "shaders/light_cull.spv", renderer.clusterSetLayout);
        if (!cull)
        {
            return Err(cull.error());
        }
        renderer.cullPipeline = *cull;

        // Post-process tonemap: a compute pipeline + a set binding the offscreen
        // color as a storage image (read+written in place). A layer adds the pass.
        Result<Ref<Pipeline>> tonemap =
            newComputePipeline(renderer, "shaders/tonemap.spv", renderer.tonemapSetLayout);
        if (!tonemap)
        {
            return Err(tonemap.error());
        }
        renderer.tonemapPipeline = *tonemap;

        vk::DescriptorSetAllocateInfo tonemapAlloc{};
        tonemapAlloc.descriptorPool = renderer.descriptorPool;
        tonemapAlloc.setSetLayouts(renderer.tonemapSetLayout);
        auto tonemapAllocated = checked(renderer.device.allocateDescriptorSets(tonemapAlloc), "allocate tonemapSet");
        if (!tonemapAllocated)
        {
            return Err(tonemapAllocated.error());
        }
        renderer.tonemapSet = (*tonemapAllocated)[0];
        updateTonemapSet(renderer);

        // FXAA: a compute pipeline + a set (source sampler + offscreen storage). The
        // scratch source is created on demand when FXAA is enabled (recreateFxaaTarget).
        Result<Ref<Pipeline>> fxaa =
            newComputePipeline(renderer, "shaders/fxaa.spv", renderer.fxaaSetLayout);
        if (!fxaa)
        {
            return Err(fxaa.error());
        }
        renderer.fxaaPipeline = *fxaa;
        vk::DescriptorSetAllocateInfo fxaaAlloc{};
        fxaaAlloc.descriptorPool = renderer.descriptorPool;
        fxaaAlloc.setSetLayouts(renderer.fxaaSetLayout);
        auto fxaaAllocated = checked(renderer.device.allocateDescriptorSets(fxaaAlloc), "allocate fxaaSet");
        if (!fxaaAllocated)
        {
            return Err(fxaaAllocated.error());
        }
        renderer.fxaaSet = (*fxaaAllocated)[0];

        // Depth pre-pass: a vertex-only pipeline that reuses the mesh vertex shader.
        Result<Ref<Pipeline>> depthPrepass =
            makeDepthPrepassPipeline(renderer, "shaders/mesh.spv");
        if (!depthPrepass)
        {
            return Err(depthPrepass.error());
        }
        renderer.depthPrepassPipeline = *depthPrepass;
        return {};
    }
}
