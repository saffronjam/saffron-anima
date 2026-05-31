module;

#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_NO_SMART_HANDLE
#include <vulkan/vulkan.hpp>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include <stb_image_write.h>
#include <nanosvg.h>
#include <nanosvgrast.h>
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

module Saffron.Rendering;

import Saffron.Core;
import Saffron.Window;
import Saffron.Geometry;
import :Detail;

namespace se
{
    auto uploadSvgIcon(Renderer& renderer, const std::string& svgPath,
                                                              u32 pixelSize, glm::vec4 tint) -> Result<Ref<GpuTexture>>
    {
        std::ifstream in(svgPath);
        if (!in)
        {
            return Err(std::format("cannot open '{}'", svgPath));
        }
        std::string svg((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // nanosvg does not resolve the "currentColor" keyword; map it to white so
        // stroke-only icons (e.g. Lucide) rasterize, then tint below.
        for (std::size_t pos = svg.find("currentColor"); pos != std::string::npos; pos = svg.find("currentColor", pos))
        {
            svg.replace(pos, 12, "#ffffff");
        }

        NSVGimage* image = nsvgParse(svg.data(), "px", 96.0f);  // nsvgParse mutates the buffer
        if (image == nullptr || image->width <= 0.0f || image->height <= 0.0f)
        {
            if (image != nullptr)
            {
                nsvgDelete(image);
            }
            return Err(std::format("nanosvg failed to parse '{}'", svgPath));
        }
        NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
        if (rasterizer == nullptr)
        {
            nsvgDelete(image);
            return Err(std::string{ "nsvgCreateRasterizer failed" });
        }
        const f32 scale = static_cast<f32>(pixelSize) / glm::max(image->width, image->height);
        std::vector<u8> rgba(static_cast<std::size_t>(pixelSize) * pixelSize * 4, 0);
        nsvgRasterize(rasterizer, image, 0.0f, 0.0f, scale, rgba.data(),
                      static_cast<int>(pixelSize), static_cast<int>(pixelSize), static_cast<int>(pixelSize) * 4);
        nsvgDeleteRasterizer(rasterizer);
        nsvgDelete(image);

        for (std::size_t i = 0; i < rgba.size(); i = i + 4)
        {
            rgba[i + 0] = static_cast<u8>(rgba[i + 0] * tint.r);
            rgba[i + 1] = static_cast<u8>(rgba[i + 1] * tint.g);
            rgba[i + 2] = static_cast<u8>(rgba[i + 2] * tint.b);
            rgba[i + 3] = static_cast<u8>(rgba[i + 3] * tint.a);
        }
        return uploadTexture(renderer, rgba.data(), pixelSize, pixelSize, false);
    }
    auto uploadTexture(Renderer& renderer, const u8* rgba, u32 width, u32 height, bool srgb) -> Result<Ref<GpuTexture>>
    {
        if (width == 0 || height == 0)
        {
            return Err(std::string{ "uploadTexture: zero-sized image" });
        }
        const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(width) * height * 4;

        VkBufferCreateInfo stagingInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        stagingInfo.size = bytes;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo stagingAlloc{};
        stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = nullptr;
        VmaAllocationInfo stagingMapped{};
        if (vmaCreateBuffer(renderer.context.allocator, &stagingInfo, &stagingAlloc, &staging, &stagingAllocation, &stagingMapped) != VK_SUCCESS)
        {
            return Err(std::string{ "uploadTexture: staging vmaCreateBuffer failed" });
        }
        std::memcpy(stagingMapped.pMappedData, rgba, bytes);
        vmaFlushAllocation(renderer.context.allocator, stagingAllocation, 0, VK_WHOLE_SIZE);

        const vk::Format format = srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = static_cast<VkFormat>(format);
        imageInfo.extent = VkExtent3D{ width, height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo imageAlloc{};
        imageAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        imageAlloc.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VkImage rawImage = VK_NULL_HANDLE;
        VmaAllocation imageAllocation = nullptr;
        if (vmaCreateImage(renderer.context.allocator, &imageInfo, &imageAlloc, &rawImage, &imageAllocation, nullptr) != VK_SUCCESS)
        {
            vmaDestroyBuffer(renderer.context.allocator, staging, stagingAllocation);
            return Err(std::string{ "uploadTexture: vmaCreateImage failed" });
        }

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = renderer.frame.frames[0].commandPool;
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(renderer.context.device.allocateCommandBuffers(cmdAlloc), "uploadTexture: allocateCommandBuffers");
        if (!cmds)
        {
            vmaDestroyImage(renderer.context.allocator, rawImage, imageAllocation);
            vmaDestroyBuffer(renderer.context.allocator, staging, stagingAllocation);
            return Err(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];
        vk::CommandBufferBeginInfo begin{};
        begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(begin));
        transitionImage(cmd, vk::Image{ rawImage },
            vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
        vk::BufferImageCopy region{};
        region.imageSubresource = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
        region.imageExtent = vk::Extent3D{ width, height, 1 };
        cmd.copyBufferToImage(vk::Buffer{ staging }, vk::Image{ rawImage }, vk::ImageLayout::eTransferDstOptimal, region);
        transitionImage(cmd, vk::Image{ rawImage },
            vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
        static_cast<void>(cmd.end());
        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = cmd;
        vk::SubmitInfo2 submitInfo{};
        submitInfo.setCommandBufferInfos(cmdInfo);
        static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, nullptr));
        static_cast<void>(renderer.context.device.waitIdle());
        renderer.context.device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);
        vmaDestroyBuffer(renderer.context.allocator, staging, stagingAllocation);

        vk::ImageViewCreateInfo viewInfo{};
        viewInfo.image = vk::Image{ rawImage };
        viewInfo.viewType = vk::ImageViewType::e2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
        auto view = checked(renderer.context.device.createImageView(viewInfo), "uploadTexture: createImageView");
        if (!view)
        {
            vmaDestroyImage(renderer.context.allocator, rawImage, imageAllocation);
            return Err(view.error());
        }

        // Claim a bindless slot and write the texture into the global array.
        const u32 index = renderer.descriptors.nextBindlessIndex;
        renderer.descriptors.nextBindlessIndex = renderer.descriptors.nextBindlessIndex + 1;
        writeBindlessTexture(renderer, *view, index);

        GpuTexture texture;
        texture.device = renderer.context.device;
        texture.allocator = renderer.context.allocator;
        texture.image = vk::Image{ rawImage };
        texture.view = *view;
        texture.alloc = imageAllocation;
        texture.extent = vk::Extent2D{ width, height };
        texture.format = format;
        texture.bindlessIndex = index;
        return std::make_shared<GpuTexture>(std::move(texture));
    }
}
