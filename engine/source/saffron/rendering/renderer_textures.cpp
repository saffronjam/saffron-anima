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
#include <bit>
#include <cstddef>
#include <cstdint>
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

module Saffron.Rendering;

import Saffron.Core;
import Saffron.Window;
import Saffron.Geometry;
import :Detail;

namespace se
{
    auto uploadSvgIcon(Renderer& renderer, const std::string& svgPath, u32 pixelSize, glm::vec4 tint)
        -> Result<Ref<GpuTexture>>
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
        nsvgRasterize(rasterizer, image, 0.0f, 0.0f, scale, rgba.data(), static_cast<int>(pixelSize),
                      static_cast<int>(pixelSize), static_cast<int>(pixelSize) * 4);
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

    // Full mip chain length for a width x height image (down to 1x1).
    static auto mipCount(u32 width, u32 height) -> u32
    {
        u32 levels = 1;
        for (u32 d = (width > height ? width : height); d > 1; d >>= 1)
        {
            levels += 1;
        }
        return levels;
    }

    // One image-memory barrier on a single mip level (sync2).
    static void mipBarrier(vk::CommandBuffer cmd, vk::Image image, u32 mip, vk::ImageLayout from, vk::ImageLayout to,
                           vk::PipelineStageFlags2 srcStage, vk::AccessFlags2 srcAccess,
                           vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess)
    {
        vk::ImageMemoryBarrier2 b{};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.image = image;
        b.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, mip, 1, 0, 1 };
        vk::DependencyInfo dep{};
        dep.setImageMemoryBarriers(b);
        cmd.pipelineBarrier2(dep);
    }

    // Generates mips 1..n-1 by blitting down the chain from mip 0 (which holds the uploaded image), then
    // transitions every level to shader-read. On entry every level is in TransferDstOptimal.
    static void recordMipChain(vk::CommandBuffer cmd, vk::Image image, u32 width, u32 height, u32 mipLevels)
    {
        i32 mw = static_cast<i32>(width);
        i32 mh = static_cast<i32>(height);
        for (u32 i = 1; i < mipLevels; i += 1)
        {
            mipBarrier(cmd, image, i - 1, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                       vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite,
                       vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferRead);
            const i32 nw = mw > 1 ? mw / 2 : 1;
            const i32 nh = mh > 1 ? mh / 2 : 1;
            vk::ImageBlit blit{};
            blit.srcSubresource = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, i - 1, 0, 1 };
            blit.srcOffsets[0] = vk::Offset3D{ 0, 0, 0 };
            blit.srcOffsets[1] = vk::Offset3D{ mw, mh, 1 };
            blit.dstSubresource = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, i, 0, 1 };
            blit.dstOffsets[0] = vk::Offset3D{ 0, 0, 0 };
            blit.dstOffsets[1] = vk::Offset3D{ nw, nh, 1 };
            cmd.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, image, vk::ImageLayout::eTransferDstOptimal,
                          blit, vk::Filter::eLinear);
            mw = nw;
            mh = nh;
        }
        for (u32 i = 0; i < mipLevels; i += 1)
        {
            const bool last = i == mipLevels - 1;
            mipBarrier(cmd, image, i,
                       last ? vk::ImageLayout::eTransferDstOptimal : vk::ImageLayout::eTransferSrcOptimal,
                       vk::ImageLayout::eShaderReadOnlyOptimal,
                       last ? vk::PipelineStageFlagBits2::eCopy : vk::PipelineStageFlagBits2::eBlit,
                       last ? vk::AccessFlagBits2::eTransferWrite : vk::AccessFlagBits2::eTransferRead,
                       vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
        }
    }

    // Reuses a reclaimed bindless slot when one is free, else grows the pool. The returned slot's texture
    // must set GpuTexture::bindlessFreeList so the slot returns here when the texture is destroyed.
    static auto claimBindlessSlot(Renderer& renderer) -> u32
    {
        // Shared with the thumbnail worker's uploads + the free-list return in ~GpuTexture.
        const std::lock_guard<std::mutex> lock(bindlessMutex());
        std::vector<u32>& freeList = *renderer.descriptors.bindlessFreeList;
        if (!freeList.empty())
        {
            const u32 slot = freeList.back();
            freeList.pop_back();
            return slot;
        }
        const u32 slot = renderer.descriptors.nextBindlessIndex;
        renderer.descriptors.nextBindlessIndex += 1;
        return slot;
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
        if (vmaCreateBuffer(renderer.context.allocator, &stagingInfo, &stagingAlloc, &staging, &stagingAllocation,
                            &stagingMapped) != VK_SUCCESS)
        {
            return Err(std::string{ "uploadTexture: staging vmaCreateBuffer failed" });
        }
        std::memcpy(stagingMapped.pMappedData, rgba, bytes);
        vmaFlushAllocation(renderer.context.allocator, stagingAllocation, 0, VK_WHOLE_SIZE);

        const u32 mipLevels = mipCount(width, height);
        const vk::Format format = srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = static_cast<VkFormat>(format);
        imageInfo.extent = VkExtent3D{ width, height, 1 };
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_SRC lets the thumbnail/preview path read the texture back via copyImageToBuffer.
        imageInfo.usage =
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo imageAlloc{};
        imageAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        imageAlloc.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VkImage rawImage = VK_NULL_HANDLE;
        VmaAllocation imageAllocation = nullptr;
        if (vmaCreateImage(renderer.context.allocator, &imageInfo, &imageAlloc, &rawImage, &imageAllocation, nullptr) !=
            VK_SUCCESS)
        {
            vmaDestroyBuffer(renderer.context.allocator, staging, stagingAllocation);
            return Err(std::string{ "uploadTexture: vmaCreateImage failed" });
        }

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = oneOffCommandPool(renderer);
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds =
            checked(renderer.context.device.allocateCommandBuffers(cmdAlloc), "uploadTexture: allocateCommandBuffers");
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
        // All mips start TransferDst: mip 0 receives the copy, the rest receive blits down the chain.
        {
            vk::ImageMemoryBarrier2 toDst{};
            toDst.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
            toDst.dstStageMask = vk::PipelineStageFlagBits2::eCopy;
            toDst.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
            toDst.oldLayout = vk::ImageLayout::eUndefined;
            toDst.newLayout = vk::ImageLayout::eTransferDstOptimal;
            toDst.image = vk::Image{ rawImage };
            toDst.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1 };
            vk::DependencyInfo dep{};
            dep.setImageMemoryBarriers(toDst);
            cmd.pipelineBarrier2(dep);
        }
        vk::BufferImageCopy region{};
        region.imageSubresource = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
        region.imageExtent = vk::Extent3D{ width, height, 1 };
        cmd.copyBufferToImage(vk::Buffer{ staging }, vk::Image{ rawImage }, vk::ImageLayout::eTransferDstOptimal,
                              region);
        recordMipChain(cmd, vk::Image{ rawImage }, width, height, mipLevels);
        static_cast<void>(cmd.end());
        auto submitted = submitAndWait(renderer, cmd);
        renderer.context.device.freeCommandBuffers(oneOffCommandPool(renderer), cmd);
        vmaDestroyBuffer(renderer.context.allocator, staging, stagingAllocation);
        if (!submitted)
        {
            vmaDestroyImage(renderer.context.allocator, rawImage, imageAllocation);
            return Err(submitted.error());
        }

        vk::ImageViewCreateInfo viewInfo{};
        viewInfo.image = vk::Image{ rawImage };
        viewInfo.viewType = vk::ImageViewType::e2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1 };
        auto view = checked(renderer.context.device.createImageView(viewInfo), "uploadTexture: createImageView");
        if (!view)
        {
            vmaDestroyImage(renderer.context.allocator, rawImage, imageAllocation);
            return Err(view.error());
        }

        // Claim a bindless slot (reusing a reclaimed one) and write the texture into the global array.
        const u32 index = claimBindlessSlot(renderer);
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
        texture.bindlessFreeList = renderer.descriptors.bindlessFreeList;
        return std::make_shared<GpuTexture>(std::move(texture));
    }

    // Narrows one finite f32 to an IEEE binary16 (round-to-nearest-even). Subnormals are
    // flushed; finite magnitudes above the f16 max (65504) saturate to +/-inf, matching what
    // the GPU produces sampling an f16 texture.
    static auto floatToHalf(f32 value) -> u16
    {
        u32 bits = std::bit_cast<u32>(value);
        const u32 sign = (bits >> 16) & 0x8000u;
        bits = bits & 0x7fffffffu;
        if (bits >= 0x7f800000u)
        {
            // inf / nan: keep nan non-zero so it stays nan
            u16 mant = static_cast<u16>(0);
            if (bits > 0x7f800000u)
            {
                mant = static_cast<u16>(0x0200u);
            }
            return static_cast<u16>(sign | 0x7c00u | mant);
        }
        if (bits >= 0x47800000u)
        {
            return static_cast<u16>(sign | 0x7c00u);  // overflow -> inf
        }
        if (bits < 0x38800000u)
        {
            // subnormal/zero in f16: round the value scaled into the denormal range
            const u32 mant = (bits & 0x7fffffu) | 0x800000u;
            const int shift = 113 - static_cast<int>(bits >> 23);
            u32 rounded = 0u;
            if (shift < 24)
            {
                rounded = mant >> shift;
            }
            const u32 half = (rounded + 0x00000fffu + ((rounded >> 13) & 1u)) >> 13;
            return static_cast<u16>(sign | half);
        }
        const u32 rebiased = bits + 0xc8000000u;  // exponent rebias (127 -> 15)
        const u32 rounded = (rebiased + 0x00000fffu + ((rebiased >> 13) & 1u)) >> 13;
        return static_cast<u16>(sign | rounded);
    }

    auto uploadTextureFloat(Renderer& renderer, const f32* rgba, u32 width, u32 height) -> Result<Ref<GpuTexture>>
    {
        if (width == 0 || height == 0)
        {
            return Err(std::string{ "uploadTextureFloat: zero-sized image" });
        }
        const std::size_t texels = static_cast<std::size_t>(width) * height * 4;
        std::vector<u16> halfRgba(texels);
        for (std::size_t i = 0; i < texels; i = i + 1)
        {
            halfRgba[i] = floatToHalf(rgba[i]);
        }
        const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(texels) * sizeof(u16);

        VkBufferCreateInfo stagingInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        stagingInfo.size = bytes;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo stagingAlloc{};
        stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = nullptr;
        VmaAllocationInfo stagingMapped{};
        if (vmaCreateBuffer(renderer.context.allocator, &stagingInfo, &stagingAlloc, &staging, &stagingAllocation,
                            &stagingMapped) != VK_SUCCESS)
        {
            return Err(std::string{ "uploadTextureFloat: staging vmaCreateBuffer failed" });
        }
        std::memcpy(stagingMapped.pMappedData, halfRgba.data(), bytes);
        vmaFlushAllocation(renderer.context.allocator, stagingAllocation, 0, VK_WHOLE_SIZE);

        const vk::Format format = vk::Format::eR16G16B16A16Sfloat;
        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = static_cast<VkFormat>(format);
        imageInfo.extent = VkExtent3D{ width, height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_SRC lets the thumbnail/preview path read the texture back via copyImageToBuffer.
        imageInfo.usage =
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo imageAlloc{};
        imageAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        imageAlloc.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VkImage rawImage = VK_NULL_HANDLE;
        VmaAllocation imageAllocation = nullptr;
        if (vmaCreateImage(renderer.context.allocator, &imageInfo, &imageAlloc, &rawImage, &imageAllocation, nullptr) !=
            VK_SUCCESS)
        {
            vmaDestroyBuffer(renderer.context.allocator, staging, stagingAllocation);
            return Err(std::string{ "uploadTextureFloat: vmaCreateImage failed" });
        }

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = oneOffCommandPool(renderer);
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(renderer.context.device.allocateCommandBuffers(cmdAlloc),
                            "uploadTextureFloat: allocateCommandBuffers");
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
        transitionImage(cmd, vk::Image{ rawImage }, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                        vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                        vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
        vk::BufferImageCopy region{};
        region.imageSubresource = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
        region.imageExtent = vk::Extent3D{ width, height, 1 };
        cmd.copyBufferToImage(vk::Buffer{ staging }, vk::Image{ rawImage }, vk::ImageLayout::eTransferDstOptimal,
                              region);
        transitionImage(cmd, vk::Image{ rawImage }, vk::ImageLayout::eTransferDstOptimal,
                        vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eCopy,
                        vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderSampledRead);
        static_cast<void>(cmd.end());
        auto submitted = submitAndWait(renderer, cmd);
        renderer.context.device.freeCommandBuffers(oneOffCommandPool(renderer), cmd);
        vmaDestroyBuffer(renderer.context.allocator, staging, stagingAllocation);
        if (!submitted)
        {
            vmaDestroyImage(renderer.context.allocator, rawImage, imageAllocation);
            return Err(submitted.error());
        }

        vk::ImageViewCreateInfo viewInfo{};
        viewInfo.image = vk::Image{ rawImage };
        viewInfo.viewType = vk::ImageViewType::e2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
        auto view = checked(renderer.context.device.createImageView(viewInfo), "uploadTextureFloat: createImageView");
        if (!view)
        {
            vmaDestroyImage(renderer.context.allocator, rawImage, imageAllocation);
            return Err(view.error());
        }

        const u32 index = claimBindlessSlot(renderer);
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
        texture.bindlessFreeList = renderer.descriptors.bindlessFreeList;
        return std::make_shared<GpuTexture>(std::move(texture));
    }
}
