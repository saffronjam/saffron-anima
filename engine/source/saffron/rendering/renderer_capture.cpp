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
import :Detail;

namespace se
{
    auto captureViewport(Renderer& renderer, const std::string& path) -> Result<void>
    {
        Image& img = renderer.offscreenViewport;
        const u32 width = img.extent.width;
        const u32 height = img.extent.height;
        const vk::DeviceSize byteSize = static_cast<vk::DeviceSize>(width) * height * 4;

        // The offscreen image may still be sampled by an in-flight frame; idle so
        // the capture's layout transition cannot race that read.
        static_cast<void>(renderer.device.waitIdle());

        VkBuffer rawBuffer = VK_NULL_HANDLE;
        VmaAllocation bufferAllocation = nullptr;
        VmaAllocationInfo bufferAllocInfo{};
        if (auto created = newHostCaptureBuffer(renderer, byteSize, rawBuffer, bufferAllocation, bufferAllocInfo); !created)
        {
            return Err(created.error());
        }

        vk::CommandBufferAllocateInfo allocInfo{};
        allocInfo.commandPool = renderer.frames[0].commandPool;
        allocInfo.level = vk::CommandBufferLevel::ePrimary;
        allocInfo.commandBufferCount = 1;
        auto cmds = checked(renderer.device.allocateCommandBuffers(allocInfo), "capture: allocateCommandBuffers");
        if (!cmds)
        {
            vmaDestroyBuffer(renderer.allocator, rawBuffer, bufferAllocation);
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
        captureImageToBuffer(
            cmd, img.image, img.extent,
            img.layout, fromStage, fromAccess,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
            vk::Buffer{ rawBuffer });
        img.layout = vk::ImageLayout::eShaderReadOnlyOptimal;

        static_cast<void>(cmd.end());

        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = cmd;
        vk::SubmitInfo2 submitInfo{};
        submitInfo.setCommandBufferInfos(cmdInfo);
        static_cast<void>(renderer.graphicsQueue.submit2(submitInfo, nullptr));
        static_cast<void>(renderer.device.waitIdle());
        renderer.device.freeCommandBuffers(renderer.frames[0].commandPool, cmd);
        vmaInvalidateAllocation(renderer.allocator, bufferAllocation, 0, VK_WHOLE_SIZE);

        auto wrote = writeBufferToPng(
            static_cast<const unsigned char*>(bufferAllocInfo.pMappedData), width, height, img.format, path);
        vmaDestroyBuffer(renderer.allocator, rawBuffer, bufferAllocation);
        if (!wrote)
        {
            return Err(wrote.error());
        }
        logInfo(std::format("captured viewport ({}x{}) to {}", width, height, path));
        return {};
    }

    auto requestWindowCapture(Renderer& renderer, std::string path) -> Result<void>
    {
        if (!renderer.swapchainCaptureSupported)
        {
            return Err(std::string{ "window capture unsupported: surface lacks TRANSFER_SRC usage" });
        }
        renderer.captureNextSwapchainPath = std::move(path);
        return {};
    }
}
