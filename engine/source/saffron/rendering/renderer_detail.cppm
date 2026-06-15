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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
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

    // Defined below; forward-declared so the synchronous one-off paths (buildBlas) can use them.
    auto oneOffCommandPool(Renderer& renderer) -> vk::CommandPool;
    auto submitAndWait(Renderer& renderer, vk::CommandBuffer cmd) -> Result<void>;
    // Allocate a one-off primary command buffer on the one-off pool, begin it OneTimeSubmit, run
    // `record`, end, submitAndWait (a fence, never waitIdle), then free it. For synchronous
    // uploads/builds only — never the frame loop. The caller owns staging resources and frees them
    // around the call (and any extra resources in the failure path, since this returns the Result).
    auto withOneOffCommands(Renderer& renderer, std::string_view label, const RenderFn& record) -> Result<void>;

    void transitionImage(vk::CommandBuffer cmd, vk::Image image, vk::ImageLayout oldLayout, vk::ImageLayout newLayout,
                         vk::PipelineStageFlags2 srcStage, vk::AccessFlags2 srcAccess, vk::PipelineStageFlags2 dstStage,
                         vk::AccessFlags2 dstAccess, vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor)
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
        builder
            .set_desired_format(VkSurfaceFormatKHR{ .format = VK_FORMAT_B8G8R8A8_UNORM,
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
            auto semaphore =
                checked(renderer.context.device.createSemaphore(vk::SemaphoreCreateInfo{}), "createSemaphore");
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

    auto newColorImage(Renderer& renderer, u32 width, u32 height, vk::Format format, bool storage = false,
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
        if (vmaCreateImage(renderer.context.allocator, &imageInfo, &allocInfo, &rawImage, &allocation, nullptr) !=
            VK_SUCCESS)
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
                       vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1, bool sampled = false)
        -> Result<Image>
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
        if (vmaCreateImage(renderer.context.allocator, &imageInfo, &allocInfo, &rawImage, &allocation, nullptr) !=
            VK_SUCCESS)
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

    // A 3D storage+sampled image (the DDGI voxel scene proxy). The one genuinely new
    // resource type beyond 2D/cube; an e3D view, storage + sampled usage.
    auto newImage3D(Renderer& renderer, u32 w, u32 h, u32 d, vk::Format format) -> Result<Image3D>
    {
        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_3D;
        imageInfo.format = static_cast<VkFormat>(format);
        imageInfo.extent = VkExtent3D{ w, h, d };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VkImage raw = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        if (vmaCreateImage(renderer.context.allocator, &imageInfo, &allocInfo, &raw, &allocation, nullptr) !=
            VK_SUCCESS)
        {
            return Err(std::string{ "vmaCreateImage failed for 3D image" });
        }
        vk::ImageViewCreateInfo viewInfo{};
        viewInfo.image = vk::Image{ raw };
        viewInfo.viewType = vk::ImageViewType::e3D;
        viewInfo.format = format;
        viewInfo.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
        vk::ResultValue<vk::ImageView> view = renderer.context.device.createImageView(viewInfo);
        if (view.result != vk::Result::eSuccess)
        {
            vmaDestroyImage(renderer.context.allocator, raw, allocation);
            return Err(std::format("createImageView (3D): {}", vk::to_string(view.result)));
        }
        Image3D result;
        result.device = renderer.context.device;
        result.allocator = renderer.context.allocator;
        result.image = vk::Image{ raw };
        result.view = view.value;
        result.alloc = allocation;
        result.extent = vk::Extent3D{ w, h, d };
        result.format = format;
        result.layout = vk::ImageLayout::eUndefined;
        return result;
    }

    // Device address of a buffer (core 1.2 vkGetBufferDeviceAddress, statically exported).
    auto bufferDeviceAddress(Renderer& renderer, vk::Buffer buffer) -> vk::DeviceAddress
    {
        VkBufferDeviceAddressInfo info{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        info.buffer = static_cast<VkBuffer>(buffer);
        return vkGetBufferDeviceAddress(static_cast<VkDevice>(renderer.context.device), &info);
    }

    // A device-local buffer with a chosen usage (e.g. AS storage / scratch / instances),
    // optionally shader-device-address-enabled. Returned as a Ref<Buffer> (RAII).
    auto makeRtBuffer(Renderer& renderer, vk::DeviceSize bytes, VkBufferUsageFlags usage, bool hostVisible)
        -> Result<Ref<Buffer>>
    {
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = bytes;
        info.usage = usage;
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO;
        void* mapped = nullptr;
        VmaAllocationInfo allocInfo{};
        if (hostVisible)
        {
            alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        }
        VkBuffer raw = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        if (vmaCreateBuffer(renderer.context.allocator, &info, &alloc, &raw, &allocation, &allocInfo) != VK_SUCCESS)
        {
            return Err(std::string{ "makeRtBuffer: vmaCreateBuffer failed" });
        }
        Buffer b;
        b.allocator = renderer.context.allocator;
        b.buffer = vk::Buffer{ raw };
        b.alloc = allocation;
        b.mapped = hostVisible ? allocInfo.pMappedData : nullptr;
        b.size = bytes;
        return std::make_shared<Buffer>(std::move(b));
    }

    // Creates an AccelerationStructure (BLAS or TLAS) of the given size + type: allocates the
    // backing AS storage buffer, calls vkCreateAccelerationStructureKHR, fetches its device
    // address. The caller records the build separately.
    auto createAccelStructure(Renderer& renderer, vk::DeviceSize size, VkAccelerationStructureTypeKHR type)
        -> Result<Ref<AccelerationStructure>>
    {
        auto storage = makeRtBuffer(
            renderer, size,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
        if (!storage)
        {
            return Err(storage.error());
        }

        VkAccelerationStructureCreateInfoKHR ci{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        ci.buffer = static_cast<VkBuffer>((*storage)->buffer);
        ci.size = size;
        ci.type = type;
        VkAccelerationStructureKHR rawAs = VK_NULL_HANDLE;
        if (renderer.context.rt.createAccel(static_cast<VkDevice>(renderer.context.device), &ci, nullptr, &rawAs) !=
            VK_SUCCESS)
        {
            return Err(std::string{ "vkCreateAccelerationStructureKHR failed" });
        }

        AccelerationStructure as;
        as.device = renderer.context.device;
        as.allocator = renderer.context.allocator;
        as.destroyFn = renderer.context.rt.destroyAccel;
        as.handle = vk::AccelerationStructureKHR{ rawAs };
        as.buffer = (*storage)->buffer;
        as.alloc = (*storage)->alloc;
        (*storage)->buffer = nullptr;  // ownership moved into the AS
        (*storage)->alloc = nullptr;

        VkAccelerationStructureDeviceAddressInfoKHR ai{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR
        };
        ai.accelerationStructure = rawAs;
        as.address = renderer.context.rt.getAccelAddress(static_cast<VkDevice>(renderer.context.device), &ai);
        return std::make_shared<AccelerationStructure>(std::move(as));
    }

    // Builds a BLAS for one mesh (one geometry over its whole vertex/index buffer). Synchronous
    // one-off (own command buffer + waitIdle), like uploadMesh. PREFER_FAST_TRACE (no compaction
    // for v1 — correctness first). Called from uploadMesh when RT is supported.
    auto buildBlas(Renderer& renderer, const GpuMesh& mesh) -> Result<Ref<AccelerationStructure>>
    {
        VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        geom.geometry.triangles.vertexData.deviceAddress = bufferDeviceAddress(renderer, mesh.vertexBuffer);
        geom.geometry.triangles.vertexStride = sizeof(Vertex);
        geom.geometry.triangles.maxVertex = mesh.vertexCount > 0 ? mesh.vertexCount - 1 : 0;
        geom.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        geom.geometry.triangles.indexData.deviceAddress = bufferDeviceAddress(renderer, mesh.indexBuffer);

        const u32 triangleCount = mesh.indexCount / 3;
        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
        };
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &geom;

        VkAccelerationStructureBuildSizesInfoKHR sizes{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        renderer.context.rt.getBuildSizes(static_cast<VkDevice>(renderer.context.device),
                                          VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &triangleCount,
                                          &sizes);

        auto blas = createAccelStructure(renderer, sizes.accelerationStructureSize,
                                         VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
        if (!blas)
        {
            return Err(blas.error());
        }
        auto scratch =
            makeRtBuffer(renderer, sizes.buildScratchSize,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
        if (!scratch)
        {
            return Err(scratch.error());
        }

        buildInfo.dstAccelerationStructure = static_cast<VkAccelerationStructureKHR>((*blas)->handle);
        buildInfo.scratchData.deviceAddress = bufferDeviceAddress(renderer, (*scratch)->buffer);
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = triangleCount;
        const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = oneOffCommandPool(renderer);
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(renderer.context.device.allocateCommandBuffers(cmdAlloc), "blas cmd");
        if (!cmds)
        {
            return Err(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];
        vk::CommandBufferBeginInfo begin{};
        begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(begin));
        renderer.context.rt.cmdBuild(static_cast<VkCommandBuffer>(cmd), 1, &buildInfo, &pRange);
        static_cast<void>(cmd.end());
        auto submitted = submitAndWait(renderer, cmd);
        renderer.context.device.freeCommandBuffers(oneOffCommandPool(renderer), cmd);
        if (!submitted)
        {
            return Err(submitted.error());
        }
        return *blas;
    }

    // Records the per-frame skinned BLAS refits into `cmd`: for each skinned instance this
    // frame, (re)builds a per-instance BLAS over its slice of the frame's deformed buffer.
    // The first frame for an entity is a full MODE_BUILD (with ALLOW_UPDATE); every later
    // frame is an in-place MODE_UPDATE (src == dst) since skinned topology never changes —
    // only vertex positions move. Mirrors recordTlasBuild: records into a graph-supplied cmd
    // (no own command buffer / waitIdle), and hand-writes the AS-build -> AS-build barrier so
    // the TLAS build that reads these BLASes waits on them. The skin-compute -> AS-build-read
    // barrier on the deformed buffer is graph-derived (the pass declares AccelStructBuildRead).
    void recordSkinnedBlasBuilds(Renderer& renderer, vk::CommandBuffer cmd, u32 frame,
                                 const std::vector<SkinnedRtInstance>& skinned)
    {
        if (skinned.empty() || !renderer.skinning.deformedBuffers[frame])
        {
            return;
        }
        const vk::DeviceAddress deformedBase =
            bufferDeviceAddress(renderer, renderer.skinning.deformedBuffers[frame]->buffer);

        // Size the shared scratch to the largest build/update need across this frame's set,
        // then refit each instance into it (the AS-build barrier below serializes reuse).
        vk::DeviceSize scratchNeeded = 0;
        struct Plan
        {
            SkinnedBlas* slot = nullptr;
            VkAccelerationStructureGeometryKHR geom{};
            u32 triangleCount = 0;
            bool update = false;
        };
        std::vector<Plan> plans;
        plans.reserve(skinned.size());
        for (const SkinnedRtInstance& inst : skinned)
        {
            if (!inst.mesh || inst.vertexCount == 0 || inst.indexCount < 3 || inst.entity == 0)
            {
                continue;
            }
            SkinnedBlas& slot = renderer.rt.skinnedBlas[frame][inst.entity];
            Plan plan;
            plan.slot = &slot;
            plan.triangleCount = inst.indexCount / 3;
            plan.geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
            plan.geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            plan.geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            plan.geom.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            plan.geom.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            plan.geom.geometry.triangles.vertexData.deviceAddress =
                deformedBase + static_cast<vk::DeviceAddress>(inst.deformedOffset) * sizeof(Vertex);
            plan.geom.geometry.triangles.vertexStride = sizeof(Vertex);
            plan.geom.geometry.triangles.maxVertex = inst.vertexCount - 1;
            plan.geom.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
            plan.geom.geometry.triangles.indexData.deviceAddress =
                bufferDeviceAddress(renderer, inst.mesh->indexBuffer);

            VkAccelerationStructureBuildGeometryInfoKHR sizeInfo{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
            };
            sizeInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            // FAST_TRACE + ALLOW_UPDATE is the standard skinned-refit pair: traversal-optimised,
            // yet refittable in place. ALLOW_UPDATE must be present on the FIRST build so later
            // MODE_UPDATE refits are legal.
            sizeInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                             VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            sizeInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            sizeInfo.geometryCount = 1;
            sizeInfo.pGeometries = &plan.geom;

            VkAccelerationStructureBuildSizesInfoKHR sizes{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
            };
            renderer.context.rt.getBuildSizes(static_cast<VkDevice>(renderer.context.device),
                                              VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &sizeInfo,
                                              &plan.triangleCount, &sizes);

            // Build the AS on the first frame for this entity; refit it in place afterwards.
            if (!slot.as)
            {
                auto blas = createAccelStructure(renderer, sizes.accelerationStructureSize,
                                                 VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
                if (!blas)
                {
                    logError(blas.error());
                    continue;
                }
                slot.as = *blas;
                slot.built = false;
            }
            plan.update = slot.built;
            scratchNeeded = glm::max(scratchNeeded, plan.update ? sizes.updateScratchSize : sizes.buildScratchSize);
            plans.push_back(plan);
        }
        if (plans.empty())
        {
            return;
        }
        if (!renderer.rt.blasScratchBuffers[frame] || renderer.rt.blasScratchCapacity[frame] < scratchNeeded)
        {
            auto scratch =
                makeRtBuffer(renderer, scratchNeeded,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
            if (!scratch)
            {
                logError(scratch.error());
                return;
            }
            renderer.rt.blasScratchBuffers[frame] = *scratch;
            renderer.rt.blasScratchCapacity[frame] = static_cast<u32>(scratchNeeded);
        }
        const vk::DeviceAddress scratchAddr =
            bufferDeviceAddress(renderer, renderer.rt.blasScratchBuffers[frame]->buffer);

        // Every build reuses the one shared scratch region, so consecutive builds must serialize
        // on it (the next build's scratch write waits on the prior build's scratch use). Emit an
        // AS-build -> AS-build barrier between builds; the per-character count is tiny so this is
        // cheaper than plumbing aligned per-build scratch offsets.
        vk::MemoryBarrier2 scratchBarrier{};
        scratchBarrier.srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR;
        scratchBarrier.srcAccessMask =
            vk::AccessFlagBits2::eAccelerationStructureWriteKHR | vk::AccessFlagBits2::eAccelerationStructureReadKHR;
        scratchBarrier.dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR;
        scratchBarrier.dstAccessMask =
            vk::AccessFlagBits2::eAccelerationStructureWriteKHR | vk::AccessFlagBits2::eAccelerationStructureReadKHR;
        vk::DependencyInfo scratchDep{};
        scratchDep.setMemoryBarriers(scratchBarrier);

        for (std::size_t i = 0; i < plans.size(); i = i + 1)
        {
            const Plan& plan = plans[i];
            if (i > 0)
            {
                cmd.pipelineBarrier2(scratchDep);  // serialize the shared scratch reuse
            }
            VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
            };
            buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                              VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            buildInfo.mode = plan.update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                         : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            buildInfo.geometryCount = 1;
            buildInfo.pGeometries = &plan.geom;
            VkAccelerationStructureKHR dst = static_cast<VkAccelerationStructureKHR>(plan.slot->as->handle);
            buildInfo.dstAccelerationStructure = dst;
            buildInfo.srcAccelerationStructure = plan.update ? dst : VK_NULL_HANDLE;  // in-place refit
            buildInfo.scratchData.deviceAddress = scratchAddr;
            VkAccelerationStructureBuildRangeInfoKHR range{};
            range.primitiveCount = plan.triangleCount;
            const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
            renderer.context.rt.cmdBuild(static_cast<VkCommandBuffer>(cmd), 1, &buildInfo, &pRange);
            plan.slot->built = true;
        }
        renderer.rt.skinnedBlasCount = static_cast<u32>(plans.size());

        // Barrier: the BLAS refit writes (build stage) -> the TLAS build that reads them as
        // input (build stage). Hand-written here, the accepted AS-pass exception to graph-derived
        // barriers (recordTlasBuild emits the matching AS-build -> fragment one for the TLAS).
        vk::MemoryBarrier2 barrier{};
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR;
        barrier.srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR;
        barrier.dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR;
        vk::DependencyInfo dep{};
        dep.setMemoryBarriers(barrier);
        cmd.pipelineBarrier2(dep);
    }

    // Ensures the frame's TLAS instance buffer holds `count` instances (host-visible, AS
    // build input + BDA), growing to the next power of two. Allocates/resizes the TLAS +
    // scratch lazily inside recordTlasBuild (sizes depend on count).
    auto ensureTlasCapacity(Renderer& renderer, u32 frame, u32 count) -> Result<void>
    {
        if (renderer.rt.instanceBuffers[frame] && renderer.rt.instanceCapacity[frame] >= count)
        {
            return {};
        }
        u32 capacity = renderer.rt.instanceCapacity[frame];
        if (capacity == 0)
        {
            capacity = 64;
        }
        while (capacity < count)
        {
            capacity = capacity * 2;
        }
        auto buf =
            makeRtBuffer(renderer, static_cast<vk::DeviceSize>(capacity) * sizeof(VkAccelerationStructureInstanceKHR),
                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                         true);
        if (!buf)
        {
            return Err(buf.error());
        }
        renderer.rt.instanceBuffers[frame] = *buf;
        renderer.rt.instanceCapacity[frame] = capacity;
        return {};
    }

    // Records the per-frame TLAS build into `cmd`: queries sizes for `count` instances,
    // (re)creates the TLAS + scratch on a capacity change, builds, then writes the TLAS into
    // the frame's mesh set (set 6). A graph compute pass supplies cmd; the graph emits the
    // AS-build -> fragment barrier via the declared usage (StorageWriteCompute on a sentinel).
    void recordTlasBuild(Renderer& renderer, vk::CommandBuffer cmd, u32 frame, u32 count)
    {
        VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        geom.geometry.instances.arrayOfPointers = VK_FALSE;
        geom.geometry.instances.data.deviceAddress =
            bufferDeviceAddress(renderer, renderer.rt.instanceBuffers[frame]->buffer);

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
        };
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &geom;

        // Size for the instance-buffer capacity (>= count) so the TLAS is stable until the
        // buffer regrows; query both that and the actual count's scratch.
        const u32 capacity = renderer.rt.instanceCapacity[frame];
        VkAccelerationStructureBuildSizesInfoKHR capSizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
        };
        renderer.context.rt.getBuildSizes(static_cast<VkDevice>(renderer.context.device),
                                          VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &capacity,
                                          &capSizes);
        VkAccelerationStructureBuildSizesInfoKHR sizes{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        renderer.context.rt.getBuildSizes(static_cast<VkDevice>(renderer.context.device),
                                          VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &count, &sizes);

        // (Re)create the TLAS when the slot still holds the shared empty seed (capacity 0) or
        // is too small for this frame's instance count.
        if (renderer.rt.tlasCapacity[frame] < count)
        {
            auto tlas = createAccelStructure(renderer, capSizes.accelerationStructureSize,
                                             VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
            if (!tlas)
            {
                logError(tlas.error());
                return;
            }
            renderer.rt.tlas[frame] = *tlas;
            renderer.rt.tlasCapacity[frame] = capacity;
            // Write the new TLAS into the frame's mesh set (set 6, binding 0).
            VkWriteDescriptorSetAccelerationStructureKHR asWrite{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
            };
            VkAccelerationStructureKHR handle =
                static_cast<VkAccelerationStructureKHR>(renderer.rt.tlas[frame]->handle);
            asWrite.accelerationStructureCount = 1;
            asWrite.pAccelerationStructures = &handle;
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.pNext = &asWrite;
            w.dstSet = static_cast<VkDescriptorSet>(renderer.rt.meshSets[frame]);
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            vkUpdateDescriptorSets(static_cast<VkDevice>(renderer.context.device), 1, &w, 0, nullptr);
        }
        const vk::DeviceSize scratchNeeded = glm::max(sizes.buildScratchSize, capSizes.buildScratchSize);
        if (!renderer.rt.scratchBuffers[frame] || renderer.rt.scratchCapacity[frame] < scratchNeeded)
        {
            auto scratch =
                makeRtBuffer(renderer, scratchNeeded,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
            if (!scratch)
            {
                logError(scratch.error());
                return;
            }
            renderer.rt.scratchBuffers[frame] = *scratch;
            renderer.rt.scratchCapacity[frame] = static_cast<u32>(scratchNeeded);
        }

        buildInfo.dstAccelerationStructure = static_cast<VkAccelerationStructureKHR>(renderer.rt.tlas[frame]->handle);
        buildInfo.scratchData.deviceAddress = bufferDeviceAddress(renderer, renderer.rt.scratchBuffers[frame]->buffer);
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = count;
        const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
        renderer.context.rt.cmdBuild(static_cast<VkCommandBuffer>(cmd), 1, &buildInfo, &pRange);

        // Barrier: the AS build (build stage / write) -> the fragment shader ray-query read.
        vk::MemoryBarrier2 barrier{};
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR;
        barrier.srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
        barrier.dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR;
        vk::DependencyInfo dep{};
        dep.setMemoryBarriers(barrier);
        cmd.pipelineBarrier2(dep);
    }

    // Builds a 0-instance TLAS (synchronous) and writes it into every frame's mesh set, so
    // set 6 always references a valid AS even before/without a real per-frame build (the
    // mesh fragment statically references rtScene regardless of the runtime flag).
    auto seedEmptyTlas(Renderer& renderer) -> Result<void>
    {
        VkAccelerationStructureGeometryKHR geom{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
        };
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &geom;
        const u32 zero = 0;
        VkAccelerationStructureBuildSizesInfoKHR sizes{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        renderer.context.rt.getBuildSizes(static_cast<VkDevice>(renderer.context.device),
                                          VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &zero, &sizes);

        auto empty = createAccelStructure(renderer, glm::max<vk::DeviceSize>(sizes.accelerationStructureSize, 256),
                                          VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
        if (!empty)
        {
            return Err(empty.error());
        }
        auto scratch =
            makeRtBuffer(renderer, glm::max<vk::DeviceSize>(sizes.buildScratchSize, 256),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
        if (!scratch)
        {
            return Err(scratch.error());
        }
        buildInfo.dstAccelerationStructure = static_cast<VkAccelerationStructureKHR>((*empty)->handle);
        buildInfo.scratchData.deviceAddress = bufferDeviceAddress(renderer, (*scratch)->buffer);
        VkAccelerationStructureBuildRangeInfoKHR range{};  // 0 instances
        const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = renderer.frame.frames[0].commandPool;
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(renderer.context.device.allocateCommandBuffers(cmdAlloc), "empty tlas cmd");
        if (!cmds)
        {
            return Err(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];
        vk::CommandBufferBeginInfo begin{};
        begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(begin));
        renderer.context.rt.cmdBuild(static_cast<VkCommandBuffer>(cmd), 1, &buildInfo, &pRange);
        static_cast<void>(cmd.end());
        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = cmd;
        vk::SubmitInfo2 submitInfo{};
        submitInfo.setCommandBufferInfos(cmdInfo);
        static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, nullptr));
        static_cast<void>(renderer.context.device.waitIdle());
        renderer.context.device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);

        // Hold the empty TLAS in every frame slot + write it into every mesh set. The first
        // real per-frame build overwrites a slot's TLAS (and re-writes its set) on demand.
        VkAccelerationStructureKHR handle = static_cast<VkAccelerationStructureKHR>((*empty)->handle);
        for (u32 i = 0; i < MaxFramesInFlight; i = i + 1)
        {
            renderer.rt.tlas[i] = *empty;  // shared until a real build replaces this slot
            VkWriteDescriptorSetAccelerationStructureKHR asWrite{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
            };
            asWrite.accelerationStructureCount = 1;
            asWrite.pAccelerationStructures = &handle;
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.pNext = &asWrite;
            w.dstSet = static_cast<VkDescriptorSet>(renderer.rt.meshSets[i]);
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            vkUpdateDescriptorSets(static_cast<VkDevice>(renderer.context.device), 1, &w, 0, nullptr);
        }
        return {};
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
        if (vmaCreateImage(renderer.context.allocator, &imageInfo, &allocInfo, &rawImage, &allocation, nullptr) !=
            VK_SUCCESS)
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
    auto newColorCubeImage(Renderer& renderer, u32 size, vk::Format format, Image& out,
                           std::array<vk::ImageView, 6>& outFaces) -> Result<void>
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
        if (vmaCreateImage(renderer.context.allocator, &imageInfo, &allocInfo, &rawImage, &allocation, nullptr) !=
            VK_SUCCESS)
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
                for (u32 f = 0; f < face; f = f + 1)
                {
                    renderer.context.device.destroyImageView(outFaces[f]);
                }
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
        const std::array<glm::vec3, 6> fwd{ glm::vec3(1, 0, 0),  glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0),
                                            glm::vec3(0, -1, 0), glm::vec3(0, 0, 1),  glm::vec3(0, 0, -1) };
        const std::array<glm::vec3, 6> up{ glm::vec3(0, -1, 0), glm::vec3(0, -1, 0), glm::vec3(0, 0, 1),
                                           glm::vec3(0, 0, -1), glm::vec3(0, -1, 0), glm::vec3(0, -1, 0) };
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
        activeView(renderer).msaaColor.reset();
        activeView(renderer).msaaDepth.reset();
        if (renderer.targets.sampleCount == vk::SampleCountFlagBits::e1)
        {
            return;
        }
        const u32 w = activeView(renderer).offscreen.extent.width;
        const u32 h = activeView(renderer).offscreen.extent.height;
        if (w == 0 || h == 0)
        {
            return;
        }
        Result<Image> color = newColorImage(renderer, w, h, OffscreenColorFormat, false, renderer.targets.sampleCount);
        if (color)
        {
            activeView(renderer).msaaColor = std::move(*color);
        }
        else
        {
            logError(color.error());
        }
        auto depth = newDepthImage(renderer, w, h, renderer.targets.sampleCount);
        if (depth)
        {
            activeView(renderer).msaaDepth = std::move(*depth);
        }
        else
        {
            logError(depth.error());
        }
    }

    void updateFxaaSet(Renderer& renderer);          // defined alongside updateTonemapSet below
    void recreateSsaoTargets(Renderer& renderer);    // defined after the SSAO pipeline helpers
    void recreateTaaTargets(Renderer& renderer);     // defined after the TAA pipeline helpers
    void recreateRestirTargets(Renderer& renderer);  // defined after the ReSTIR pipeline helpers
    // Frees one shm publisher's readback slots + mapped segment (defined in renderer_capture.cpp);
    // destroyShmPublish loops it over every view, destroyView calls it for one.
    void destroyShmPublishSlots(Renderer& renderer, ShmPublish& shm);

    // (Re)create the 1x scratch target FXAA + TAA read from (the scene renders here when
    // either is on); drop it when neither. Sized to the offscreen; the GPU is already idle.
    void recreateFxaaTarget(Renderer& renderer)
    {
        activeView(renderer).scratch.reset();
        if (!renderer.targets.fxaaEnabled && !renderer.targets.taaEnabled)
        {
            return;
        }
        const u32 w = activeView(renderer).offscreen.extent.width;
        const u32 h = activeView(renderer).offscreen.extent.height;
        if (w == 0 || h == 0)
        {
            return;
        }
        auto scratch = newColorImage(renderer, w, h, OffscreenColorFormat, false);
        if (scratch)
        {
            activeView(renderer).scratch = std::move(*scratch);
            if (renderer.targets.fxaaEnabled)
            {
                updateFxaaSet(renderer);
            }
        }
        else
        {
            logError(scratch.error());
        }
    }

    // Host-visible, mapped buffer the caller owns (vmaDestroyBuffer when done).
    auto newHostCaptureBuffer(Renderer& renderer, vk::DeviceSize bytes, VkBuffer& outBuffer, VmaAllocation& outAlloc,
                              VmaAllocationInfo& outInfo) -> Result<void>
    {
        VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        if (vmaCreateBuffer(renderer.context.allocator, &bufferInfo, &allocInfo, &outBuffer, &outAlloc, &outInfo) !=
            VK_SUCCESS)
        {
            return Err(std::string{ "capture: vmaCreateBuffer failed" });
        }
        return {};
    }

    // The command pool one-off uploads/readbacks allocate from. Defaults to the main frame pool, but
    // the thumbnail worker thread rebinds this (thread-locally) to the renderer's dedicated worker
    // pool — Vulkan command pools are not thread-safe, so each thread must own its pool.
    inline thread_local vk::CommandPool tlsThumbnailPool = nullptr;

    auto oneOffCommandPool(Renderer& renderer) -> vk::CommandPool
    {
        return tlsThumbnailPool ? tlsThumbnailPool : renderer.frame.frames[0].commandPool;
    }

    // Submits a one-off command buffer on the graphics queue and blocks only on *its* completion via
    // a fresh fence — never `device.waitIdle()`, which would drain the in-flight scene frame too. The
    // out-of-band upload/thumbnail paths use this so a worker thread (and the present loop) are never
    // stalled. The queue is externally synchronized, so the submit is taken under gpuQueueMutex (the
    // frame loop's submit/present sites take the same lock). The caller owns `cmd` and frees it after.
    auto submitAndWait(Renderer& renderer, vk::CommandBuffer cmd) -> Result<void>
    {
        vk::FenceCreateInfo fenceInfo{};
        auto fence = checked(renderer.context.device.createFence(fenceInfo), "submitAndWait: createFence");
        if (!fence)
        {
            return Err(fence.error());
        }
        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = cmd;
        vk::SubmitInfo2 submitInfo{};
        submitInfo.setCommandBufferInfos(cmdInfo);
        {
            const std::lock_guard<std::mutex> lock(gpuQueueMutex());
            if (auto submitted =
                    checked(renderer.context.graphicsQueue.submit2(submitInfo, *fence), "submitAndWait: submit2");
                !submitted)
            {
                renderer.context.device.destroyFence(*fence);
                return Err(submitted.error());
            }
        }
        const vk::Result waited =
            renderer.context.device.waitForFences(*fence, VK_TRUE, std::numeric_limits<u64>::max());
        renderer.context.device.destroyFence(*fence);
        return checked(waited, "submitAndWait: waitForFences");
    }

    auto withOneOffCommands(Renderer& renderer, std::string_view label, const RenderFn& record) -> Result<void>
    {
        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = oneOffCommandPool(renderer);
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(renderer.context.device.allocateCommandBuffers(cmdAlloc), label);
        if (!cmds)
        {
            return Err(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];
        vk::CommandBufferBeginInfo begin{};
        begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(begin));
        record(cmd);
        static_cast<void>(cmd.end());
        auto submitted = submitAndWait(renderer, cmd);
        renderer.context.device.freeCommandBuffers(oneOffCommandPool(renderer), cmd);
        return submitted;
    }

    // Records a fromLayout->TransferSrc barrier, the image->buffer copy, and a
    // TransferSrc->toLayout barrier into a caller-owned command buffer.
    void captureImageToBuffer(vk::CommandBuffer cmd, vk::Image image, vk::Extent2D extent, vk::ImageLayout fromLayout,
                              vk::PipelineStageFlags2 fromStage, vk::AccessFlags2 fromAccess, vk::ImageLayout toLayout,
                              vk::PipelineStageFlags2 toStage, vk::AccessFlags2 toAccess, vk::Buffer destination)
    {
        transitionImage(cmd, image, fromLayout, vk::ImageLayout::eTransferSrcOptimal, fromStage, fromAccess,
                        vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);

        vk::BufferImageCopy region{};
        region.imageSubresource = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
        region.imageExtent = vk::Extent3D{ extent.width, extent.height, 1 };
        cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, destination, region);

        transitionImage(cmd, image, vk::ImageLayout::eTransferSrcOptimal, toLayout, vk::PipelineStageFlagBits2::eCopy,
                        vk::AccessFlagBits2::eTransferRead, toStage, toAccess);
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

    // Converts a captured framebuffer to a tightly-packed 3-channel RGB buffer (one conversion
    // path shared by the file + in-memory encoders below). 8-bit sources reorder BGRA->RGB. For
    // RGBA16F, `transfer` selects the mapping: Clamp ([0,1]×255) for an already-tonemapped
    // offscreen, or Tonemap (Reinhard + gamma) for scene-linear HDR-asset radiance.
    auto convertToRgb(const unsigned char* pixels, u32 width, u32 height, vk::Format format,
                      PngTransfer transfer = PngTransfer::Clamp) -> std::vector<unsigned char>
    {
        std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * height * 3);
        if (format == vk::Format::eR16G16B16A16Sfloat)
        {
            const u32* halfs = reinterpret_cast<const u32*>(pixels);  // two halves per u32
            const auto encode = [transfer](f32 c) -> unsigned char
            {
                f32 v = c < 0.0f ? 0.0f : c;
                if (transfer == PngTransfer::Tonemap)
                {
                    v = v / (1.0f + v);            // Reinhard: scene-linear radiance -> [0,1)
                    v = std::pow(v, 1.0f / 2.2f);  // gamma encode for display
                }
                else
                {
                    v = v > 1.0f ? 1.0f : v;
                }
                return static_cast<unsigned char>(v * 255.0f + 0.5f);
            };
            for (u32 i = 0; i < width * height; i = i + 1)
            {
                const glm::vec2 rg = glm::unpackHalf2x16(halfs[i * 2 + 0]);
                const glm::vec2 ba = glm::unpackHalf2x16(halfs[i * 2 + 1]);
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
        return rgb;
    }

    // Writes a PNG file. 8-bit sources reorder BGRA->RGB; the HDR (RGBA16F) offscreen is
    // already tonemapped to display range, so its half floats are clamped to [0,1]*255.
    auto writeBufferToPng(const unsigned char* pixels, u32 width, u32 height, vk::Format format,
                          const std::string& path) -> Result<void>
    {
        const std::vector<unsigned char> rgb = convertToRgb(pixels, width, height, format);
        const int ok = stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 3, rgb.data(),
                                      static_cast<int>(width) * 3);
        if (ok == 0)
        {
            return Err(std::format("stbi_write_png failed for '{}'", path));
        }
        return {};
    }

    // Encodes a captured framebuffer to PNG bytes in memory (no file). Used for thumbnails
    // shipped over the JSON control protocol as base64.
    auto encodeBufferToPng(const unsigned char* pixels, u32 width, u32 height, vk::Format format,
                           PngTransfer transfer = PngTransfer::Clamp) -> Result<std::vector<u8>>
    {
        const std::vector<unsigned char> rgb = convertToRgb(pixels, width, height, format, transfer);
        std::vector<u8> out;
        const auto append = [](void* context, void* data, int size)
        {
            auto& buffer = *static_cast<std::vector<u8>*>(context);
            const u8* bytes = static_cast<const u8*>(data);
            buffer.insert(buffer.end(), bytes, bytes + size);
        };
        const int ok = stbi_write_png_to_func(append, &out, static_cast<int>(width), static_cast<int>(height), 3,
                                              rgb.data(), static_cast<int>(width) * 3);
        if (ok == 0)
        {
            return Err(std::string{ "stbi_write_png_to_func failed" });
        }
        return out;
    }

    // Matches the shader's set 1 light uniform (std140).
    struct LightUbo
    {
        glm::vec4 directionAmbient;    // xyz direction, w ambient
        glm::vec4 colorIntensity;      // rgb color, a intensity
        glm::uvec4 counts;             // x=punctual count, y=dir shadow on, z=IBL on
        glm::vec4 eyePosition;         // xyz world-space camera position
        glm::mat4 shadowViewProj;      // directional light-space transform (world -> shadow clip)
        glm::mat4 spotShadowViewProj;  // shadowed spot light-space transform (perspective)
        glm::uvec4 spotShadow;         // x = shadowed spot's light index, y = enabled (0/1)
        glm::vec4 pointShadow;         // xyz = shadowed point light world pos, w = far plane
        glm::uvec4 pointShadowMeta;    // x = shadowed point's light index, y = enabled (0/1)
        glm::uvec4 screenFlags;        // x = contact-shadows, y = SSGI, z = DDGI (AO is counts.w)
        glm::vec4 ddgiVolumeMin;       // xyz = DDGI volume world min corner
        glm::vec4 ddgiVolumeExtent;    // xyz = DDGI volume world size
        glm::uvec4 ddgiProbeCount;     // xyz = probes per axis
        glm::vec4 ambientColor;        // rgb = scene-environment ambient (non-IBL fallback), a unused
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

    // TAA: per-pixel screen-space motion (rg16f); history is the offscreen HDR format.
    inline constexpr vk::Format MotionFormat = vk::Format::eR16G16Sfloat;
    inline constexpr f32 TaaHistoryWeight = 0.9f;

    // DDGI: one coarse probe volume + a voxel scene proxy. Probe counts per axis, rays per
    // probe per frame, octahedral tile interiors (irradiance 8, distance 16), atlas formats.
    inline constexpr u32 DdgiProbesX = 8;
    inline constexpr u32 DdgiProbesY = 4;
    inline constexpr u32 DdgiProbesZ = 8;
    inline constexpr u32 DdgiRaysPerProbe = 64;
    inline constexpr u32 DdgiIrrInterior = 8;
    inline constexpr u32 DdgiDistInterior = 16;
    inline constexpr u32 DdgiVoxelRes = 32;  // voxel proxy is DdgiVoxelRes^3
    inline constexpr vk::Format DdgiVoxelFormat = vk::Format::eR16G16B16A16Sfloat;
    inline constexpr vk::Format DdgiIrrFormat = vk::Format::eR16G16B16A16Sfloat;
    inline constexpr vk::Format DdgiDistFormat = vk::Format::eR16G16Sfloat;
    inline constexpr u32 DdgiMaxBoxes = 256;
    inline constexpr f32 DdgiHysteresis = 0.95f;

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

    // Atmosphere LUT sizes (Hillaire 2020). Small + persistent; baked into ShaderReadOnly
    // alongside envCube and re-baked with the sun. The multiscatter/skyview counts are coarse
    // first-target sizes adequate on llvmpipe.
    inline constexpr u32 AtmosTransmittanceW = 256;
    inline constexpr u32 AtmosTransmittanceH = 64;
    inline constexpr u32 AtmosMultiScatterSize = 32;
    inline constexpr u32 AtmosSkyViewW = 192;
    inline constexpr u32 AtmosSkyViewH = 108;

    // One reflection-probe metadata record in the probe SSBO (std430). Mirrors the ProbeMeta
    // struct in mesh.slang: origin + influence radius, box extents + intensity, a valid flag.
    struct ProbeMetaGpu
    {
        glm::vec4 originRadius{ 0.0f };     // xyz = world origin, w = influence radius
        glm::vec4 extentIntensity{ 0.0f };  // xyz = box half-extents, w = intensity
        glm::uvec4 flags{ 0 };              // x = valid (1/0), y = boxProjection (1/0)
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

    // A device-local buffer usable as both a storage buffer (the compute skinning pass
    // writes it) and a vertex stream (the scene pass draws it) — the deformed-vertex
    // buffer for the frame.
    auto makeDeviceVertexStorageBuffer(Renderer& renderer, vk::DeviceSize bytes) -> Result<Ref<Buffer>>
    {
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        // When RT is on the deformed buffer also feeds the per-frame skinned BLAS refit, so it
        // needs shader device address + AS-build-input usage (mirrors the static mesh buffers).
        if (renderer.context.rtSupported)
        {
            info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        }
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO;
        VkBuffer raw = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        if (vmaCreateBuffer(renderer.context.allocator, &info, &alloc, &raw, &allocation, nullptr) != VK_SUCCESS)
        {
            return Err(std::string{ "makeDeviceVertexStorageBuffer: vmaCreateBuffer failed" });
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
    auto newComputePipeline(Renderer& renderer, std::string_view shaderName, vk::DescriptorSetLayout setLayout,
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
        auto layoutResult =
            checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (compute)");
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
            vk::VertexInputAttributeDescription{ 2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv0) }
        };
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
        std::array<vk::DescriptorSetLayout, 3> setLayouts{ renderer.descriptors.bindlessSetLayout,
                                                           renderer.descriptors.lightSetLayout,
                                                           renderer.descriptors.instanceSetLayout };
        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setSetLayouts(setLayouts);
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult =
            checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (depth-prepass)");
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
            vk::VertexInputAttributeDescription{ 2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv0) }
        };
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
        std::array<vk::DynamicState, 3> dynamicStates{ vk::DynamicState::eViewport, vk::DynamicState::eScissor,
                                                       vk::DynamicState::eDepthBias };
        vk::PipelineDynamicStateCreateInfo dynamic{};
        dynamic.setDynamicStates(dynamicStates);

        vk::PipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.depthAttachmentFormat = DepthFormat;

        vk::PushConstantRange pushConstant{};
        pushConstant.stageFlags = vk::ShaderStageFlagBits::eVertex;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(glm::mat4);
        std::array<vk::DescriptorSetLayout, 3> setLayouts{ renderer.descriptors.bindlessSetLayout,
                                                           renderer.descriptors.lightSetLayout,
                                                           renderer.descriptors.instanceSetLayout };
        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setSetLayouts(setLayouts);
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult =
            checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (shadow)");
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
            vk::VertexInputAttributeDescription{ 2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv0) }
        };
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
        std::array<vk::DescriptorSetLayout, 3> setLayouts{ renderer.descriptors.bindlessSetLayout,
                                                           renderer.descriptors.lightSetLayout,
                                                           renderer.descriptors.instanceSetLayout };
        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setSetLayouts(setLayouts);
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult =
            checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (point-shadow)");
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
            vk::VertexInputAttributeDescription{ 2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv0) }
        };
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
        std::array<vk::DescriptorSetLayout, 3> setLayouts{ renderer.descriptors.bindlessSetLayout,
                                                           renderer.descriptors.lightSetLayout,
                                                           renderer.descriptors.instanceSetLayout };
        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setSetLayouts(setLayouts);
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult =
            checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (gbuffer)");
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

    // Fullscreen sky PSO: no vertex input, depth test+write off, cull none. Bakes the sample
    // count so it can render into the (possibly multisampled) scene color target; rebuilt on
    // AA change. Sets: 0 = bindless (panorama), 1 = sky (envCube). Push constant: fragment.
    auto makeSkyPipeline(Renderer& renderer) -> Result<Ref<Pipeline>>
    {
        auto moduleResult = loadShaderModule(renderer.context.device, assetPath("shaders/sky.spv"));
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

        vk::PipelineVertexInputStateCreateInfo vertexInput{};  // fullscreen triangle: no inputs
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
        multisample.rasterizationSamples = renderer.targets.sampleCount;  // match the scene color target
        vk::PipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;
        vk::PipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.setAttachments(blendAttachment);
        std::array<vk::DynamicState, 2> dynamicStates{ vk::DynamicState::eViewport, vk::DynamicState::eScissor };
        vk::PipelineDynamicStateCreateInfo dynamic{};
        dynamic.setDynamicStates(dynamicStates);

        vk::PipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.setColorAttachmentFormats(OffscreenColorFormat);  // no depth attachment

        vk::PushConstantRange pushConstant{};
        pushConstant.stageFlags = vk::ShaderStageFlagBits::eFragment;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(glm::mat4) + 2 * sizeof(glm::vec4);  // invViewProj + params + clearColor
        std::array<vk::DescriptorSetLayout, 2> setLayouts{ renderer.descriptors.bindlessSetLayout,
                                                           renderer.sky.setLayout };
        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setSetLayouts(setLayouts);
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult =
            checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (sky)");
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
            return Err(std::format("createGraphicsPipeline (sky): {}", vk::to_string(created.result)));
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
        activeView(renderer).gNormal.reset();
        activeView(renderer).gDepth.reset();
        activeView(renderer).aoRaw.reset();
        activeView(renderer).aoMap.reset();
        activeView(renderer).contactMap.reset();
        activeView(renderer).ssgiMap.reset();
        activeView(renderer).ssgiDenoised.reset();
        activeView(renderer).ssgiResolved.reset();
        activeView(renderer).prevColor.reset();
        const u32 w = activeView(renderer).offscreen.extent.width;
        const u32 h = activeView(renderer).offscreen.extent.height;
        if (w == 0 || h == 0)
        {
            return;
        }
        auto gNormal = newColorImage(renderer, w, h, GNormalFormat, false);
        if (!gNormal)
        {
            logError(gNormal.error());
            return;
        }
        activeView(renderer).gNormal = std::move(*gNormal);
        auto gDepth = newDepthImage(renderer, w, h);
        if (!gDepth)
        {
            logError(gDepth.error());
            return;
        }
        activeView(renderer).gDepth = std::move(*gDepth);
        auto aoRaw = newColorImage(renderer, w, h, AoFormat, true);
        if (!aoRaw)
        {
            logError(aoRaw.error());
            return;
        }
        activeView(renderer).aoRaw = std::move(*aoRaw);
        auto aoMap = newColorImage(renderer, w, h, AoFormat, true);
        if (!aoMap)
        {
            logError(aoMap.error());
            return;
        }
        activeView(renderer).aoMap = std::move(*aoMap);
        auto contactMap = newColorImage(renderer, w, h, AoFormat, true);
        if (!contactMap)
        {
            logError(contactMap.error());
            return;
        }
        activeView(renderer).contactMap = std::move(*contactMap);
        auto ssgiMap = newColorImage(renderer, w, h, GNormalFormat, true);  // rgba16f radiance
        if (!ssgiMap)
        {
            logError(ssgiMap.error());
            return;
        }
        activeView(renderer).ssgiMap = std::move(*ssgiMap);
        auto ssgiDenoised = newColorImage(renderer, w, h, GNormalFormat, true);  // rgba16f radiance
        if (!ssgiDenoised)
        {
            logError(ssgiDenoised.error());
            return;
        }
        activeView(renderer).ssgiDenoised = std::move(*ssgiDenoised);
        auto ssgiResolved = newColorImage(renderer, w, h, GNormalFormat, true);  // rgba16f radiance
        if (!ssgiResolved)
        {
            logError(ssgiResolved.error());
            return;
        }
        activeView(renderer).ssgiResolved = std::move(*ssgiResolved);
        auto prevColor = newColorImage(renderer, w, h, OffscreenColorFormat, true);
        if (!prevColor)
        {
            logError(prevColor.error());
            return;
        }
        activeView(renderer).prevColor = std::move(*prevColor);

        // Transition the three mesh-sampled maps to ShaderReadOnly so set 4 is valid even
        // before the passes first run (each is gated by its enable flag in the shader), and
        // prevColor (sampled by SSGI) likewise. No clears needed — gated reads.
        {
            vk::CommandBufferAllocateInfo allocInfo{};
            allocInfo.commandPool = renderer.frame.frames[0].commandPool;
            allocInfo.level = vk::CommandBufferLevel::ePrimary;
            allocInfo.commandBufferCount = 1;
            auto cmds = checked(renderer.context.device.allocateCommandBuffers(allocInfo), "ssao init cmd");
            if (!cmds)
            {
                logError(cmds.error());
                return;
            }
            vk::CommandBuffer cmd = (*cmds)[0];
            vk::CommandBufferBeginInfo begin{};
            begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
            static_cast<void>(cmd.begin(begin));
            const std::array<Image*, 6> maps{ &activeView(renderer).aoMap,        &activeView(renderer).contactMap,
                                              &activeView(renderer).ssgiMap,      &activeView(renderer).ssgiDenoised,
                                              &activeView(renderer).ssgiResolved, &activeView(renderer).prevColor };
            for (Image* img : maps)
            {
                transitionImage(cmd, img->image, vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal,
                                vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
                img->layout = vk::ImageLayout::eShaderReadOnlyOptimal;
            }
            static_cast<void>(cmd.end());
            vk::CommandBufferSubmitInfo cmdInfo{};
            cmdInfo.commandBuffer = cmd;
            vk::SubmitInfo2 submitInfo{};
            submitInfo.setCommandBufferInfos(cmdInfo);
            static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, nullptr));
            static_cast<void>(renderer.context.device.waitIdle());
            renderer.context.device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);
        }

        const vk::Sampler nearest = renderer.ssao.sampler;
        const vk::Sampler linear = renderer.descriptors.linearSampler;
        auto sampled = [](vk::Sampler s, vk::ImageView v) -> vk::DescriptorImageInfo
        { return vk::DescriptorImageInfo{ s, v, vk::ImageLayout::eShaderReadOnlyOptimal }; };
        auto storage = [](vk::ImageView v) -> vk::DescriptorImageInfo
        {
            vk::DescriptorImageInfo i{};
            i.imageView = v;
            i.imageLayout = vk::ImageLayout::eGeneral;
            return i;
        };
        auto write =
            [&](vk::DescriptorSet set, u32 binding, vk::DescriptorType type, const vk::DescriptorImageInfo& info)
        {
            vk::WriteDescriptorSet wr{};
            wr.dstSet = set;
            wr.dstBinding = binding;
            wr.descriptorType = type;
            wr.setImageInfo(info);
            renderer.context.device.updateDescriptorSets(wr, {});
        };
        const auto S = vk::DescriptorType::eCombinedImageSampler;
        const auto I = vk::DescriptorType::eStorageImage;
        const vk::ImageView gv = activeView(renderer).gNormal.view;

        // All sets are this (active) view's per-view sets, bound to this view's images.
        ViewTargets& vt = activeView(renderer);
        // gtao: gbuffer -> aoRaw
        write(vt.gtaoSet, 0, S, sampled(nearest, gv));
        write(vt.gtaoSet, 1, I, storage(vt.aoRaw.view));
        // ao_blur: aoRaw + gbuffer -> aoMap
        write(vt.aoBlurSet, 0, S, sampled(nearest, vt.aoRaw.view));
        write(vt.aoBlurSet, 1, S, sampled(nearest, gv));
        write(vt.aoBlurSet, 2, I, storage(vt.aoMap.view));
        // contact: gbuffer -> contactMap
        write(vt.contactSet, 0, S, sampled(nearest, gv));
        write(vt.contactSet, 1, I, storage(vt.contactMap.view));
        // ssgi: gbuffer + prevColor -> ssgiMap
        write(vt.ssgiSet, 0, S, sampled(nearest, gv));
        write(vt.ssgiSet, 1, S, sampled(linear, vt.prevColor.view));
        write(vt.ssgiSet, 2, I, storage(vt.ssgiMap.view));
        // ssgi_blur: raw ssgiMap + gbuffer -> ssgiDenoised
        write(vt.ssgiBlurSet, 0, S, sampled(nearest, vt.ssgiMap.view));
        write(vt.ssgiBlurSet, 1, S, sampled(nearest, gv));
        write(vt.ssgiBlurSet, 2, I, storage(vt.ssgiDenoised.view));
        // copy_color: offscreen sceneColor + prevColor storage
        write(vt.copyColorSet, 0, S, sampled(linear, vt.offscreen.view));
        write(vt.copyColorSet, 1, I, storage(vt.prevColor.view));
        // mesh set 4: AO + contact + SSGI (all linear-sampled). The scene reads the temporally
        // resolved SSGI: accumulation runs whenever SSGI is on (independent of the AA mode) and is
        // scheduled before the scene pass, so ssgiResolved is always written before this sample.
        write(vt.meshSet, 0, S, sampled(linear, vt.aoMap.view));
        write(vt.meshSet, 1, S, sampled(linear, vt.contactMap.view));
        write(vt.meshSet, 2, S, sampled(linear, vt.ssgiResolved.view));

        renderer.ssao.generation = renderer.ssao.generation + 1;
        renderer.ssao.ready = true;
    }

    // The motion-vector prepass pipeline: instanced scene, depth-tested, writing screen
    // motion (rg16f) from cur/prev camera reprojection. Same shape as the G-buffer prepass
    // (set 2 = instances) with a 2x mat4 vertex push constant. Reuses motion.slang.
    auto makeMotionPipeline(Renderer& renderer, std::string_view shaderName) -> Result<Ref<Pipeline>>
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

        // Two per-vertex streams: binding 0 is this frame's (deformed) Vertex stream, binding
        // 1 the previous frame's. The drawIndexed vertexOffset applies to both, so a skinned
        // batch shifts cur + prev by the same deformedVertexOffset; a static batch binds the
        // same buffer twice (prevPosition == position, object motion comes from prevModel).
        std::array<vk::VertexInputBindingDescription, 2> bindings{
            vk::VertexInputBindingDescription{ 0, sizeof(Vertex), vk::VertexInputRate::eVertex },
            vk::VertexInputBindingDescription{ 1, sizeof(Vertex), vk::VertexInputRate::eVertex }
        };
        std::array<vk::VertexInputAttributeDescription, 4> attributes{
            vk::VertexInputAttributeDescription{ 0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position) },
            vk::VertexInputAttributeDescription{ 1, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, normal) },
            vk::VertexInputAttributeDescription{ 2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv0) },
            vk::VertexInputAttributeDescription{ 3, 1, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position) }
        };
        vk::PipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.setVertexBindingDescriptions(bindings);
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
        renderingInfo.setColorAttachmentFormats(MotionFormat);
        renderingInfo.depthAttachmentFormat = DepthFormat;

        vk::PushConstantRange pushConstant{};
        pushConstant.stageFlags = vk::ShaderStageFlagBits::eVertex;
        pushConstant.offset = 0;
        pushConstant.size = 2 * sizeof(glm::mat4);
        std::array<vk::DescriptorSetLayout, 3> setLayouts{ renderer.descriptors.bindlessSetLayout,
                                                           renderer.descriptors.lightSetLayout,
                                                           renderer.descriptors.instanceSetLayout };
        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setSetLayouts(setLayouts);
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult =
            checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (motion)");
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
            return Err(std::format("createGraphicsPipeline (motion): {}", vk::to_string(created.result)));
        }
        Pipeline pipeline;
        pipeline.device = renderer.context.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;
        return std::make_shared<Pipeline>(std::move(pipeline));
    }

    // (Re)creates the TAA targets (motion + its depth scratch + 2 ping-pong history images)
    // at the viewport extent and rewrites both parities of the TAA compute set. The history
    // is marked invalid so the first post-resize frame falls back to the current frame.
    // Called at init + after the offscreen resizes (the caller has idled the GPU).
    void recreateTaaTargets(Renderer& renderer)
    {
        activeView(renderer).motion.reset();
        activeView(renderer).motionDepth.reset();
        activeView(renderer).history[0].reset();
        activeView(renderer).history[1].reset();
        activeView(renderer).ssgiHistory[0].reset();
        activeView(renderer).ssgiHistory[1].reset();
        activeView(renderer).historyValid = false;
        const u32 w = activeView(renderer).offscreen.extent.width;
        const u32 h = activeView(renderer).offscreen.extent.height;
        if (w == 0 || h == 0)
        {
            return;
        }
        auto motion = newColorImage(renderer, w, h, MotionFormat, false);
        if (!motion)
        {
            logError(motion.error());
            return;
        }
        activeView(renderer).motion = std::move(*motion);
        auto motionDepth = newDepthImage(renderer, w, h);
        if (!motionDepth)
        {
            logError(motionDepth.error());
            return;
        }
        activeView(renderer).motionDepth = std::move(*motionDepth);
        for (u32 i = 0; i < 2; i = i + 1)
        {
            auto hist = newColorImage(renderer, w, h, OffscreenColorFormat, true);  // storage (TAA writes it)
            if (!hist)
            {
                logError(hist.error());
                return;
            }
            activeView(renderer).history[i] = std::move(*hist);
        }
        for (u32 i = 0; i < 2; i = i + 1)
        {
            // rgba16f to hold HDR indirect radiance (NOT the display-format scene color).
            auto hist = newColorImage(renderer, w, h, GNormalFormat, true);
            if (!hist)
            {
                logError(hist.error());
                return;
            }
            activeView(renderer).ssgiHistory[i] = std::move(*hist);
        }

        // The history images start ShaderReadOnly so their sampler bindings are valid even
        // before the first TAA write (historyValid gates the actual blend).
        {
            vk::CommandBufferAllocateInfo allocInfo{};
            allocInfo.commandPool = renderer.frame.frames[0].commandPool;
            allocInfo.level = vk::CommandBufferLevel::ePrimary;
            allocInfo.commandBufferCount = 1;
            auto cmds = checked(renderer.context.device.allocateCommandBuffers(allocInfo), "taa init cmd");
            if (!cmds)
            {
                logError(cmds.error());
                return;
            }
            vk::CommandBuffer cmd = (*cmds)[0];
            vk::CommandBufferBeginInfo begin{};
            begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
            static_cast<void>(cmd.begin(begin));
            const std::array<Image*, 4> histImages{ &activeView(renderer).history[0], &activeView(renderer).history[1],
                                                    &activeView(renderer).ssgiHistory[0],
                                                    &activeView(renderer).ssgiHistory[1] };
            for (Image* img : histImages)
            {
                transitionImage(cmd, img->image, vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal,
                                vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderSampledRead);
                img->layout = vk::ImageLayout::eShaderReadOnlyOptimal;
            }
            static_cast<void>(cmd.end());
            vk::CommandBufferSubmitInfo cmdInfo{};
            cmdInfo.commandBuffer = cmd;
            vk::SubmitInfo2 submitInfo{};
            submitInfo.setCommandBufferInfos(cmdInfo);
            static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, nullptr));
            static_cast<void>(renderer.context.device.waitIdle());
            renderer.context.device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);
        }

        // Rewrite both TAA set parities. Parity p writes history[p] and reads history[1-p];
        // current + motion + offscreen are the same in both. (offscreen view is stable.)
        // TAA reads the scene's 1x result from the scratch image (the scene renders there
        // when TAA is on, mirroring FXAA). When scratch isn't allocated (TAA off), bind the
        // offscreen as a valid placeholder — the set is unused until TAA turns on + rebinds.
        vk::ImageView sceneInput = activeView(renderer).scratch.image ? activeView(renderer).scratch.view
                                                                      : activeView(renderer).offscreen.view;
        for (u32 p = 0; p < 2; p = p + 1)
        {
            vk::DescriptorImageInfo curInfo{ renderer.descriptors.linearSampler, sceneInput,
                                             vk::ImageLayout::eShaderReadOnlyOptimal };
            vk::DescriptorImageInfo histInfo{ renderer.descriptors.linearSampler,
                                              activeView(renderer).history[1 - p].view,
                                              vk::ImageLayout::eShaderReadOnlyOptimal };
            vk::DescriptorImageInfo motionInfo{ renderer.descriptors.linearSampler, activeView(renderer).motion.view,
                                                vk::ImageLayout::eShaderReadOnlyOptimal };
            vk::DescriptorImageInfo outInfo{};
            outInfo.imageView = activeView(renderer).offscreen.view;
            outInfo.imageLayout = vk::ImageLayout::eGeneral;
            vk::DescriptorImageInfo histOut{};
            histOut.imageView = activeView(renderer).history[p].view;
            histOut.imageLayout = vk::ImageLayout::eGeneral;
            std::array<vk::WriteDescriptorSet, 5> writes{};
            const std::array<vk::DescriptorImageInfo*, 5> infos{ &curInfo, &histInfo, &motionInfo, &outInfo, &histOut };
            const std::array<vk::DescriptorType, 5> types{ vk::DescriptorType::eCombinedImageSampler,
                                                           vk::DescriptorType::eCombinedImageSampler,
                                                           vk::DescriptorType::eCombinedImageSampler,
                                                           vk::DescriptorType::eStorageImage,
                                                           vk::DescriptorType::eStorageImage };
            for (u32 b = 0; b < 5; b = b + 1)
            {
                writes[b].dstSet = activeView(renderer).taaSets[p];
                writes[b].dstBinding = b;
                writes[b].descriptorType = types[b];
                writes[b].setImageInfo(*infos[b]);
            }
            renderer.context.device.updateDescriptorSets(writes, {});

            // SSGI temporal accumulation shares the parity: parity p reads ssgiHistory[1-p],
            // writes ssgiHistory[p], and resolves into the stable ssgiResolved map the scene reads.
            vk::DescriptorImageInfo ssgiCur{ renderer.descriptors.linearSampler, activeView(renderer).ssgiDenoised.view,
                                             vk::ImageLayout::eShaderReadOnlyOptimal };
            vk::DescriptorImageInfo ssgiHist{ renderer.descriptors.linearSampler,
                                              activeView(renderer).ssgiHistory[1 - p].view,
                                              vk::ImageLayout::eShaderReadOnlyOptimal };
            vk::DescriptorImageInfo ssgiResolvedOut{};
            ssgiResolvedOut.imageView = activeView(renderer).ssgiResolved.view;
            ssgiResolvedOut.imageLayout = vk::ImageLayout::eGeneral;
            vk::DescriptorImageInfo ssgiHistOut{};
            ssgiHistOut.imageView = activeView(renderer).ssgiHistory[p].view;
            ssgiHistOut.imageLayout = vk::ImageLayout::eGeneral;
            std::array<vk::WriteDescriptorSet, 5> ssgiWrites{};
            const std::array<vk::DescriptorImageInfo*, 5> ssgiInfos{ &ssgiCur, &ssgiHist, &motionInfo, &ssgiResolvedOut,
                                                                     &ssgiHistOut };
            for (u32 b = 0; b < 5; b = b + 1)
            {
                ssgiWrites[b].dstSet = activeView(renderer).ssgiAccumSets[p];
                ssgiWrites[b].dstBinding = b;
                ssgiWrites[b].descriptorType = types[b];
                ssgiWrites[b].setImageInfo(*ssgiInfos[b]);
            }
            renderer.context.device.updateDescriptorSets(ssgiWrites, {});
        }

        // Repoint the mesh's SSGI sampler (set 4, binding 2) at the authoritative final map for
        // the current AA state: the temporally resolved map when TAA is on, the spatially
        // denoised map otherwise. recreateSsaoTargets seeds it too, but TAA toggles route only
        // through here, so this is the binding of record.
        {
            const vk::ImageView ssgiSceneView = renderer.targets.taaEnabled ? activeView(renderer).ssgiResolved.view
                                                                            : activeView(renderer).ssgiDenoised.view;
            vk::DescriptorImageInfo info{ renderer.descriptors.linearSampler, ssgiSceneView,
                                          vk::ImageLayout::eShaderReadOnlyOptimal };
            vk::WriteDescriptorSet wr{};
            wr.dstSet = activeView(renderer).meshSet;
            wr.dstBinding = 2;
            wr.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            wr.setImageInfo(info);
            renderer.context.device.updateDescriptorSets(wr, {});
        }
    }

    // (Re)creates the ReSTIR reservoir buffers (initial/combined/previous, 32B/pixel) + the
    // output radiance image at the viewport extent, and writes the STABLE descriptor bindings
    // (reservoirs, radiance, the resolve TLAS). The per-frame light + cluster SSBO + sampler
    // bindings (G-buffer/motion) are rewritten in the ReSTIR graph passes since those buffers
    // change. Called at init + on resize (GPU already idle). Requires rtSupported.
    void recreateRestirTargets(Renderer& renderer)
    {
        RestirView& rv = activeRestir(renderer);
        rv.ready = false;
        rv.radiance.reset();
        rv.initial.reset();
        rv.combined.reset();
        rv.previous.reset();
        const u32 w = activeView(renderer).offscreen.extent.width;
        const u32 h = activeView(renderer).offscreen.extent.height;
        if (w == 0 || h == 0)
        {
            return;
        }
        const u32 pixels = w * h;
        const vk::DeviceSize reservoirBytes = static_cast<vk::DeviceSize>(pixels) * 2 * sizeof(glm::vec4);  // 2x float4
        auto mk = [&]() -> Result<Ref<Buffer>> { return makeDeviceStorageBuffer(renderer, reservoirBytes); };
        auto r0 = mk();
        if (!r0)
        {
            logError(r0.error());
            return;
        }
        rv.initial = *r0;
        auto r1 = mk();
        if (!r1)
        {
            logError(r1.error());
            return;
        }
        rv.combined = *r1;
        auto r2 = mk();
        if (!r2)
        {
            logError(r2.error());
            return;
        }
        rv.previous = *r2;
        rv.reservoirCapacity = pixels;
        auto rad = newColorImage(renderer, w, h, GNormalFormat, true);  // rgba16f radiance, storage
        if (!rad)
        {
            logError(rad.error());
            return;
        }
        rv.radiance = std::move(*rad);

        // Radiance rests in ShaderReadOnly (mesh samples it; resolve writes it as storage).
        {
            vk::CommandBufferAllocateInfo allocInfo{};
            allocInfo.commandPool = renderer.frame.frames[0].commandPool;
            allocInfo.level = vk::CommandBufferLevel::ePrimary;
            allocInfo.commandBufferCount = 1;
            auto cmds = checked(renderer.context.device.allocateCommandBuffers(allocInfo), "restir init cmd");
            if (!cmds)
            {
                logError(cmds.error());
                return;
            }
            vk::CommandBuffer cmd = (*cmds)[0];
            vk::CommandBufferBeginInfo begin{};
            begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
            static_cast<void>(cmd.begin(begin));
            transitionImage(cmd, rv.radiance.image, vk::ImageLayout::eUndefined,
                            vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eTopOfPipe,
                            vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eFragmentShader,
                            vk::AccessFlagBits2::eShaderSampledRead);
            rv.radiance.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
            static_cast<void>(cmd.end());
            vk::CommandBufferSubmitInfo cmdInfo{};
            cmdInfo.commandBuffer = cmd;
            vk::SubmitInfo2 submitInfo{};
            submitInfo.setCommandBufferInfos(cmdInfo);
            static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, nullptr));
            static_cast<void>(renderer.context.device.waitIdle());
            renderer.context.device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);
        }

        auto bufInfo = [](const Ref<Buffer>& b) { return vk::DescriptorBufferInfo{ b->buffer, 0, b->size }; };
        auto wBuf = [&](vk::DescriptorSet set, u32 binding, const Ref<Buffer>& b)
        {
            auto bi = bufInfo(b);
            vk::WriteDescriptorSet w{};
            w.dstSet = set;
            w.dstBinding = binding;
            w.descriptorType = vk::DescriptorType::eStorageBuffer;
            w.setBufferInfo(bi);
            renderer.context.device.updateDescriptorSets(w, {});
        };
        // initial set: b3 = reservoir out (initial buffer).
        wBuf(rv.initialSet, 3, rv.initial);
        // reuse set: b2 = initial, b3 = previous, b5 = combined.
        wBuf(rv.reuseSet, 2, rv.initial);
        wBuf(rv.reuseSet, 3, rv.previous);
        wBuf(rv.reuseSet, 5, rv.combined);
        // resolve set: b1 = combined, b2 = previousOut. The radiance storage image (b5) + TLAS
        // (b4) + samplers + light SSBO are written per frame in the pass (TLAS changes; light
        // SSBO grows). Radiance storage written here (stable view).
        wBuf(rv.resolveSet, 1, rv.combined);
        wBuf(rv.resolveSet, 2, rv.previous);
        {
            vk::DescriptorImageInfo radStore{};
            radStore.imageView = rv.radiance.view;
            radStore.imageLayout = vk::ImageLayout::eGeneral;
            vk::WriteDescriptorSet w{};
            w.dstSet = rv.resolveSet;
            w.dstBinding = 5;
            w.descriptorType = vk::DescriptorType::eStorageImage;
            w.setImageInfo(radStore);
            renderer.context.device.updateDescriptorSets(w, {});
            // mesh set 7: the radiance sampled.
            vk::DescriptorImageInfo radSamp{ renderer.descriptors.linearSampler, rv.radiance.view,
                                             vk::ImageLayout::eShaderReadOnlyOptimal };
            vk::WriteDescriptorSet wm{};
            wm.dstSet = rv.meshSet;
            wm.dstBinding = 0;
            wm.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            wm.setImageInfo(radSamp);
            renderer.context.device.updateDescriptorSets(wm, {});
        }
        rv.ready = true;
    }

    // Writes the ReSTIR sets' PER-FRAME bindings: the punctual light SSBO + cluster list SSBO
    // (they regrow), the G-buffer + motion samplers, and the TLAS into the resolve set. Called
    // each frame before the ReSTIR passes (the stable bindings were written in recreate).
    void writeRestirFrameBindings(Renderer& renderer, u32 frame)
    {
        vk::Device dev = renderer.context.device;
        const vk::ImageView gv = activeView(renderer).gNormal.view;
        const vk::ImageView mv = activeView(renderer).motion.image ? activeView(renderer).motion.view
                                                                   : gv;  // motion may be absent (TAA off)
        auto wSampler = [&](vk::DescriptorSet set, u32 binding, vk::ImageView v)
        {
            vk::DescriptorImageInfo ii{ renderer.restir.sampler, v, vk::ImageLayout::eShaderReadOnlyOptimal };
            vk::WriteDescriptorSet w{};
            w.dstSet = set;
            w.dstBinding = binding;
            w.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            w.setImageInfo(ii);
            dev.updateDescriptorSets(w, {});
        };
        auto wBuffer = [&](vk::DescriptorSet set, u32 binding, vk::Buffer buf, vk::DeviceSize size)
        {
            vk::DescriptorBufferInfo bi{ buf, 0, size };
            vk::WriteDescriptorSet w{};
            w.dstSet = set;
            w.dstBinding = binding;
            w.descriptorType = vk::DescriptorType::eStorageBuffer;
            w.setBufferInfo(bi);
            dev.updateDescriptorSets(w, {});
        };
        const Ref<Buffer>& lightBuf = renderer.lighting.lightListBuffers[frame];
        const Ref<Buffer>& clusterBuf = renderer.lighting.clusterBuffers[frame];
        RestirView& rv = activeRestir(renderer);

        // initial: b0 gbuffer, b1 lights, b2 clusters.
        wSampler(rv.initialSet, 0, gv);
        if (lightBuf)
        {
            wBuffer(rv.initialSet, 1, lightBuf->buffer, lightBuf->size);
        }
        if (clusterBuf)
        {
            wBuffer(rv.initialSet, 2, clusterBuf->buffer, clusterBuf->size);
        }
        // reuse: b0 gbuffer, b1 motion, b4 lights.
        wSampler(rv.reuseSet, 0, gv);
        wSampler(rv.reuseSet, 1, mv);
        if (lightBuf)
        {
            wBuffer(rv.reuseSet, 4, lightBuf->buffer, lightBuf->size);
        }
        // resolve: b0 gbuffer, b3 lights, b4 TLAS.
        wSampler(rv.resolveSet, 0, gv);
        if (lightBuf)
        {
            wBuffer(rv.resolveSet, 3, lightBuf->buffer, lightBuf->size);
        }
        {
            VkAccelerationStructureKHR handle =
                static_cast<VkAccelerationStructureKHR>(renderer.rt.tlas[frame]->handle);
            VkWriteDescriptorSetAccelerationStructureKHR asWrite{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
            };
            asWrite.accelerationStructureCount = 1;
            asWrite.pAccelerationStructures = &handle;
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.pNext = &asWrite;
            w.dstSet = static_cast<VkDescriptorSet>(rv.resolveSet);
            w.dstBinding = 4;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            vkUpdateDescriptorSets(static_cast<VkDevice>(dev), 1, &w, 0, nullptr);
        }
    }

    // Write a texture into the bindless array at the given slot (set 0, binding 0).
    // update-after-bind: safe to write a live set between draws.
    void writeBindlessTexture(Renderer& renderer, vk::ImageView view, u32 index)
    {
        vk::DescriptorImageInfo info{ renderer.descriptors.linearSampler, view,
                                      vk::ImageLayout::eShaderReadOnlyOptimal };
        vk::WriteDescriptorSet write{};
        write.dstSet = renderer.descriptors.bindlessSet;
        write.dstBinding = 0;
        write.dstArrayElement = index;
        write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        write.setImageInfo(info);
        // Host access to a descriptor set is externally synchronized; the worker writes it too.
        const std::lock_guard<std::mutex> lock(bindlessMutex());
        renderer.context.device.updateDescriptorSets(write, {});
    }

    // Point the tonemap set's storage-image binding at the current offscreen color
    // view (GENERAL layout). Called after the offscreen color is (re)created.
    void updateTonemapSet(Renderer& renderer)
    {
        if (!activeView(renderer).tonemapSet)
        {
            return;
        }
        vk::DescriptorImageInfo imageInfo{};
        imageInfo.imageView = activeView(renderer).offscreen.view;
        imageInfo.imageLayout = vk::ImageLayout::eGeneral;
        vk::WriteDescriptorSet write{};
        write.dstSet = activeView(renderer).tonemapSet;
        write.dstBinding = 0;
        write.descriptorType = vk::DescriptorType::eStorageImage;
        write.setImageInfo(imageInfo);
        renderer.context.device.updateDescriptorSets(write, {});
    }

    // Point the FXAA set at the current scratch (sampled source) + offscreen (storage
    // target) views. Called after either is (re)created.
    void updateFxaaSet(Renderer& renderer)
    {
        if (!activeView(renderer).fxaaSet || !activeView(renderer).scratch.view)
        {
            return;
        }
        vk::DescriptorImageInfo sourceInfo{ renderer.descriptors.linearSampler, activeView(renderer).scratch.view,
                                            vk::ImageLayout::eShaderReadOnlyOptimal };
        vk::DescriptorImageInfo targetInfo{};
        targetInfo.imageView = activeView(renderer).offscreen.view;
        targetInfo.imageLayout = vk::ImageLayout::eGeneral;
        std::array<vk::WriteDescriptorSet, 2> writes{};
        writes[0].dstSet = activeView(renderer).fxaaSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        writes[0].setImageInfo(sourceInfo);
        writes[1].dstSet = activeView(renderer).fxaaSet;
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
        auto shadowSampler =
            checked(renderer.context.device.createSampler(shadowSamplerInfo), "createSampler (shadow)");
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
                                                  renderer.targets.pointShadowCube, renderer.targets.pointShadowFaces);
            !cube)
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
                transitionImage(cmd, map, vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal,
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
        auto materialLayout =
            checked(renderer.context.device.createDescriptorSetLayout(materialLayoutInfo), "bindlessSetLayout");
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
        auto lightLayout =
            checked(renderer.context.device.createDescriptorSetLayout(lightLayoutInfo), "lightSetLayout");
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
        auto clusterLayout =
            checked(renderer.context.device.createDescriptorSetLayout(clusterLayoutInfo), "clusterSetLayout");
        if (!clusterLayout)
        {
            return Err(clusterLayout.error());
        }
        renderer.descriptors.clusterSetLayout = *clusterLayout;

        // Binding 0 is the per-instance array; binding 1 the frame's joint palette for
        // skinned draws. Unskinned shaders never statically use binding 1, so it may
        // stay unwritten until the first skinned draw uploads it.
        std::array<vk::DescriptorSetLayoutBinding, 3> instanceBindings{};
        instanceBindings[0].binding = 0;
        instanceBindings[0].descriptorType = vk::DescriptorType::eStorageBuffer;
        instanceBindings[0].descriptorCount = 1;
        instanceBindings[0].stageFlags = vk::ShaderStageFlagBits::eVertex;
        instanceBindings[1].binding = 1;
        instanceBindings[1].descriptorType = vk::DescriptorType::eStorageBuffer;
        instanceBindings[1].descriptorCount = 1;
        instanceBindings[1].stageFlags = vk::ShaderStageFlagBits::eVertex;
        // Binding 2: per-distinct-material params, read in the fragment stage. Unwritten until
        // the first scene draw uploads it (no pipeline statically uses it before then).
        instanceBindings[2].binding = 2;
        instanceBindings[2].descriptorType = vk::DescriptorType::eStorageBuffer;
        instanceBindings[2].descriptorCount = 1;
        instanceBindings[2].stageFlags = vk::ShaderStageFlagBits::eFragment;
        vk::DescriptorSetLayoutCreateInfo instanceLayoutInfo{};
        instanceLayoutInfo.setBindings(instanceBindings);
        auto instanceLayout =
            checked(renderer.context.device.createDescriptorSetLayout(instanceLayoutInfo), "instanceSetLayout");
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
        auto tonemapLayout =
            checked(renderer.context.device.createDescriptorSetLayout(tonemapLayoutInfo), "tonemapSetLayout");
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

        // Compute skinning set: in-vertices (0), in-skins (1), joint palette (2), out-vertices (3).
        std::array<vk::DescriptorSetLayoutBinding, 4> skinBindings{};
        for (u32 b = 0; b < skinBindings.size(); b = b + 1)
        {
            skinBindings[b].binding = b;
            skinBindings[b].descriptorType = vk::DescriptorType::eStorageBuffer;
            skinBindings[b].descriptorCount = 1;
            skinBindings[b].stageFlags = vk::ShaderStageFlagBits::eCompute;
        }
        vk::DescriptorSetLayoutCreateInfo skinLayoutInfo{};
        skinLayoutInfo.setBindings(skinBindings);
        auto skinLayout = checked(renderer.context.device.createDescriptorSetLayout(skinLayoutInfo), "skinSetLayout");
        if (!skinLayout)
        {
            return Err(skinLayout.error());
        }
        renderer.skinning.setLayout = *skinLayout;

        // One per-frame pool, reset wholesale each frame; sized for SkinMaxSetsPerFrame
        // per-instance sets (4 storage buffers each), doubled to also hold the parallel
        // previous-pose dispatch sets the motion pass reads.
        for (u32 fi = 0; fi < MaxFramesInFlight; fi = fi + 1)
        {
            vk::DescriptorPoolSize skinPoolSize{ vk::DescriptorType::eStorageBuffer, 8 * SkinMaxSetsPerFrame };
            vk::DescriptorPoolCreateInfo skinPoolInfo{};
            skinPoolInfo.maxSets = 2 * SkinMaxSetsPerFrame;
            skinPoolInfo.setPoolSizes(skinPoolSize);
            auto skinPool = checked(renderer.context.device.createDescriptorPool(skinPoolInfo), "skinPool");
            if (!skinPool)
            {
                return Err(skinPool.error());
            }
            renderer.skinning.pools[fi] = *skinPool;
        }

        std::array<vk::DescriptorPoolSize, 5> poolSizes{
            vk::DescriptorPoolSize{ vk::DescriptorType::eCombinedImageSampler, 1024 },
            vk::DescriptorPoolSize{ vk::DescriptorType::eUniformBuffer, 4 * MaxFramesInFlight + 8 },
            // + the DDGI box SSBO + the per-view ReSTIR reservoir bindings (6 SSBOs per view).
            vk::DescriptorPoolSize{ vk::DescriptorType::eStorageBuffer, 8 * MaxFramesInFlight + 16 + 8 * ViewCount },
            // The screen-space + post-process storage-image bindings are per-view (tonemap/fxaa/TAA +
            // gtao/aoBlur/contact/ssgi/copyColor ~= 11 per view); plus DDGI (voxel/ray/atlas) + headroom.
            vk::DescriptorPoolSize{ vk::DescriptorType::eStorageImage, 48 + 11 * ViewCount },
            // RT: one TLAS descriptor per in-flight frame (set 6) + the per-view ReSTIR resolve sets + headroom.
            vk::DescriptorPoolSize{ vk::DescriptorType::eAccelerationStructureKHR, MaxFramesInFlight + 2 + ViewCount }
        };
        vk::DescriptorPoolCreateInfo poolInfo{};
        poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;  // texture sets freed on Ref drop
        // +10 per-view sets (gtao/aoBlur/contact/ssgi/copyColor/mesh + tonemap/fxaa/taa[2]) × ViewCount.
        poolInfo.maxSets = 1024 + 8 * MaxFramesInFlight + 64 + 10 * ViewCount;
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
        auto bindlessPoolResult =
            checked(renderer.context.device.createDescriptorPool(bindlessPoolInfo), "bindlessPool");
        if (!bindlessPoolResult)
        {
            return Err(bindlessPoolResult.error());
        }
        renderer.descriptors.bindlessPool = *bindlessPoolResult;

        vk::DescriptorSetAllocateInfo bindlessAlloc{};
        bindlessAlloc.descriptorPool = renderer.descriptors.bindlessPool;
        bindlessAlloc.setSetLayouts(renderer.descriptors.bindlessSetLayout);
        auto bindlessAllocated =
            checked(renderer.context.device.allocateDescriptorSets(bindlessAlloc), "allocate bindlessSet");
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
            if (vmaCreateBuffer(renderer.context.allocator, &bufferInfo, &allocInfo, &renderer.lighting.lightBuffers[i],
                                &renderer.lighting.lightAllocs[i], &mapped) != VK_SUCCESS)
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

            vk::DescriptorBufferInfo lightBufferInfo{ vk::Buffer{ renderer.lighting.lightBuffers[i] }, 0,
                                                      sizeof(LightUbo) };
            vk::WriteDescriptorSet lightWrite{};
            lightWrite.dstSet = renderer.lighting.lightSets[i];
            lightWrite.dstBinding = 0;
            lightWrite.descriptorType = vk::DescriptorType::eUniformBuffer;
            lightWrite.setBufferInfo(lightBufferInfo);
            renderer.context.device.updateDescriptorSets(lightWrite, {});

            // Bind the directional (binding 4) + spot (binding 5) shadow maps with the
            // compare sampler. The graph guarantees ShaderReadOnly when the scene samples.
            vk::DescriptorImageInfo shadowInfo{ renderer.descriptors.shadowSampler, renderer.targets.shadowMap.view,
                                                vk::ImageLayout::eShaderReadOnlyOptimal };
            vk::DescriptorImageInfo spotShadowInfo{ renderer.descriptors.shadowSampler,
                                                    renderer.targets.spotShadowMap.view,
                                                    vk::ImageLayout::eShaderReadOnlyOptimal };
            // The point distance cube samples with the plain linear sampler (no compare).
            vk::DescriptorImageInfo pointShadowInfo{ renderer.descriptors.linearSampler,
                                                     renderer.targets.pointShadowCube.view,
                                                     vk::ImageLayout::eShaderReadOnlyOptimal };
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
            auto instanceAllocated =
                checked(renderer.context.device.allocateDescriptorSets(instanceAlloc), "allocate instanceSet");
            if (!instanceAllocated)
            {
                return Err(instanceAllocated.error());
            }
            renderer.instancing.sets[i] = (*instanceAllocated)[0];

            // Cluster light-list buffer (device-local, compute-written) + the
            // cluster params UBO (host-mapped, written each frame).
            Result<Ref<Buffer>> clusterBuffer = makeDeviceStorageBuffer(renderer, ClusterCount * ClusterStride);
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
            if (vmaCreateBuffer(renderer.context.allocator, &paramInfo, &paramAlloc,
                                &renderer.lighting.clusterParamBuffers[i], &renderer.lighting.clusterParamAllocs[i],
                                &paramMapped) != VK_SUCCESS)
            {
                return Err(std::string{ "cluster params vmaCreateBuffer failed" });
            }
            renderer.lighting.clusterParamMapped[i] = paramMapped.pMappedData;

            // Lighting set bindings 2 (cluster lists) + 3 (cluster params).
            vk::DescriptorBufferInfo clusterInfo{ (*clusterBuffer)->buffer, 0, (*clusterBuffer)->size };
            vk::DescriptorBufferInfo paramBufferInfo{ vk::Buffer{ renderer.lighting.clusterParamBuffers[i] }, 0,
                                                      sizeof(ClusterParams) };
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
            auto clusterAllocated =
                checked(renderer.context.device.allocateDescriptorSets(clusterAlloc), "allocate clusterSet");
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

        // Compute skinning pre-pass: deforms each skinned mesh-instance once into the
        // frame's deformed-vertex buffer (push constant: vertexCount, jointOffset,
        // deformedOffset, pad = 16 B).
        Result<Ref<Pipeline>> skin = newComputePipeline(renderer, "shaders/skin.spv", renderer.skinning.setLayout, 16);
        if (!skin)
        {
            return Err(skin.error());
        }
        renderer.pipelines.skin = *skin;

        // Tonemap: a compute pipeline + a set binding the offscreen color as a storage
        // image (read+written in place). beginFrameGraph adds the pass every frame; the
        // exposure multiplier is a compute-stage push constant.
        Result<Ref<Pipeline>> tonemap = newComputePipeline(
            renderer, "shaders/tonemap.spv", renderer.descriptors.tonemapSetLayout, static_cast<u32>(sizeof(f32)));
        if (!tonemap)
        {
            return Err(tonemap.error());
        }
        renderer.pipelines.tonemap = *tonemap;

        for (ViewTargets& vt : renderer.views)
        {
            vk::DescriptorSetAllocateInfo tonemapAlloc{};
            tonemapAlloc.descriptorPool = renderer.descriptors.descriptorPool;
            tonemapAlloc.setSetLayouts(renderer.descriptors.tonemapSetLayout);
            auto tonemapAllocated =
                checked(renderer.context.device.allocateDescriptorSets(tonemapAlloc), "allocate tonemapSet");
            if (!tonemapAllocated)
            {
                return Err(tonemapAllocated.error());
            }
            vt.tonemapSet = (*tonemapAllocated)[0];
        }
        updateTonemapSet(renderer);  // writes the active (Scene) view; other views written on their first resize

        // FXAA: a compute pipeline + a set (source sampler + offscreen storage). The
        // scratch source is created on demand when FXAA is enabled (recreateFxaaTarget).
        Result<Ref<Pipeline>> fxaa =
            newComputePipeline(renderer, "shaders/fxaa.spv", renderer.descriptors.fxaaSetLayout);
        if (!fxaa)
        {
            return Err(fxaa.error());
        }
        renderer.pipelines.fxaa = *fxaa;
        for (ViewTargets& vt : renderer.views)
        {
            vk::DescriptorSetAllocateInfo fxaaAlloc{};
            fxaaAlloc.descriptorPool = renderer.descriptors.descriptorPool;
            fxaaAlloc.setSetLayouts(renderer.descriptors.fxaaSetLayout);
            auto fxaaAllocated = checked(renderer.context.device.allocateDescriptorSets(fxaaAlloc), "allocate fxaaSet");
            if (!fxaaAllocated)
            {
                return Err(fxaaAllocated.error());
            }
            vt.fxaaSet = (*fxaaAllocated)[0];
        }

        // Depth pre-pass: a vertex-only pipeline that reuses the mesh vertex shader.
        Result<Ref<Pipeline>> depthPrepass = makeDepthPrepassPipeline(renderer, "shaders/mesh.spv");
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

        // Editor overlay: screen-space gizmo handles + entity billboards drawn over the
        // tonemapped scene color (so they show under present-only mode). The depth variant
        // occludes camera frustums against the scene depth; the plain one always draws on top.
        Result<Ref<Pipeline>> overlay = newOverlayPipeline(renderer, false);
        if (!overlay)
        {
            return Err(overlay.error());
        }
        renderer.pipelines.overlay = *overlay;

        Result<Ref<Pipeline>> overlayDepth = newOverlayPipeline(renderer, true);
        if (!overlayDepth)
        {
            return Err(overlayDepth.error());
        }
        renderer.pipelines.overlayDepth = *overlayDepth;

        Result<Ref<Pipeline>> grid = newGridPipeline(renderer);
        if (!grid)
        {
            return Err(grid.error());
        }
        renderer.pipelines.grid = *grid;

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

        // Bindings 0-2 are the global IBL (irradiance/prefiltered/brdf); bindings 3-5 carry the
        // reflection probes (prefiltered-cube array + irradiance-cube array + metadata SSBO) so
        // probes ride the always-present IBL set instead of a 9th bound set that would exceed
        // maxBoundDescriptorSets.
        std::array<vk::DescriptorSetLayoutBinding, 6> iblBindings{};
        for (u32 b = 0; b < 3; b = b + 1)
        {
            iblBindings[b].binding = b;
            iblBindings[b].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            iblBindings[b].descriptorCount = 1;
            iblBindings[b].stageFlags = vk::ShaderStageFlagBits::eFragment;
        }
        iblBindings[3].binding = 3;
        iblBindings[3].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        iblBindings[3].descriptorCount = MaxReflectionProbes;
        iblBindings[3].stageFlags = vk::ShaderStageFlagBits::eFragment;
        iblBindings[4].binding = 4;
        iblBindings[4].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        iblBindings[4].descriptorCount = MaxReflectionProbes;
        iblBindings[4].stageFlags = vk::ShaderStageFlagBits::eFragment;
        iblBindings[5].binding = 5;
        iblBindings[5].descriptorType = vk::DescriptorType::eStorageBuffer;
        iblBindings[5].descriptorCount = 1;
        iblBindings[5].stageFlags = vk::ShaderStageFlagBits::eFragment;
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

        // Reflection probes share the IBL set (bindings 3-5): a prefiltered-cube array + an
        // irradiance-cube array (MaxReflectionProbes each) + the probe-metadata SSBO. The array
        // slots are seeded with the global IBL cubes after the first bake so the bind is always
        // valid; real probes overwrite their slot on capture. The sampler matches the IBL one.
        vk::SamplerCreateInfo probeSamplerInfo{};
        probeSamplerInfo.magFilter = vk::Filter::eLinear;
        probeSamplerInfo.minFilter = vk::Filter::eLinear;
        probeSamplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
        probeSamplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        probeSamplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        probeSamplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        probeSamplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        auto probeSampler = checked(renderer.context.device.createSampler(probeSamplerInfo), "createSampler (probe)");
        if (!probeSampler)
        {
            return Err(probeSampler.error());
        }
        renderer.reflection.sampler = *probeSampler;
        renderer.reflection.meshSet = renderer.ibl.set;

        auto probeMeta = makeRtBuffer(renderer, sizeof(ProbeMetaGpu) * MaxReflectionProbes,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        if (!probeMeta)
        {
            return Err(probeMeta.error());
        }
        renderer.reflection.metaBuffer = *probeMeta;
        std::memset(renderer.reflection.metaBuffer->mapped, 0, sizeof(ProbeMetaGpu) * MaxReflectionProbes);

        // Sky set (set 1 in the sky pipeline): the procedural environment cube the visible-sky
        // pass samples. Layout + set created here so makeSkyPipeline can reference the layout;
        // bakeEnvironment writes the descriptor once the envCube is filled. The sky reuses the
        // IBL sampler (linear/clamp/mipped) for the cube.
        vk::DescriptorSetLayoutBinding skyBinding{};
        skyBinding.binding = 0;
        skyBinding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        skyBinding.descriptorCount = 1;
        skyBinding.stageFlags = vk::ShaderStageFlagBits::eFragment;
        vk::DescriptorSetLayoutCreateInfo skyLayoutInfo{};
        skyLayoutInfo.setBindings(skyBinding);
        auto skyLayout = checked(renderer.context.device.createDescriptorSetLayout(skyLayoutInfo), "skySetLayout");
        if (!skyLayout)
        {
            return Err(skyLayout.error());
        }
        renderer.sky.setLayout = *skyLayout;
        vk::DescriptorSetAllocateInfo skyAlloc{};
        skyAlloc.descriptorPool = renderer.descriptors.descriptorPool;
        skyAlloc.setSetLayouts(renderer.sky.setLayout);
        auto skySet = checked(renderer.context.device.allocateDescriptorSets(skyAlloc), "allocate skySet");
        if (!skySet)
        {
            return Err(skySet.error());
        }
        renderer.sky.set = (*skySet)[0];
        auto skyPipe = makeSkyPipeline(renderer);
        if (!skyPipe)
        {
            return Err(skyPipe.error());
        }
        renderer.sky.pipeline = *skyPipe;

        // Screen-space effects (GTAO + denoise, contact shadows, SSGI). A nearest sampler
        // for the G-buffer; two shared compute set layouts (2-binding sampler+storage,
        // 3-binding sampler+sampler+storage); a 3-binding mesh set (set 4: AO/contact/SSGI);
        // the five compute sets + the mesh set; the pipelines; and the viewport targets.
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

        auto makeComputeLayout = [&](u32 samplerCount, u32 storageCount,
                                     const char* name) -> Result<vk::DescriptorSetLayout>
        {
            std::vector<vk::DescriptorSetLayoutBinding> bindings;
            for (u32 b = 0; b < samplerCount; b = b + 1)
            {
                vk::DescriptorSetLayoutBinding bd{};
                bd.binding = b;
                bd.descriptorType = vk::DescriptorType::eCombinedImageSampler;
                bd.descriptorCount = 1;
                bd.stageFlags = vk::ShaderStageFlagBits::eCompute;
                bindings.push_back(bd);
            }
            for (u32 b = 0; b < storageCount; b = b + 1)
            {
                vk::DescriptorSetLayoutBinding bd{};
                bd.binding = samplerCount + b;
                bd.descriptorType = vk::DescriptorType::eStorageImage;
                bd.descriptorCount = 1;
                bd.stageFlags = vk::ShaderStageFlagBits::eCompute;
                bindings.push_back(bd);
            }
            vk::DescriptorSetLayoutCreateInfo info{};
            info.setBindings(bindings);
            return checked(renderer.context.device.createDescriptorSetLayout(info), name);
        };
        auto c2 = makeComputeLayout(1, 1, "compute2Layout");
        if (!c2)
        {
            return Err(c2.error());
        }
        renderer.ssao.compute2Layout = *c2;
        auto c3 = makeComputeLayout(2, 1, "compute3Layout");
        if (!c3)
        {
            return Err(c3.error());
        }
        renderer.ssao.compute3Layout = *c3;

        std::array<vk::DescriptorSetLayoutBinding, 3> meshAoBindings{};
        for (u32 b = 0; b < 3; b = b + 1)
        {
            meshAoBindings[b].binding = b;  // 0 = AO, 1 = contact, 2 = SSGI
            meshAoBindings[b].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            meshAoBindings[b].descriptorCount = 1;
            meshAoBindings[b].stageFlags = vk::ShaderStageFlagBits::eFragment;
        }
        vk::DescriptorSetLayoutCreateInfo meshAoLayoutInfo{};
        meshAoLayoutInfo.setBindings(meshAoBindings);
        auto meshAoLayout =
            checked(renderer.context.device.createDescriptorSetLayout(meshAoLayoutInfo), "meshAoSetLayout");
        if (!meshAoLayout)
        {
            return Err(meshAoLayout.error());
        }
        renderer.ssao.meshSetLayout = *meshAoLayout;

        auto allocSet = [&](vk::DescriptorSetLayout layout, const char* name) -> Result<vk::DescriptorSet>
        {
            vk::DescriptorSetAllocateInfo ai{};
            ai.descriptorPool = renderer.descriptors.descriptorPool;
            ai.setSetLayouts(layout);
            auto s = checked(renderer.context.device.allocateDescriptorSets(ai), name);
            if (!s)
            {
                return Err(s.error());
            }
            return (*s)[0];
        };
        // The screen-space compute sets are per-view (they bind per-view ViewTargets images); allocate
        // one set of each for every view. The active (Scene) view's bindings are written by the
        // recreateSsaoTargets call below; the others are written on their first activate-resize.
        for (ViewTargets& vt : renderer.views)
        {
            auto gtaoSet = allocSet(renderer.ssao.compute2Layout, "gtaoSet");
            if (!gtaoSet)
            {
                return Err(gtaoSet.error());
            }
            vt.gtaoSet = *gtaoSet;
            auto aoBlurSet = allocSet(renderer.ssao.compute3Layout, "aoBlurSet");
            if (!aoBlurSet)
            {
                return Err(aoBlurSet.error());
            }
            vt.aoBlurSet = *aoBlurSet;
            auto contactSet = allocSet(renderer.ssao.compute2Layout, "contactSet");
            if (!contactSet)
            {
                return Err(contactSet.error());
            }
            vt.contactSet = *contactSet;
            auto ssgiSet = allocSet(renderer.ssao.compute3Layout, "ssgiSet");
            if (!ssgiSet)
            {
                return Err(ssgiSet.error());
            }
            vt.ssgiSet = *ssgiSet;
            auto ssgiBlurSet = allocSet(renderer.ssao.compute3Layout, "ssgiBlurSet");
            if (!ssgiBlurSet)
            {
                return Err(ssgiBlurSet.error());
            }
            vt.ssgiBlurSet = *ssgiBlurSet;
            auto copyColorSet = allocSet(renderer.ssao.compute2Layout, "copyColorSet");
            if (!copyColorSet)
            {
                return Err(copyColorSet.error());
            }
            vt.copyColorSet = *copyColorSet;
            auto meshSet = allocSet(renderer.ssao.meshSetLayout, "meshAoSet");
            if (!meshSet)
            {
                return Err(meshSet.error());
            }
            vt.meshSet = *meshSet;
        }

        Result<Ref<Pipeline>> gbufferPipe = makeGbufferPipeline(renderer, "shaders/gbuffer.spv");
        if (!gbufferPipe)
        {
            return Err(gbufferPipe.error());
        }
        renderer.pipelines.gbuffer = *gbufferPipe;
        // gtao + ssgi + contact use a mat4x2 + vec4 push (160B); ao_blur + copy_color none.
        const u32 bigPush = static_cast<u32>(2 * sizeof(glm::mat4) + sizeof(glm::vec4) + sizeof(glm::vec4));
        auto gtaoPipe = newComputePipeline(renderer, "shaders/gtao.spv", renderer.ssao.compute2Layout,
                                           static_cast<u32>(sizeof(glm::mat4) + sizeof(glm::vec4)));
        if (!gtaoPipe)
        {
            return Err(gtaoPipe.error());
        }
        renderer.pipelines.gtao = *gtaoPipe;
        auto aoBlurPipe = newComputePipeline(renderer, "shaders/ao_blur.spv", renderer.ssao.compute3Layout);
        if (!aoBlurPipe)
        {
            return Err(aoBlurPipe.error());
        }
        renderer.pipelines.aoBlur = *aoBlurPipe;
        auto contactPipe = newComputePipeline(renderer, "shaders/contact.spv", renderer.ssao.compute2Layout, bigPush);
        if (!contactPipe)
        {
            return Err(contactPipe.error());
        }
        renderer.pipelines.contact = *contactPipe;
        auto ssgiPipe = newComputePipeline(renderer, "shaders/ssgi.spv", renderer.ssao.compute3Layout, bigPush);
        if (!ssgiPipe)
        {
            return Err(ssgiPipe.error());
        }
        renderer.pipelines.ssgi = *ssgiPipe;
        auto ssgiBlurPipe = newComputePipeline(renderer, "shaders/ssgi_blur.spv", renderer.ssao.compute3Layout);
        if (!ssgiBlurPipe)
        {
            return Err(ssgiBlurPipe.error());
        }
        renderer.pipelines.ssgiBlur = *ssgiBlurPipe;
        auto copyColorPipe = newComputePipeline(renderer, "shaders/copy_color.spv", renderer.ssao.compute2Layout);
        if (!copyColorPipe)
        {
            return Err(copyColorPipe.error());
        }
        renderer.pipelines.copyColor = *copyColorPipe;

        recreateSsaoTargets(renderer);

        // TAA: the resolve compute set (3 samplers: current/history/motion + 2 storage:
        // offscreen out + history out), two parities for ping-pong, the motion + resolve
        // pipelines, and the targets.
        std::array<vk::DescriptorSetLayoutBinding, 5> taaBindings{};
        for (u32 b = 0; b < 3; b = b + 1)
        {
            taaBindings[b].binding = b;
            taaBindings[b].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            taaBindings[b].descriptorCount = 1;
            taaBindings[b].stageFlags = vk::ShaderStageFlagBits::eCompute;
        }
        for (u32 b = 3; b < 5; b = b + 1)
        {
            taaBindings[b].binding = b;
            taaBindings[b].descriptorType = vk::DescriptorType::eStorageImage;
            taaBindings[b].descriptorCount = 1;
            taaBindings[b].stageFlags = vk::ShaderStageFlagBits::eCompute;
        }
        vk::DescriptorSetLayoutCreateInfo taaLayoutInfo{};
        taaLayoutInfo.setBindings(taaBindings);
        auto taaLayout = checked(renderer.context.device.createDescriptorSetLayout(taaLayoutInfo), "taaSetLayout");
        if (!taaLayout)
        {
            return Err(taaLayout.error());
        }
        renderer.descriptors.taaSetLayout = *taaLayout;
        // Two TAA ping-pong-parity sets PER VIEW (they bind per-view scratch/history/motion/offscreen);
        // the active view's are written by recreateTaaTargets below, the others on their first resize.
        for (ViewTargets& vt : renderer.views)
        {
            for (u32 p = 0; p < 2; p = p + 1)
            {
                vk::DescriptorSetAllocateInfo taaAlloc{};
                taaAlloc.descriptorPool = renderer.descriptors.descriptorPool;
                taaAlloc.setSetLayouts(renderer.descriptors.taaSetLayout);
                auto taaSet = checked(renderer.context.device.allocateDescriptorSets(taaAlloc), "allocate taaSet");
                if (!taaSet)
                {
                    return Err(taaSet.error());
                }
                vt.taaSets[p] = (*taaSet)[0];
                // SSGI accumulation reuses the TAA set shape (3 samplers + 2 storage); same
                // ping-pong parity, written alongside the TAA sets in recreateTaaTargets.
                vk::DescriptorSetAllocateInfo ssgiAlloc{};
                ssgiAlloc.descriptorPool = renderer.descriptors.descriptorPool;
                ssgiAlloc.setSetLayouts(renderer.descriptors.taaSetLayout);
                auto ssgiAccumSet =
                    checked(renderer.context.device.allocateDescriptorSets(ssgiAlloc), "allocate ssgiAccumSet");
                if (!ssgiAccumSet)
                {
                    return Err(ssgiAccumSet.error());
                }
                vt.ssgiAccumSets[p] = (*ssgiAccumSet)[0];
            }
        }

        Result<Ref<Pipeline>> motionPipe = makeMotionPipeline(renderer, "shaders/motion.spv");
        if (!motionPipe)
        {
            return Err(motionPipe.error());
        }
        renderer.pipelines.motion = *motionPipe;

        Result<Ref<Pipeline>> taaPipe = newComputePipeline(
            renderer, "shaders/taa.spv", renderer.descriptors.taaSetLayout, static_cast<u32>(sizeof(glm::vec4)));
        if (!taaPipe)
        {
            return Err(taaPipe.error());
        }
        renderer.pipelines.taa = *taaPipe;

        Result<Ref<Pipeline>> ssgiAccumPipe = newComputePipeline(
            renderer, "shaders/ssgi_accum.spv", renderer.descriptors.taaSetLayout, static_cast<u32>(sizeof(glm::vec4)));
        if (!ssgiAccumPipe)
        {
            return Err(ssgiAccumPipe.error());
        }
        renderer.pipelines.ssgiAccum = *ssgiAccumPipe;

        recreateTaaTargets(renderer);

        // DDGI: probe atlases + voxel proxy + ray image + box SSBO + 6 sets + 5 PSOs.
        const u32 probeTotal = DdgiProbesX * DdgiProbesY * DdgiProbesZ;
        const u32 irrTile = DdgiIrrInterior + 2;
        const u32 distTile = DdgiDistInterior + 2;
        const u32 irrW = DdgiProbesX * DdgiProbesY * irrTile;
        const u32 irrH = DdgiProbesZ * irrTile;
        const u32 distW = DdgiProbesX * DdgiProbesY * distTile;
        const u32 distH = DdgiProbesZ * distTile;

        auto voxels = newImage3D(renderer, DdgiVoxelRes, DdgiVoxelRes, DdgiVoxelRes, DdgiVoxelFormat);
        if (!voxels)
        {
            return Err(voxels.error());
        }
        renderer.ddgi.voxels = std::move(*voxels);
        auto irr = newColorImage(renderer, irrW, irrH, DdgiIrrFormat, true);
        if (!irr)
        {
            return Err(irr.error());
        }
        renderer.ddgi.irradiance = std::move(*irr);
        auto dist = newColorImage(renderer, distW, distH, DdgiDistFormat, true);
        if (!dist)
        {
            return Err(dist.error());
        }
        renderer.ddgi.distance = std::move(*dist);
        auto rays = newColorImage(renderer, DdgiRaysPerProbe, probeTotal, DdgiVoxelFormat, true);
        if (!rays)
        {
            return Err(rays.error());
        }
        renderer.ddgi.rays = std::move(*rays);
        auto boxBuf =
            makeMappedStorageBuffer(renderer, static_cast<vk::DeviceSize>(DdgiMaxBoxes) * 3 * sizeof(glm::vec4));
        if (!boxBuf)
        {
            return Err(boxBuf.error());
        }
        renderer.ddgi.boxBuffer = *boxBuf;
        renderer.ddgi.boxCapacity = DdgiMaxBoxes;

        vk::SamplerCreateInfo ddgiSamplerInfo{};
        ddgiSamplerInfo.magFilter = vk::Filter::eLinear;
        ddgiSamplerInfo.minFilter = vk::Filter::eLinear;
        ddgiSamplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
        ddgiSamplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        ddgiSamplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        ddgiSamplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        auto ddgiSampler = checked(renderer.context.device.createSampler(ddgiSamplerInfo), "createSampler (ddgi)");
        if (!ddgiSampler)
        {
            return Err(ddgiSampler.error());
        }
        renderer.ddgi.sampler = *ddgiSampler;

        // Layout builder: ordered (type) list of compute (or fragment) bindings.
        auto makeLayout = [&](std::vector<vk::DescriptorType> types, vk::ShaderStageFlags stage,
                              const char* name) -> Result<vk::DescriptorSetLayout>
        {
            std::vector<vk::DescriptorSetLayoutBinding> b;
            for (u32 i = 0; i < types.size(); i = i + 1)
            {
                vk::DescriptorSetLayoutBinding bd{};
                bd.binding = i;
                bd.descriptorType = types[i];
                bd.descriptorCount = 1;
                bd.stageFlags = stage;
                b.push_back(bd);
            }
            vk::DescriptorSetLayoutCreateInfo info{};
            info.setBindings(b);
            return checked(renderer.context.device.createDescriptorSetLayout(info), name);
        };
        const auto SI = vk::DescriptorType::eStorageImage;
        const auto CS = vk::DescriptorType::eCombinedImageSampler;
        const auto SB = vk::DescriptorType::eStorageBuffer;
        const auto comp = vk::ShaderStageFlagBits::eCompute;
        auto voxL = makeLayout({ SI, SB }, comp, "ddgiVoxelLayout");
        if (!voxL)
        {
            return Err(voxL.error());
        }
        renderer.ddgi.voxelLayout = *voxL;
        auto trL = makeLayout({ SI, CS, SI }, comp, "ddgiTraceLayout");
        if (!trL)
        {
            return Err(trL.error());
        }
        renderer.ddgi.traceLayout = *trL;
        auto biL = makeLayout({ CS, SI }, comp, "ddgiBlendIrrLayout");
        if (!biL)
        {
            return Err(biL.error());
        }
        renderer.ddgi.blendIrrLayout = *biL;
        auto bdL = makeLayout({ CS, SI }, comp, "ddgiBlendDistLayout");
        if (!bdL)
        {
            return Err(bdL.error());
        }
        renderer.ddgi.blendDistLayout = *bdL;
        auto boL = makeLayout({ SI }, comp, "ddgiBorderLayout");
        if (!boL)
        {
            return Err(boL.error());
        }
        renderer.ddgi.borderLayout = *boL;
        auto meL = makeLayout({ CS, CS }, vk::ShaderStageFlagBits::eFragment, "ddgiMeshLayout");
        if (!meL)
        {
            return Err(meL.error());
        }
        renderer.ddgi.meshLayout = *meL;

        auto allocOne = [&](vk::DescriptorSetLayout layout, const char* name) -> Result<vk::DescriptorSet>
        {
            vk::DescriptorSetAllocateInfo ai{};
            ai.descriptorPool = renderer.descriptors.descriptorPool;
            ai.setSetLayouts(layout);
            auto s = checked(renderer.context.device.allocateDescriptorSets(ai), name);
            if (!s)
            {
                return Err(s.error());
            }
            return (*s)[0];
        };
        auto vs = allocOne(renderer.ddgi.voxelLayout, "ddgiVoxelSet");
        if (!vs)
        {
            return Err(vs.error());
        }
        renderer.ddgi.voxelSet = *vs;
        auto ts = allocOne(renderer.ddgi.traceLayout, "ddgiTraceSet");
        if (!ts)
        {
            return Err(ts.error());
        }
        renderer.ddgi.traceSet = *ts;
        auto bis = allocOne(renderer.ddgi.blendIrrLayout, "ddgiBlendIrr");
        if (!bis)
        {
            return Err(bis.error());
        }
        renderer.ddgi.blendIrrSet = *bis;
        auto bds = allocOne(renderer.ddgi.blendDistLayout, "ddgiBlendDist");
        if (!bds)
        {
            return Err(bds.error());
        }
        renderer.ddgi.blendDistSet = *bds;
        auto bos = allocOne(renderer.ddgi.borderLayout, "ddgiBorder");
        if (!bos)
        {
            return Err(bos.error());
        }
        renderer.ddgi.borderSet = *bos;
        auto mes = allocOne(renderer.ddgi.meshLayout, "ddgiMeshSet");
        if (!mes)
        {
            return Err(mes.error());
        }
        renderer.ddgi.meshSet = *mes;

        // Static descriptor writes (the images/buffer never reallocate after init).
        auto wImg = [&](vk::DescriptorSet set, u32 binding, vk::DescriptorType type, vk::ImageView v, vk::ImageLayout l,
                        vk::Sampler s)
        {
            vk::DescriptorImageInfo ii{ s, v, l };
            vk::WriteDescriptorSet w{};
            w.dstSet = set;
            w.dstBinding = binding;
            w.descriptorType = type;
            w.setImageInfo(ii);
            renderer.context.device.updateDescriptorSets(w, {});
        };
        const vk::ImageLayout GEN = vk::ImageLayout::eGeneral;
        const vk::ImageLayout RO = vk::ImageLayout::eShaderReadOnlyOptimal;
        // voxelize: voxel storage + box SSBO
        wImg(renderer.ddgi.voxelSet, 0, SI, renderer.ddgi.voxels.view, GEN, nullptr);
        {
            vk::DescriptorBufferInfo bi{ renderer.ddgi.boxBuffer->buffer, 0, renderer.ddgi.boxBuffer->size };
            vk::WriteDescriptorSet w{};
            w.dstSet = renderer.ddgi.voxelSet;
            w.dstBinding = 1;
            w.descriptorType = SB;
            w.setBufferInfo(bi);
            renderer.context.device.updateDescriptorSets(w, {});
        }
        // trace: voxel storage (read) + irradiance sampler (prev) + ray storage
        wImg(renderer.ddgi.traceSet, 0, SI, renderer.ddgi.voxels.view, GEN, nullptr);
        wImg(renderer.ddgi.traceSet, 1, CS, renderer.ddgi.irradiance.view, RO, renderer.ddgi.sampler);
        wImg(renderer.ddgi.traceSet, 2, SI, renderer.ddgi.rays.view, GEN, nullptr);
        // blend irradiance: ray sampler + irradiance storage
        wImg(renderer.ddgi.blendIrrSet, 0, CS, renderer.ddgi.rays.view, RO, renderer.ddgi.sampler);
        wImg(renderer.ddgi.blendIrrSet, 1, SI, renderer.ddgi.irradiance.view, GEN, nullptr);
        // blend distance: ray sampler + distance storage
        wImg(renderer.ddgi.blendDistSet, 0, CS, renderer.ddgi.rays.view, RO, renderer.ddgi.sampler);
        wImg(renderer.ddgi.blendDistSet, 1, SI, renderer.ddgi.distance.view, GEN, nullptr);
        // border: irradiance storage
        wImg(renderer.ddgi.borderSet, 0, SI, renderer.ddgi.irradiance.view, GEN, nullptr);
        // mesh set 5: irradiance + distance samplers
        wImg(renderer.ddgi.meshSet, 0, CS, renderer.ddgi.irradiance.view, RO, renderer.ddgi.sampler);
        wImg(renderer.ddgi.meshSet, 1, CS, renderer.ddgi.distance.view, RO, renderer.ddgi.sampler);

        // Pipelines.
        const u32 voxPush = static_cast<u32>(sizeof(glm::uvec4) + 2 * sizeof(glm::vec4));
        const u32 tracePush = static_cast<u32>(2 * sizeof(glm::uvec4) + 5 * sizeof(glm::vec4));
        const u32 blendPush = static_cast<u32>(2 * sizeof(glm::uvec4) + sizeof(glm::vec4));
        const u32 borderPush = static_cast<u32>(2 * sizeof(glm::uvec4));
        auto p1 = newComputePipeline(renderer, "shaders/ddgi_voxelize.spv", renderer.ddgi.voxelLayout, voxPush);
        if (!p1)
        {
            return Err(p1.error());
        }
        renderer.pipelines.ddgiVoxelize = *p1;
        auto p2 = newComputePipeline(renderer, "shaders/ddgi_trace.spv", renderer.ddgi.traceLayout, tracePush);
        if (!p2)
        {
            return Err(p2.error());
        }
        renderer.pipelines.ddgiTrace = *p2;
        auto p3 =
            newComputePipeline(renderer, "shaders/ddgi_blend_irradiance.spv", renderer.ddgi.blendIrrLayout, blendPush);
        if (!p3)
        {
            return Err(p3.error());
        }
        renderer.pipelines.ddgiBlendIrr = *p3;
        auto p4 =
            newComputePipeline(renderer, "shaders/ddgi_blend_distance.spv", renderer.ddgi.blendDistLayout, blendPush);
        if (!p4)
        {
            return Err(p4.error());
        }
        renderer.pipelines.ddgiBlendDist = *p4;
        auto p5 = newComputePipeline(renderer, "shaders/ddgi_border.spv", renderer.ddgi.borderLayout, borderPush);
        if (!p5)
        {
            return Err(p5.error());
        }
        renderer.pipelines.ddgiBorder = *p5;

        // Initial layouts: atlases + voxel begin in their resting state. Irradiance/distance
        // are sampler-read by the mesh (RO); voxel is storage (GENERAL). One-shot barrier.
        {
            vk::CommandBufferAllocateInfo allocInfo{};
            allocInfo.commandPool = renderer.frame.frames[0].commandPool;
            allocInfo.level = vk::CommandBufferLevel::ePrimary;
            allocInfo.commandBufferCount = 1;
            auto cmds = checked(renderer.context.device.allocateCommandBuffers(allocInfo), "ddgi init cmd");
            if (!cmds)
            {
                return Err(cmds.error());
            }
            vk::CommandBuffer cmd = (*cmds)[0];
            vk::CommandBufferBeginInfo begin{};
            begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
            static_cast<void>(cmd.begin(begin));
            transitionImage(cmd, renderer.ddgi.irradiance.image, vk::ImageLayout::eUndefined, RO,
                            vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                            vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
            transitionImage(cmd, renderer.ddgi.distance.image, vk::ImageLayout::eUndefined, RO,
                            vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                            vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
            renderer.ddgi.irradiance.layout = RO;
            renderer.ddgi.distance.layout = RO;
            static_cast<void>(cmd.end());
            vk::CommandBufferSubmitInfo cmdInfo{};
            cmdInfo.commandBuffer = cmd;
            vk::SubmitInfo2 submitInfo{};
            submitInfo.setCommandBufferInfos(cmdInfo);
            static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, nullptr));
            static_cast<void>(renderer.context.device.waitIdle());
            renderer.context.device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);
        }
        renderer.ddgi.ready = true;

        // Ray tracing: the TLAS descriptor set (set 6 in the mesh pipeline).
        // Only meaningful when RT is supported; the layout always exists so the mesh PSO
        // layout is stable (the set is bound + the shader gates the trace on a UBO flag).
        if (renderer.context.rtSupported)
        {
            vk::DescriptorSetLayoutBinding tlasBinding{};
            tlasBinding.binding = 0;
            tlasBinding.descriptorType = vk::DescriptorType::eAccelerationStructureKHR;
            tlasBinding.descriptorCount = 1;
            tlasBinding.stageFlags = vk::ShaderStageFlagBits::eFragment;
            vk::DescriptorSetLayoutCreateInfo tlasLayoutInfo{};
            tlasLayoutInfo.setBindings(tlasBinding);
            auto tlasLayout =
                checked(renderer.context.device.createDescriptorSetLayout(tlasLayoutInfo), "rtMeshLayout");
            if (!tlasLayout)
            {
                return Err(tlasLayout.error());
            }
            renderer.rt.meshLayout = *tlasLayout;
            for (u32 i = 0; i < MaxFramesInFlight; i = i + 1)
            {
                vk::DescriptorSetAllocateInfo ai{};
                ai.descriptorPool = renderer.descriptors.descriptorPool;
                ai.setSetLayouts(renderer.rt.meshLayout);
                auto s = checked(renderer.context.device.allocateDescriptorSets(ai), "allocate rtMeshSet");
                if (!s)
                {
                    return Err(s.error());
                }
                renderer.rt.meshSets[i] = (*s)[0];
            }

            // Build a minimal empty TLAS (0 instances) so set 6 always holds a VALID
            // acceleration structure — the mesh fragment statically references rtScene even
            // when the runtime ray-query flag is off, so an unwritten descriptor would be a
            // validation error. Per-frame builds overwrite the set with the real TLAS.
            if (Result<void> seeded = seedEmptyTlas(renderer); !seeded)
            {
                return Err(seeded.error());
            }

            // ReSTIR DI (needs ray-query for the resolve visibility ray): a nearest sampler,
            // three compute set layouts + a mesh set layout (set 7), the four sets, and the
            // reservoir buffers + radiance image (sized to the viewport in recreate).
            vk::SamplerCreateInfo rsSampler{};
            rsSampler.magFilter = vk::Filter::eNearest;
            rsSampler.minFilter = vk::Filter::eNearest;
            rsSampler.mipmapMode = vk::SamplerMipmapMode::eNearest;
            rsSampler.addressModeU = vk::SamplerAddressMode::eClampToEdge;
            rsSampler.addressModeV = vk::SamplerAddressMode::eClampToEdge;
            rsSampler.addressModeW = vk::SamplerAddressMode::eClampToEdge;
            auto rss = checked(renderer.context.device.createSampler(rsSampler), "restir sampler");
            if (!rss)
            {
                return Err(rss.error());
            }
            renderer.restir.sampler = *rss;

            // Layout helper: ordered descriptor types (sampler / storage-buffer / storage-image / AS).
            auto rsLayout = [&](std::vector<vk::DescriptorType> types,
                                const char* name) -> Result<vk::DescriptorSetLayout>
            {
                std::vector<vk::DescriptorSetLayoutBinding> b;
                for (u32 i = 0; i < types.size(); i = i + 1)
                {
                    vk::DescriptorSetLayoutBinding bd{};
                    bd.binding = i;
                    bd.descriptorType = types[i];
                    bd.descriptorCount = 1;
                    bd.stageFlags = vk::ShaderStageFlagBits::eCompute;
                    b.push_back(bd);
                }
                vk::DescriptorSetLayoutCreateInfo info{};
                info.setBindings(b);
                return checked(renderer.context.device.createDescriptorSetLayout(info), name);
            };
            const auto CS = vk::DescriptorType::eCombinedImageSampler;
            const auto SB = vk::DescriptorType::eStorageBuffer;
            const auto SI = vk::DescriptorType::eStorageImage;
            const auto AS = vk::DescriptorType::eAccelerationStructureKHR;
            // initial: gbuffer + lightSSBO + clusterSSBO + reservoirOut
            auto il = rsLayout({ CS, SB, SB, SB }, "restirInitialLayout");
            if (!il)
            {
                return Err(il.error());
            }
            renderer.restir.initialLayout = *il;
            // reuse: gbuffer + motion + initial + previous + lights + combined
            auto rl = rsLayout({ CS, CS, SB, SB, SB, SB }, "restirReuseLayout");
            if (!rl)
            {
                return Err(rl.error());
            }
            renderer.restir.reuseLayout = *rl;
            // resolve: gbuffer + combined + previousOut + lights + TLAS + radianceImage
            auto sl = rsLayout({ CS, SB, SB, SB, AS, SI }, "restirResolveLayout");
            if (!sl)
            {
                return Err(sl.error());
            }
            renderer.restir.resolveLayout = *sl;
            // mesh set 7: the radiance sampler (fragment stage)
            vk::DescriptorSetLayoutBinding meshBind{};
            meshBind.binding = 0;
            meshBind.descriptorType = CS;
            meshBind.descriptorCount = 1;
            meshBind.stageFlags = vk::ShaderStageFlagBits::eFragment;
            vk::DescriptorSetLayoutCreateInfo meshLi{};
            meshLi.setBindings(meshBind);
            auto ml = checked(renderer.context.device.createDescriptorSetLayout(meshLi), "restirMeshLayout");
            if (!ml)
            {
                return Err(ml.error());
            }
            renderer.restir.meshLayout = *ml;

            auto rsAlloc = [&](vk::DescriptorSetLayout layout, const char* name) -> Result<vk::DescriptorSet>
            {
                vk::DescriptorSetAllocateInfo ai{};
                ai.descriptorPool = renderer.descriptors.descriptorPool;
                ai.setSetLayouts(layout);
                auto s = checked(renderer.context.device.allocateDescriptorSets(ai), name);
                if (!s)
                {
                    return Err(s.error());
                }
                return (*s)[0];
            };
            // The reservoir-binding sets are per-view (each view's reservoirs differ); the layouts
            // they share. Allocate one set of each per view; recreate writes a view's bindings.
            for (RestirView& rv : renderer.restirViews)
            {
                auto si0 = rsAlloc(renderer.restir.initialLayout, "restirInitialSet");
                if (!si0)
                {
                    return Err(si0.error());
                }
                rv.initialSet = *si0;
                auto si1 = rsAlloc(renderer.restir.reuseLayout, "restirReuseSet");
                if (!si1)
                {
                    return Err(si1.error());
                }
                rv.reuseSet = *si1;
                auto si2 = rsAlloc(renderer.restir.resolveLayout, "restirResolveSet");
                if (!si2)
                {
                    return Err(si2.error());
                }
                rv.resolveSet = *si2;
                auto si3 = rsAlloc(renderer.restir.meshLayout, "restirMeshSet");
                if (!si3)
                {
                    return Err(si3.error());
                }
                rv.meshSet = *si3;
            }

            // Pipelines. initial: invView+invProj+grid+screen+zPlanes; reuse: +params; resolve: +eye.
            const u32 initialPush =
                static_cast<u32>(2 * sizeof(glm::mat4) + 2 * sizeof(glm::uvec4) + sizeof(glm::vec4));
            const u32 reusePush = static_cast<u32>(2 * sizeof(glm::mat4) + sizeof(glm::uvec4) + sizeof(glm::vec4));
            const u32 resolvePush = static_cast<u32>(2 * sizeof(glm::mat4) + sizeof(glm::uvec4) + sizeof(glm::vec4));
            auto rp1 =
                newComputePipeline(renderer, "shaders/restir_initial.spv", renderer.restir.initialLayout, initialPush);
            if (!rp1)
            {
                return Err(rp1.error());
            }
            renderer.pipelines.restirInitial = *rp1;
            auto rp2 = newComputePipeline(renderer, "shaders/restir_reuse.spv", renderer.restir.reuseLayout, reusePush);
            if (!rp2)
            {
                return Err(rp2.error());
            }
            renderer.pipelines.restirReuse = *rp2;
            auto rp3 =
                newComputePipeline(renderer, "shaders/restir_resolve.spv", renderer.restir.resolveLayout, resolvePush);
            if (!rp3)
            {
                return Err(rp3.error());
            }
            renderer.pipelines.restirResolve = *rp3;

            recreateRestirTargets(renderer);
        }

        return {};
    }

    // IBL / reflection-probe / environment baking. Defined in renderer_detail_ibl.cpp.
    void seedReflectionProbeSet(Renderer& renderer);
    auto captureReflectionProbe(Renderer& renderer, ReflectionProbe& probe, u32 slot) -> Result<void>;
    auto bakeEnvironment(Renderer& renderer, const SkygenParams& sky, bool firstBake) -> Result<void>;
}
