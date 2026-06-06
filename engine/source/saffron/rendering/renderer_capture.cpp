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
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

// POSIX shared memory for the viewport publish segment.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

module Saffron.Rendering;

import Saffron.Core;
import :Detail;

namespace se
{
    auto captureViewport(Renderer& renderer, const std::string& path) -> Result<void>
    {
        Image& img = renderer.targets.offscreen;
        const u32 width = img.extent.width;
        const u32 height = img.extent.height;
        const vk::DeviceSize byteSize = static_cast<vk::DeviceSize>(width) * height * formatPixelBytes(img.format);

        // The offscreen image may still be sampled by an in-flight frame; idle so
        // the capture's layout transition cannot race that read.
        static_cast<void>(renderer.context.device.waitIdle());

        VkBuffer rawBuffer = VK_NULL_HANDLE;
        VmaAllocation bufferAllocation = nullptr;
        VmaAllocationInfo bufferAllocInfo{};
        if (auto created = newHostCaptureBuffer(renderer, byteSize, rawBuffer, bufferAllocation, bufferAllocInfo);
            !created)
        {
            return Err(created.error());
        }

        vk::CommandBufferAllocateInfo allocInfo{};
        allocInfo.commandPool = renderer.frame.frames[0].commandPool;
        allocInfo.level = vk::CommandBufferLevel::ePrimary;
        allocInfo.commandBufferCount = 1;
        auto cmds =
            checked(renderer.context.device.allocateCommandBuffers(allocInfo), "capture: allocateCommandBuffers");
        if (!cmds)
        {
            vmaDestroyBuffer(renderer.context.allocator, rawBuffer, bufferAllocation);
            return Err(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];

        vk::CommandBufferBeginInfo beginInfo{};
        beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(beginInfo));

        // Leave the image in ShaderReadOnly so the next frame's producer barrier holds.
        vk::PipelineStageFlags2 fromStage = vk::PipelineStageFlagBits2::eFragmentShader;
        vk::AccessFlags2 fromAccess = vk::AccessFlagBits2::eShaderSampledRead;
        if (img.layout != vk::ImageLayout::eShaderReadOnlyOptimal)
        {
            fromStage = vk::PipelineStageFlagBits2::eTopOfPipe;
            fromAccess = vk::AccessFlagBits2::eNone;
        }
        captureImageToBuffer(cmd, img.image, img.extent, img.layout, fromStage, fromAccess,
                             vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eFragmentShader,
                             vk::AccessFlagBits2::eShaderSampledRead, vk::Buffer{ rawBuffer });
        img.layout = vk::ImageLayout::eShaderReadOnlyOptimal;

        static_cast<void>(cmd.end());

        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = cmd;
        vk::SubmitInfo2 submitInfo{};
        submitInfo.setCommandBufferInfos(cmdInfo);
        static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, nullptr));
        static_cast<void>(renderer.context.device.waitIdle());
        renderer.context.device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);
        vmaInvalidateAllocation(renderer.context.allocator, bufferAllocation, 0, VK_WHOLE_SIZE);

        auto wrote = writeBufferToPng(static_cast<const unsigned char*>(bufferAllocInfo.pMappedData), width, height,
                                      img.format, path);
        vmaDestroyBuffer(renderer.context.allocator, rawBuffer, bufferAllocation);
        if (!wrote)
        {
            return Err(wrote.error());
        }
        logInfo(std::format("captured viewport ({}x{}) to {}", width, height, path));
        return {};
    }

    auto requestWindowCapture(Renderer& renderer, std::string path) -> Result<void>
    {
        if (!renderer.swapchain.captureSupported)
        {
            return Err(std::string{ "window capture unsupported: surface lacks TRANSFER_SRC usage" });
        }
        renderer.captureNextSwapchainPath = std::move(path);
        return {};
    }

    // Viewport shm publish: a 32-byte header [magic, width, height, seq, ringSlots,
    // slotCapacity, 0, 0] followed by a ring of fixed-capacity BGRA8 frames. The reader
    // checks `seq` around its copy (a seqlock) to skip torn frames.
    namespace
    {
        constexpr u32 ShmMagic = 0x53465632;  // "SFV2"
        constexpr std::size_t ShmHeaderBytes = 32;
        constexpr u32 ShmRingSlots = 4;
    }

    void enableViewportShmPublish(Renderer& renderer, const std::string& shmName)
    {
        renderer.shmPublish.enabled = true;
        renderer.shmPublish.name = shmName;
        logInfo("viewport shm publish enabled (pipelined readback, swapchain present skipped)");
    }

    auto ensureShmPublishSlot(Renderer& renderer, ShmPublishSlot& slot, u32 width, u32 height) -> bool
    {
        if (slot.buffer != VK_NULL_HANDLE && slot.width == width && slot.height == height)
        {
            return true;
        }

        // The caller has waited this slot's fence, so its previous resources are idle.
        if (slot.image)
        {
            vmaDestroyImage(renderer.context.allocator, static_cast<VkImage>(slot.image), slot.imageAlloc);
            slot.image = nullptr;
            slot.imageAlloc = nullptr;
        }
        if (slot.buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(renderer.context.allocator, slot.buffer, slot.bufferAlloc);
            slot.buffer = VK_NULL_HANDLE;
            slot.bufferAlloc = nullptr;
        }
        slot.valid = false;

        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        // BGRA byte order: little-endian WL_SHM_FORMAT_XRGB8888, the wl_shm format every
        // compositor must support.
        imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        imageInfo.extent = VkExtent3D{ width, height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo allocCreate{};
        allocCreate.usage = VMA_MEMORY_USAGE_AUTO;
        VkImage rawImage = VK_NULL_HANDLE;
        if (vmaCreateImage(renderer.context.allocator, &imageInfo, &allocCreate, &rawImage, &slot.imageAlloc,
                           nullptr) != VK_SUCCESS)
        {
            logError("shm publish: vmaCreateImage failed");
            return false;
        }
        slot.image = vk::Image{ rawImage };

        const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(width) * height * 4;
        if (auto created = newHostCaptureBuffer(renderer, bytes, slot.buffer, slot.bufferAlloc, slot.bufferInfo);
            !created)
        {
            logError(created.error());
            vmaDestroyImage(renderer.context.allocator, rawImage, slot.imageAlloc);
            slot.image = nullptr;
            slot.imageAlloc = nullptr;
            return false;
        }
        slot.width = width;
        slot.height = height;
        return true;
    }

    void publishShmPublishSlot(Renderer& renderer, ShmPublishSlot& slot)
    {
        ShmPublish& shm = renderer.shmPublish;
        const std::size_t pixelBytes = static_cast<std::size_t>(slot.width) * slot.height * 4;
        if (pixelBytes == 0 || slot.buffer == VK_NULL_HANDLE || slot.bufferInfo.pMappedData == nullptr)
        {
            return;
        }

        // Grow-only segment: header + a fixed-capacity ring. Capacity follows the largest
        // frame seen, so the reader's mapping stays valid across smaller resizes.
        if (shm.fd < 0 || pixelBytes > shm.slotCapacity)
        {
            const std::size_t capacity = pixelBytes;
            const std::size_t totalBytes = ShmHeaderBytes + static_cast<std::size_t>(ShmRingSlots) * capacity;
            if (shm.base != nullptr)
            {
                munmap(shm.base, shm.mappedSize);
                shm.base = nullptr;
            }
            if (shm.fd >= 0)
            {
                close(shm.fd);
                shm.fd = -1;
            }
            shm_unlink(shm.name.c_str());
            const int fd = shm_open(shm.name.c_str(), O_CREAT | O_RDWR, 0600);
            if (fd < 0)
            {
                return;
            }
            if (ftruncate(fd, static_cast<off_t>(totalBytes)) != 0)
            {
                close(fd);
                return;
            }
            void* base = mmap(nullptr, totalBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (base == MAP_FAILED)
            {
                close(fd);
                return;
            }
            shm.fd = fd;
            shm.base = static_cast<unsigned char*>(base);
            shm.mappedSize = totalBytes;
            shm.slotCapacity = capacity;
            u32* header = reinterpret_cast<u32*>(shm.base);
            header[0] = ShmMagic;
            header[4] = ShmRingSlots;
            header[5] = static_cast<u32>(capacity);
            header[6] = 0;
            header[7] = 0;
        }

        const u32 next = shm.seq + 1;
        unsigned char* dst = shm.base + ShmHeaderBytes + (next % ShmRingSlots) * shm.slotCapacity;
        vmaInvalidateAllocation(renderer.context.allocator, slot.bufferAlloc, 0, VK_WHOLE_SIZE);
        std::memcpy(dst, slot.bufferInfo.pMappedData, pixelBytes);

        // Write dimensions + pixels first, bump seq last (release fence) so a reader that
        // sees the new seq is guaranteed the matching w/h + pixels.
        u32* header = reinterpret_cast<u32*>(shm.base);
        header[1] = slot.width;
        header[2] = slot.height;
        shm.seq = next;
        std::atomic_thread_fence(std::memory_order_release);
        header[3] = next;
    }

    void destroyShmPublish(Renderer& renderer)
    {
        ShmPublish& shm = renderer.shmPublish;
        for (ShmPublishSlot& slot : shm.slots)
        {
            if (slot.image)
            {
                vmaDestroyImage(renderer.context.allocator, static_cast<VkImage>(slot.image), slot.imageAlloc);
                slot.image = nullptr;
                slot.imageAlloc = nullptr;
            }
            if (slot.buffer != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(renderer.context.allocator, slot.buffer, slot.bufferAlloc);
                slot.buffer = VK_NULL_HANDLE;
                slot.bufferAlloc = nullptr;
            }
            slot.valid = false;
        }
        if (shm.base != nullptr)
        {
            munmap(shm.base, shm.mappedSize);
            shm.base = nullptr;
        }
        if (shm.fd >= 0)
        {
            close(shm.fd);
            shm.fd = -1;
        }
        if (shm.enabled && !shm.name.empty())
        {
            shm_unlink(shm.name.c_str());
        }
        shm.slotCapacity = 0;
        shm.enabled = false;
    }
}
