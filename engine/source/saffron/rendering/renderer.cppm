module;

// Vulkan-Hpp in no-exceptions mode: every call returns a result we convert to
// std::expected. We never use vk::raii (it throws). Classic includes (no import std).
#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_NO_SMART_HANDLE
#include <vulkan/vulkan.hpp>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include <stb_image_write.h>
#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module Saffron.Rendering;

import Saffron.Core;
import Saffron.Window;
import Saffron.Geometry;

export namespace se
{
    // Format of the offscreen depth buffer. D32_SFLOAT is universally supported.
    inline constexpr vk::Format DepthFormat = vk::Format::eD32Sfloat;

    // A unit of GPU work recorded into the active command buffer — the deferred
    // submission seam. The backend supplies the command buffer.
    using RenderFn = std::function<void(vk::CommandBuffer)>;

    inline constexpr u32 MaxFramesInFlight = 2;

    struct FrameData
    {
        vk::CommandPool commandPool;
        vk::CommandBuffer commandBuffer;
        vk::Semaphore imageAvailable;
        vk::Fence inFlight;
    };

    // A graphics pipeline that owns its vk handles and frees them on destruction.
    // Move-only: the destructor + move-assignment are resource management. Owned
    // by the Renderer (never crosses to client code), destroyed before the device.
    struct Pipeline
    {
        vk::Device device;  // borrowed
        vk::Pipeline pipeline;
        vk::PipelineLayout layout;

        Pipeline() = default;
        Pipeline(const Pipeline&) = delete;
        Pipeline& operator=(const Pipeline&) = delete;

        Pipeline(Pipeline&& other) noexcept
            : device(other.device), pipeline(other.pipeline), layout(other.layout)
        {
            other.device = nullptr;
            other.pipeline = nullptr;
            other.layout = nullptr;
        }

        Pipeline& operator=(Pipeline&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                device = other.device;
                pipeline = other.pipeline;
                layout = other.layout;
                other.device = nullptr;
                other.pipeline = nullptr;
                other.layout = nullptr;
            }
            return *this;
        }

        ~Pipeline()
        {
            reset();
        }

        void reset()
        {
            if (device)
            {
                if (pipeline)
                {
                    device.destroyPipeline(pipeline);
                }
                if (layout)
                {
                    device.destroyPipelineLayout(layout);
                }
            }
            pipeline = nullptr;
            layout = nullptr;
        }
    };

    // A VMA-allocated image that owns its handle + view + allocation and frees
    // them on destruction. Move-only, like Pipeline. Used for offscreen targets.
    struct Image
    {
        vk::Device device;                 // borrowed
        VmaAllocator allocator = nullptr;  // borrowed
        vk::Image image;
        vk::ImageView view;
        VmaAllocation alloc = nullptr;
        vk::Extent2D extent;
        vk::Format format = vk::Format::eUndefined;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;  // tracked across frames

        Image() = default;
        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;

        Image(Image&& other) noexcept
            : device(other.device), allocator(other.allocator), image(other.image),
              view(other.view), alloc(other.alloc), extent(other.extent),
              format(other.format), layout(other.layout)
        {
            other.device = nullptr;
            other.allocator = nullptr;
            other.image = nullptr;
            other.view = nullptr;
            other.alloc = nullptr;
            other.layout = vk::ImageLayout::eUndefined;
        }

        Image& operator=(Image&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                device = other.device;
                allocator = other.allocator;
                image = other.image;
                view = other.view;
                alloc = other.alloc;
                extent = other.extent;
                format = other.format;
                layout = other.layout;
                other.device = nullptr;
                other.allocator = nullptr;
                other.image = nullptr;
                other.view = nullptr;
                other.alloc = nullptr;
                other.layout = vk::ImageLayout::eUndefined;
            }
            return *this;
        }

        ~Image()
        {
            reset();
        }

        void reset()
        {
            if (device && view)
            {
                device.destroyImageView(view);
            }
            if (allocator != nullptr && image)
            {
                vmaDestroyImage(allocator, static_cast<VkImage>(image), alloc);
            }
            view = nullptr;
            image = nullptr;
            alloc = nullptr;
        }
    };

    // A device-local mesh: vertex + index buffers + the submesh ranges. Move-only
    // like Pipeline/Image; owned by the Renderer and freed before the allocator.
    struct GpuMesh
    {
        VmaAllocator allocator = nullptr;  // borrowed
        vk::Buffer vertexBuffer;
        VmaAllocation vertexAlloc = nullptr;
        vk::Buffer indexBuffer;
        VmaAllocation indexAlloc = nullptr;
        u32 indexCount = 0;
        std::vector<Submesh> submeshes;

        GpuMesh() = default;
        GpuMesh(const GpuMesh&) = delete;
        GpuMesh& operator=(const GpuMesh&) = delete;

        GpuMesh(GpuMesh&& other) noexcept
            : allocator(other.allocator), vertexBuffer(other.vertexBuffer), vertexAlloc(other.vertexAlloc),
              indexBuffer(other.indexBuffer), indexAlloc(other.indexAlloc), indexCount(other.indexCount),
              submeshes(std::move(other.submeshes))
        {
            other.allocator = nullptr;
            other.vertexBuffer = nullptr;
            other.vertexAlloc = nullptr;
            other.indexBuffer = nullptr;
            other.indexAlloc = nullptr;
        }

        GpuMesh& operator=(GpuMesh&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                allocator = other.allocator;
                vertexBuffer = other.vertexBuffer;
                vertexAlloc = other.vertexAlloc;
                indexBuffer = other.indexBuffer;
                indexAlloc = other.indexAlloc;
                indexCount = other.indexCount;
                submeshes = std::move(other.submeshes);
                other.allocator = nullptr;
                other.vertexBuffer = nullptr;
                other.vertexAlloc = nullptr;
                other.indexBuffer = nullptr;
                other.indexAlloc = nullptr;
            }
            return *this;
        }

        ~GpuMesh()
        {
            reset();
        }

        void reset()
        {
            if (allocator != nullptr)
            {
                if (vertexBuffer)
                {
                    vmaDestroyBuffer(allocator, static_cast<VkBuffer>(vertexBuffer), vertexAlloc);
                }
                if (indexBuffer)
                {
                    vmaDestroyBuffer(allocator, static_cast<VkBuffer>(indexBuffer), indexAlloc);
                }
            }
            vertexBuffer = nullptr;
            indexBuffer = nullptr;
            vertexAlloc = nullptr;
            indexAlloc = nullptr;
        }
    };

    struct Renderer
    {
        // vk-bootstrap keeps the bits we need for clean teardown.
        vkb::Instance vkbInstance;
        vkb::Device vkbDevice;

        vk::Instance instance;
        vk::SurfaceKHR surface;
        vk::PhysicalDevice physicalDevice;
        vk::Device device;
        vk::Queue graphicsQueue;
        u32 graphicsQueueFamily = 0;
        VmaAllocator allocator = nullptr;

        vk::SwapchainKHR swapchain;
        vk::Format swapchainFormat = vk::Format::eUndefined;
        vk::Extent2D swapchainExtent;
        bool swapchainCaptureSupported = false;  // surface allows TRANSFER_SRC (window screenshots)
        std::vector<vk::Image> swapchainImages;
        std::vector<vk::ImageView> swapchainImageViews;
        std::vector<vk::Semaphore> renderFinished;  // one per swapchain image
        std::vector<vk::Fence> imagesInFlight;       // borrowed per-frame fence per image

        std::array<FrameData, MaxFramesInFlight> frames{};
        u32 frameIndex = 0;
        u32 imageIndex = 0;

        std::array<f32, 4> clearColor{ 0.05f, 0.06f, 0.08f, 1.0f };
        std::vector<RenderFn> sceneSubmissions;  // replayed into the offscreen pass
        std::vector<RenderFn> uiSubmissions;     // replayed into the swapchain pass
        std::vector<Pipeline> pipelines;         // owned; destroyed before the device
        std::vector<GpuMesh> meshes;             // owned; destroyed before the allocator

        Image offscreenViewport;       // scene render target shown in the Viewport panel
        Image offscreenDepth;          // depth buffer for the scene pass, sized to the viewport
        u32 viewportDesiredWidth = 0;  // requested by the UI panel (applied next frame)
        u32 viewportDesiredHeight = 0;
        u32 viewportGeneration = 0;    // bumped whenever the offscreen image is recreated

        // Pending window screenshot, consumed in endFrame: the swapchain image is
        // only safely owned in-frame, so the copy is deferred there.
        std::optional<std::string> captureNextSwapchainPath;

        Window* window = nullptr;  // borrowed
    };

    std::expected<Renderer, std::string> newRenderer(Window& window);
    void destroyRenderer(Renderer& renderer);

    bool beginFrame(Renderer& renderer);
    void submit(Renderer& renderer, RenderFn fn);    // scene pass (offscreen target)
    void submitUi(Renderer& renderer, RenderFn fn);  // ui pass (swapchain)
    void endFrame(Renderer& renderer);

    // The offscreen Viewport target the editor samples + displays in a panel.
    void setViewportDesiredSize(Renderer& renderer, u32 width, u32 height);
    vk::ImageView viewportImageView(const Renderer& renderer);
    u32 viewportGeneration(const Renderer& renderer);
    u32 viewportWidth(const Renderer& renderer);
    u32 viewportHeight(const Renderer& renderer);

    std::string assetPath(std::string_view relative);

    // Creates a pipeline owned by the renderer; returns its handle (index).
    std::expected<u32, std::string> newTrianglePipeline(Renderer& renderer, std::string_view shaderName);
    void drawTriangle(Renderer& renderer, u32 pipelineHandle);

    // Mesh rendering: a vertex-buffer + depth-tested pipeline, a device-local mesh
    // upload, and a draw recorded via submit() with a model-view-projection.
    std::expected<u32, std::string> newMeshPipeline(Renderer& renderer, std::string_view shaderName);
    std::expected<u32, std::string> uploadMesh(Renderer& renderer, const Mesh& mesh);
    void drawMesh(Renderer& renderer, u32 meshHandle, u32 pipelineHandle, const glm::mat4& mvp);

    // Copies the offscreen viewport image to a PNG. Synchronous (own submit +
    // waitIdle), safe to call between frames.
    std::expected<void, std::string> captureViewport(Renderer& renderer, const std::string& path);

    // Requests a PNG of the next presented frame (written in endFrame). Fails here
    // if the surface lacks TRANSFER_SRC; otherwise returns immediately.
    std::expected<void, std::string> requestWindowCapture(Renderer& renderer, std::string path);
}

namespace se
{
    namespace
    {
        // Converts a Vulkan-Hpp ResultValue to std::expected, checked at the call site.
        template <typename T>
        std::expected<T, std::string> checked(vk::ResultValue<T> rv, std::string_view what)
        {
            if (rv.result != vk::Result::eSuccess)
            {
                return std::unexpected(std::format("{}: {}", what, vk::to_string(rv.result)));
            }
            return std::move(rv.value);
        }

        std::expected<void, std::string> checked(vk::Result result, std::string_view what)
        {
            if (result != vk::Result::eSuccess)
            {
                return std::unexpected(std::format("{}: {}", what, vk::to_string(result)));
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

        std::expected<void, std::string> buildSwapchain(Renderer& renderer, u32 width, u32 height)
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
                return std::unexpected(std::format("swapchain build failed: {}", result.error().message()));
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
                    return std::unexpected(view.error());
                }
                renderer.swapchainImageViews.push_back(*view);
            }

            renderer.renderFinished.clear();
            for (std::size_t i = 0; i < renderer.swapchainImages.size(); i = i + 1)
            {
                auto semaphore = checked(renderer.device.createSemaphore(vk::SemaphoreCreateInfo{}), "createSemaphore");
                if (!semaphore)
                {
                    return std::unexpected(semaphore.error());
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

        std::expected<vk::ShaderModule, std::string> loadShaderModule(vk::Device device, const std::string& path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
            {
                return std::unexpected(std::format("cannot open shader '{}'", path));
            }
            std::streamsize size = file.tellg();
            if (size <= 0 || (size % 4) != 0)
            {
                return std::unexpected(std::format("invalid spir-v size for '{}'", path));
            }
            std::vector<u32> code(static_cast<std::size_t>(size) / 4);
            file.seekg(0);
            file.read(reinterpret_cast<char*>(code.data()), size);

            vk::ShaderModuleCreateInfo info{};
            info.codeSize = static_cast<std::size_t>(size);
            info.pCode = code.data();
            return checked(device.createShaderModule(info), std::format("createShaderModule '{}'", path));
        }

        std::expected<Image, std::string> newColorImage(Renderer& renderer, u32 width, u32 height, vk::Format format)
        {
            vk::FormatProperties props = renderer.physicalDevice.getFormatProperties(format);
            vk::FormatFeatureFlags needed =
                vk::FormatFeatureFlagBits::eColorAttachment | vk::FormatFeatureFlagBits::eSampledImage;
            if ((props.optimalTilingFeatures & needed) != needed)
            {
                return std::unexpected(std::format("format {} cannot be a sampled color attachment", vk::to_string(format)));
            }

            VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = static_cast<VkFormat>(format);
            imageInfo.extent = VkExtent3D{ width, height, 1 };
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

            VkImage rawImage = VK_NULL_HANDLE;
            VmaAllocation allocation = nullptr;
            if (vmaCreateImage(renderer.allocator, &imageInfo, &allocInfo, &rawImage, &allocation, nullptr) != VK_SUCCESS)
            {
                return std::unexpected(std::string{ "vmaCreateImage failed for offscreen target" });
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
                return std::unexpected(std::format("createImageView (offscreen): {}", vk::to_string(view.result)));
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

        std::expected<Image, std::string> newDepthImage(Renderer& renderer, u32 width, u32 height)
        {
            VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = static_cast<VkFormat>(DepthFormat);
            imageInfo.extent = VkExtent3D{ width, height, 1 };
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
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
                return std::unexpected(std::string{ "vmaCreateImage failed for depth target" });
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
                return std::unexpected(std::format("createImageView (depth): {}", vk::to_string(view.result)));
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

        // Host-visible, mapped buffer the caller owns (vmaDestroyBuffer when done).
        std::expected<void, std::string> newHostCaptureBuffer(
            Renderer& renderer, vk::DeviceSize bytes,
            VkBuffer& outBuffer, VmaAllocation& outAlloc, VmaAllocationInfo& outInfo)
        {
            VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bufferInfo.size = bytes;
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            if (vmaCreateBuffer(renderer.allocator, &bufferInfo, &allocInfo, &outBuffer, &outAlloc, &outInfo) != VK_SUCCESS)
            {
                return std::unexpected(std::string{ "capture: vmaCreateBuffer failed" });
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
        std::expected<void, std::string> writeBufferToPng(
            const unsigned char* pixels, u32 width, u32 height, vk::Format format, const std::string& path)
        {
            const bool bgr = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
            std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * height * 3);
            for (u32 i = 0; i < width * height; i = i + 1)
            {
                const u32 r = bgr ? (i * 4 + 2) : (i * 4 + 0);
                const u32 b = bgr ? (i * 4 + 0) : (i * 4 + 2);
                rgb[i * 3 + 0] = pixels[r];
                rgb[i * 3 + 1] = pixels[i * 4 + 1];
                rgb[i * 3 + 2] = pixels[b];
            }
            const int ok = stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height),
                                          3, rgb.data(), static_cast<int>(width) * 3);
            if (ok == 0)
            {
                return std::unexpected(std::format("stbi_write_png failed for '{}'", path));
            }
            return {};
        }
    }

    std::expected<Renderer, std::string> newRenderer(Window& window)
    {
        Renderer renderer;
        renderer.window = &window;

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
        renderer.instance = vk::Instance{ renderer.vkbInstance.instance };

        VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(window.handle, renderer.vkbInstance.instance, nullptr, &rawSurface))
        {
            return std::unexpected(std::format("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError()));
        }
        renderer.surface = vk::SurfaceKHR{ rawSurface };

        VkPhysicalDeviceVulkan11Features features11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        features11.shaderDrawParameters = VK_TRUE;  // Slang SV_VertexID emits the DrawParameters capability

        VkPhysicalDeviceVulkan13Features features13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;

        vkb::PhysicalDeviceSelector selector{ renderer.vkbInstance };
        auto physicalResult = selector
                                  .set_minimum_version(1, 3)
                                  .set_required_features_11(features11)
                                  .set_required_features_13(features13)
                                  .set_surface(rawSurface)
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
        renderer.physicalDevice = vk::PhysicalDevice{ physicalResult.value().physical_device };
        renderer.device = vk::Device{ renderer.vkbDevice.device };

        auto queueResult = renderer.vkbDevice.get_queue(vkb::QueueType::graphics);
        if (!queueResult)
        {
            return std::unexpected(std::format("no graphics queue: {}", queueResult.error().message()));
        }
        renderer.graphicsQueue = vk::Queue{ queueResult.value() };
        renderer.graphicsQueueFamily = renderer.vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.instance = renderer.vkbInstance.instance;
        allocatorInfo.physicalDevice = physicalResult.value().physical_device;
        allocatorInfo.device = renderer.vkbDevice.device;
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

        // Offscreen scene target shown in the editor Viewport panel. Same format
        // as the swapchain so the scene pipelines need no special format.
        auto offscreen = newColorImage(renderer, window.width, window.height, renderer.swapchainFormat);
        if (!offscreen)
        {
            return std::unexpected(offscreen.error());
        }
        renderer.offscreenViewport = std::move(*offscreen);
        renderer.viewportDesiredWidth = window.width;
        renderer.viewportDesiredHeight = window.height;
        renderer.viewportGeneration = 1;

        auto depth = newDepthImage(renderer, window.width, window.height);
        if (!depth)
        {
            return std::unexpected(depth.error());
        }
        renderer.offscreenDepth = std::move(*depth);

        for (FrameData& frame : renderer.frames)
        {
            vk::CommandPoolCreateInfo poolInfo{};
            poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
            poolInfo.queueFamilyIndex = renderer.graphicsQueueFamily;
            auto pool = checked(renderer.device.createCommandPool(poolInfo), "createCommandPool");
            if (!pool)
            {
                return std::unexpected(pool.error());
            }
            frame.commandPool = *pool;

            vk::CommandBufferAllocateInfo allocInfo{};
            allocInfo.commandPool = frame.commandPool;
            allocInfo.level = vk::CommandBufferLevel::ePrimary;
            allocInfo.commandBufferCount = 1;
            auto buffers = checked(renderer.device.allocateCommandBuffers(allocInfo), "allocateCommandBuffers");
            if (!buffers)
            {
                return std::unexpected(buffers.error());
            }
            frame.commandBuffer = (*buffers)[0];

            auto imageAvailable = checked(renderer.device.createSemaphore(vk::SemaphoreCreateInfo{}), "createSemaphore");
            if (!imageAvailable)
            {
                return std::unexpected(imageAvailable.error());
            }
            frame.imageAvailable = *imageAvailable;

            vk::FenceCreateInfo fenceInfo{};
            fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled;
            auto fence = checked(renderer.device.createFence(fenceInfo), "createFence");
            if (!fence)
            {
                return std::unexpected(fence.error());
            }
            frame.inFlight = *fence;
        }

        logInfo(std::format("vulkan ready — gpu '{}', {} swapchain images",
                            renderer.vkbDevice.physical_device.name,
                            renderer.swapchainImages.size()));
        return renderer;
    }

    void destroyRenderer(Renderer& renderer)
    {
        if (renderer.device)
        {
            static_cast<void>(renderer.device.waitIdle());
        }

        renderer.offscreenViewport.reset();  // free before the allocator/device
        renderer.offscreenDepth.reset();
        renderer.meshes.clear();             // RAII frees device buffers before the allocator
        renderer.pipelines.clear();          // RAII frees them while the device is still alive

        for (FrameData& frame : renderer.frames)
        {
            renderer.device.destroyFence(frame.inFlight);
            renderer.device.destroySemaphore(frame.imageAvailable);
            renderer.device.destroyCommandPool(frame.commandPool);
        }

        destroySwapchainResources(renderer);

        if (renderer.allocator != nullptr)
        {
            vmaDestroyAllocator(renderer.allocator);
            renderer.allocator = nullptr;
        }
        if (renderer.surface)
        {
            vkb::destroy_surface(renderer.vkbInstance, static_cast<VkSurfaceKHR>(renderer.surface));
        }
        vkb::destroy_device(renderer.vkbDevice);
        vkb::destroy_instance(renderer.vkbInstance);
    }

    bool beginFrame(Renderer& renderer)
    {
        FrameData& frame = renderer.frames[renderer.frameIndex];

        static_cast<void>(renderer.device.waitForFences(frame.inFlight, VK_TRUE, UINT64_MAX));

        vk::ResultValue<u32> acquire = renderer.device.acquireNextImageKHR(
            renderer.swapchain, UINT64_MAX, frame.imageAvailable, nullptr);
        if (acquire.result == vk::Result::eErrorOutOfDateKHR)
        {
            recreateSwapchain(renderer);
            return false;
        }
        if (acquire.result != vk::Result::eSuccess && acquire.result != vk::Result::eSuboptimalKHR)
        {
            logError(std::format("vkAcquireNextImageKHR failed: {}", vk::to_string(acquire.result)));
            return false;
        }
        renderer.imageIndex = acquire.value;

        // Ensure the previous frame that used THIS image has finished before we
        // reuse the image's renderFinished semaphore.
        if (renderer.imagesInFlight[renderer.imageIndex])
        {
            static_cast<void>(renderer.device.waitForFences(renderer.imagesInFlight[renderer.imageIndex], VK_TRUE, UINT64_MAX));
        }
        renderer.imagesInFlight[renderer.imageIndex] = frame.inFlight;

        // Apply a pending Viewport resize (requested last frame). Single shared
        // target, so a full device idle is required before recreating it.
        if (renderer.viewportDesiredWidth > 0 && renderer.viewportDesiredHeight > 0 &&
            (renderer.viewportDesiredWidth != renderer.offscreenViewport.extent.width ||
             renderer.viewportDesiredHeight != renderer.offscreenViewport.extent.height))
        {
            static_cast<void>(renderer.device.waitIdle());
            auto resized = newColorImage(renderer, renderer.viewportDesiredWidth,
                                         renderer.viewportDesiredHeight, renderer.swapchainFormat);
            if (resized)
            {
                renderer.offscreenViewport = std::move(*resized);
                renderer.viewportGeneration = renderer.viewportGeneration + 1;
                auto resizedDepth = newDepthImage(renderer, renderer.viewportDesiredWidth, renderer.viewportDesiredHeight);
                if (resizedDepth)
                {
                    renderer.offscreenDepth = std::move(*resizedDepth);
                }
                else
                {
                    logError(resizedDepth.error());
                }
            }
            else
            {
                logError(resized.error());
            }
        }

        static_cast<void>(renderer.device.resetFences(frame.inFlight));
        static_cast<void>(frame.commandBuffer.reset());
        renderer.sceneSubmissions.clear();
        renderer.uiSubmissions.clear();

        vk::CommandBufferBeginInfo beginInfo{};
        beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(frame.commandBuffer.begin(beginInfo));

        // Rendering scopes are opened in endFrame: pass 1 (scene → offscreen),
        // pass 2 (ui → swapchain).
        return true;
    }

    void submit(Renderer& renderer, RenderFn fn)
    {
        renderer.sceneSubmissions.push_back(std::move(fn));
    }

    void submitUi(Renderer& renderer, RenderFn fn)
    {
        renderer.uiSubmissions.push_back(std::move(fn));
    }

    void setViewportDesiredSize(Renderer& renderer, u32 width, u32 height)
    {
        renderer.viewportDesiredWidth = width;
        renderer.viewportDesiredHeight = height;
    }

    vk::ImageView viewportImageView(const Renderer& renderer)
    {
        return renderer.offscreenViewport.view;
    }

    u32 viewportGeneration(const Renderer& renderer)
    {
        return renderer.viewportGeneration;
    }

    u32 viewportWidth(const Renderer& renderer)
    {
        return renderer.offscreenViewport.extent.width;
    }

    u32 viewportHeight(const Renderer& renderer)
    {
        return renderer.offscreenViewport.extent.height;
    }

    void endFrame(Renderer& renderer)
    {
        FrameData& frame = renderer.frames[renderer.frameIndex];
        Image& offscreen = renderer.offscreenViewport;

        // Pass 1: scene -> offscreen image. Enter from the image's tracked layout
        // (Undefined on frame 1 / after a recreate; ShaderReadOnly thereafter, the
        // WAR barrier vs last frame's read).
        vk::PipelineStageFlags2 srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
        vk::AccessFlags2 srcAccess = vk::AccessFlagBits2::eNone;
        if (offscreen.layout == vk::ImageLayout::eShaderReadOnlyOptimal)
        {
            srcStage = vk::PipelineStageFlagBits2::eFragmentShader;
            srcAccess = vk::AccessFlagBits2::eShaderSampledRead;
        }
        transitionImage(
            frame.commandBuffer, offscreen.image,
            offscreen.layout, vk::ImageLayout::eColorAttachmentOptimal,
            srcStage, srcAccess,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite);
        offscreen.layout = vk::ImageLayout::eColorAttachmentOptimal;

        // Depth is cleared every frame (loadOp clear), so enter from Undefined.
        Image& depth = renderer.offscreenDepth;
        transitionImage(
            frame.commandBuffer, depth.image,
            vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthAttachmentOptimal,
            vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
            vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
            vk::ImageAspectFlagBits::eDepth);

        vk::RenderingAttachmentInfo sceneColor{};
        sceneColor.imageView = offscreen.view;
        sceneColor.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        sceneColor.loadOp = vk::AttachmentLoadOp::eClear;
        sceneColor.storeOp = vk::AttachmentStoreOp::eStore;
        sceneColor.clearValue = vk::ClearValue{ vk::ClearColorValue{ renderer.clearColor } };

        vk::RenderingAttachmentInfo sceneDepth{};
        sceneDepth.imageView = depth.view;
        sceneDepth.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
        sceneDepth.loadOp = vk::AttachmentLoadOp::eClear;
        sceneDepth.storeOp = vk::AttachmentStoreOp::eDontCare;
        sceneDepth.clearValue = vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } };

        vk::RenderingInfo sceneRendering{};
        sceneRendering.renderArea = vk::Rect2D{ vk::Offset2D{ 0, 0 }, offscreen.extent };
        sceneRendering.layerCount = 1;
        sceneRendering.setColorAttachments(sceneColor);
        sceneRendering.setPDepthAttachment(&sceneDepth);
        frame.commandBuffer.beginRendering(sceneRendering);

        vk::Viewport sceneViewport{ 0.0f, 0.0f,
                                    static_cast<f32>(offscreen.extent.width),
                                    static_cast<f32>(offscreen.extent.height), 0.0f, 1.0f };
        vk::Rect2D sceneScissor{ vk::Offset2D{ 0, 0 }, offscreen.extent };
        frame.commandBuffer.setViewport(0, sceneViewport);
        frame.commandBuffer.setScissor(0, sceneScissor);

        for (RenderFn& fn : renderer.sceneSubmissions)
        {
            fn(frame.commandBuffer);
        }
        frame.commandBuffer.endRendering();

        // Producer → consumer: the offscreen image becomes a sampled texture for ImGui.
        // Must be OUTSIDE any rendering scope.
        transitionImage(
            frame.commandBuffer, offscreen.image,
            vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
            vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
        offscreen.layout = vk::ImageLayout::eShaderReadOnlyOptimal;

        // Pass 2: ImGui -> swapchain image.
        transitionImage(
            frame.commandBuffer, renderer.swapchainImages[renderer.imageIndex],
            vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
            vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite);

        vk::RenderingAttachmentInfo swapColor{};
        swapColor.imageView = renderer.swapchainImageViews[renderer.imageIndex];
        swapColor.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        swapColor.loadOp = vk::AttachmentLoadOp::eClear;
        swapColor.storeOp = vk::AttachmentStoreOp::eStore;
        swapColor.clearValue = vk::ClearValue{ vk::ClearColorValue{ renderer.clearColor } };

        vk::RenderingInfo swapRendering{};
        swapRendering.renderArea = vk::Rect2D{ vk::Offset2D{ 0, 0 }, renderer.swapchainExtent };
        swapRendering.layerCount = 1;
        swapRendering.setColorAttachments(swapColor);
        frame.commandBuffer.beginRendering(swapRendering);

        vk::Viewport swapViewport{ 0.0f, 0.0f,
                                   static_cast<f32>(renderer.swapchainExtent.width),
                                   static_cast<f32>(renderer.swapchainExtent.height), 0.0f, 1.0f };
        vk::Rect2D swapScissor{ vk::Offset2D{ 0, 0 }, renderer.swapchainExtent };
        frame.commandBuffer.setViewport(0, swapViewport);
        frame.commandBuffer.setScissor(0, swapScissor);

        for (RenderFn& fn : renderer.uiSubmissions)
        {
            fn(frame.commandBuffer);
        }
        frame.commandBuffer.endRendering();

        // The swapchain image is only safely owned in-frame, so a pending capture
        // is copied here, between the ImGui pass and present; its COLOR->PRESENT
        // transition is folded into captureImageToBuffer's toLayout.
        VkBuffer captureBuffer = VK_NULL_HANDLE;
        VmaAllocation captureAlloc = nullptr;
        VmaAllocationInfo captureInfo{};
        vk::Extent2D captureExtent{};
        bool doCapture = renderer.captureNextSwapchainPath.has_value();
        if (doCapture)
        {
            captureExtent = renderer.swapchainExtent;
            const vk::DeviceSize bytes =
                static_cast<vk::DeviceSize>(captureExtent.width) * captureExtent.height * 4;
            std::expected<void, std::string> created =
                newHostCaptureBuffer(renderer, bytes, captureBuffer, captureAlloc, captureInfo);
            if (!created)
            {
                logError(created.error());
                doCapture = false;
                renderer.captureNextSwapchainPath.reset();
            }
        }
        if (doCapture)
        {
            captureImageToBuffer(
                frame.commandBuffer, renderer.swapchainImages[renderer.imageIndex], captureExtent,
                vk::ImageLayout::eColorAttachmentOptimal,
                vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                vk::ImageLayout::ePresentSrcKHR,
                vk::PipelineStageFlagBits2::eBottomOfPipe, vk::AccessFlagBits2::eNone,
                vk::Buffer{ captureBuffer });
        }
        else
        {
            transitionImage(
                frame.commandBuffer, renderer.swapchainImages[renderer.imageIndex],
                vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR,
                vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                vk::PipelineStageFlagBits2::eBottomOfPipe, vk::AccessFlagBits2::eNone);
        }

        static_cast<void>(frame.commandBuffer.end());

        vk::Semaphore signalSemaphore = renderer.renderFinished[renderer.imageIndex];

        vk::SemaphoreSubmitInfo waitInfo{};
        waitInfo.semaphore = frame.imageAvailable;
        waitInfo.stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;

        vk::SemaphoreSubmitInfo signalInfo{};
        signalInfo.semaphore = signalSemaphore;
        signalInfo.stageMask = vk::PipelineStageFlagBits2::eAllCommands;

        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = frame.commandBuffer;

        vk::SubmitInfo2 submitInfo{};
        submitInfo.setWaitSemaphoreInfos(waitInfo);
        submitInfo.setCommandBufferInfos(cmdInfo);
        submitInfo.setSignalSemaphoreInfos(signalInfo);
        static_cast<void>(renderer.graphicsQueue.submit2(submitInfo, frame.inFlight));

        vk::PresentInfoKHR presentInfo{};
        presentInfo.setWaitSemaphores(signalSemaphore);
        presentInfo.setSwapchains(renderer.swapchain);
        presentInfo.setImageIndices(renderer.imageIndex);
        vk::Result present = renderer.graphicsQueue.presentKHR(presentInfo);
        if (present == vk::Result::eErrorOutOfDateKHR || present == vk::Result::eSuboptimalKHR)
        {
            recreateSwapchain(renderer);
        }

        // The recorded copy is now submitted; idle so it completed, then write the PNG.
        if (doCapture && captureBuffer != VK_NULL_HANDLE)
        {
            static_cast<void>(renderer.device.waitIdle());
            vmaInvalidateAllocation(renderer.allocator, captureAlloc, 0, VK_WHOLE_SIZE);
            std::expected<void, std::string> wrote = writeBufferToPng(
                static_cast<const unsigned char*>(captureInfo.pMappedData),
                captureExtent.width, captureExtent.height,
                renderer.swapchainFormat, *renderer.captureNextSwapchainPath);
            if (!wrote)
            {
                logError(wrote.error());
            }
            else
            {
                logInfo(std::format("captured window ({}x{}) to {}",
                                    captureExtent.width, captureExtent.height,
                                    *renderer.captureNextSwapchainPath));
            }
            vmaDestroyBuffer(renderer.allocator, captureBuffer, captureAlloc);
            renderer.captureNextSwapchainPath.reset();
        }

        renderer.frameIndex = (renderer.frameIndex + 1) % MaxFramesInFlight;
    }

    std::string assetPath(std::string_view relative)
    {
        const char* base = SDL_GetBasePath();  // SDL3: owned by SDL, do not free
        std::string result;
        if (base != nullptr)
        {
            result = base;
        }
        result.append(relative);
        return result;
    }

    std::expected<u32, std::string> newTrianglePipeline(Renderer& renderer, std::string_view shaderName)
    {
        std::string path = assetPath(shaderName);
        auto moduleResult = loadShaderModule(renderer.device, path);
        if (!moduleResult)
        {
            return std::unexpected(moduleResult.error());
        }
        vk::ShaderModule shaderModule = *moduleResult;

        std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
        stages[0].stage = vk::ShaderStageFlagBits::eVertex;
        stages[0].module = shaderModule;
        stages[0].pName = "vertexMain";
        stages[1].stage = vk::ShaderStageFlagBits::eFragment;
        stages[1].module = shaderModule;
        stages[1].pName = "fragmentMain";

        vk::PipelineVertexInputStateCreateInfo vertexInput{};

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

        vk::PipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.blendEnable = VK_FALSE;
        blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

        vk::PipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.setAttachments(blendAttachment);

        std::array<vk::DynamicState, 2> dynamicStates{ vk::DynamicState::eViewport, vk::DynamicState::eScissor };
        vk::PipelineDynamicStateCreateInfo dynamic{};
        dynamic.setDynamicStates(dynamicStates);

        vk::PipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.setColorAttachmentFormats(renderer.swapchainFormat);

        auto layoutResult = checked(renderer.device.createPipelineLayout(vk::PipelineLayoutCreateInfo{}), "createPipelineLayout");
        if (!layoutResult)
        {
            renderer.device.destroyShaderModule(shaderModule);
            return std::unexpected(layoutResult.error());
        }

        vk::GraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.setStages(stages);
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = *layoutResult;

        vk::ResultValue<vk::Pipeline> created = renderer.device.createGraphicsPipeline(nullptr, pipelineInfo);
        renderer.device.destroyShaderModule(shaderModule);  // baked into the pipeline now
        if (created.result != vk::Result::eSuccess)
        {
            renderer.device.destroyPipelineLayout(*layoutResult);
            return std::unexpected(std::format("createGraphicsPipeline: {}", vk::to_string(created.result)));
        }

        Pipeline pipeline;
        pipeline.device = renderer.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;

        u32 handle = static_cast<u32>(renderer.pipelines.size());
        renderer.pipelines.push_back(std::move(pipeline));
        return handle;
    }

    void drawTriangle(Renderer& renderer, u32 pipelineHandle)
    {
        if (pipelineHandle >= renderer.pipelines.size())
        {
            return;
        }
        vk::Pipeline handle = renderer.pipelines[pipelineHandle].pipeline;
        submit(renderer, [handle](vk::CommandBuffer cmd)
        {
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, handle);
            cmd.draw(3, 1, 0, 0);
        });
    }

    std::expected<u32, std::string> newMeshPipeline(Renderer& renderer, std::string_view shaderName)
    {
        std::string path = assetPath(shaderName);
        auto moduleResult = loadShaderModule(renderer.device, path);
        if (!moduleResult)
        {
            return std::unexpected(moduleResult.error());
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
        raster.cullMode = vk::CullModeFlagBits::eNone;  // enable Back once winding is verified
        raster.frontFace = vk::FrontFace::eCounterClockwise;
        raster.lineWidth = 1.0f;

        vk::PipelineMultisampleStateCreateInfo multisample{};
        multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;

        vk::PipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = vk::CompareOp::eLess;

        vk::PipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.blendEnable = VK_FALSE;
        blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.setAttachments(blendAttachment);

        std::array<vk::DynamicState, 2> dynamicStates{ vk::DynamicState::eViewport, vk::DynamicState::eScissor };
        vk::PipelineDynamicStateCreateInfo dynamic{};
        dynamic.setDynamicStates(dynamicStates);

        vk::PipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.setColorAttachmentFormats(renderer.swapchainFormat);
        renderingInfo.depthAttachmentFormat = DepthFormat;

        vk::PushConstantRange pushConstant{};
        pushConstant.stageFlags = vk::ShaderStageFlagBits::eVertex;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(glm::mat4);

        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult = checked(renderer.device.createPipelineLayout(layoutInfo), "createPipelineLayout (mesh)");
        if (!layoutResult)
        {
            renderer.device.destroyShaderModule(shaderModule);
            return std::unexpected(layoutResult.error());
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

        vk::ResultValue<vk::Pipeline> created = renderer.device.createGraphicsPipeline(nullptr, pipelineInfo);
        renderer.device.destroyShaderModule(shaderModule);
        if (created.result != vk::Result::eSuccess)
        {
            renderer.device.destroyPipelineLayout(*layoutResult);
            return std::unexpected(std::format("createGraphicsPipeline (mesh): {}", vk::to_string(created.result)));
        }

        Pipeline pipeline;
        pipeline.device = renderer.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;
        u32 handle = static_cast<u32>(renderer.pipelines.size());
        renderer.pipelines.push_back(std::move(pipeline));
        return handle;
    }

    std::expected<u32, std::string> uploadMesh(Renderer& renderer, const Mesh& mesh)
    {
        if (mesh.vertices.empty() || mesh.indices.empty())
        {
            return std::unexpected(std::string{ "uploadMesh: empty mesh" });
        }
        const vk::DeviceSize vertexBytes = mesh.vertices.size() * sizeof(Vertex);
        const vk::DeviceSize indexBytes = mesh.indices.size() * sizeof(u32);

        // One staging buffer holds [vertices | indices]; two copies fan it out to
        // device-local vertex + index buffers.
        VkBufferCreateInfo stagingInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        stagingInfo.size = vertexBytes + indexBytes;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo stagingAlloc{};
        stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = nullptr;
        VmaAllocationInfo stagingMapped{};
        if (vmaCreateBuffer(renderer.allocator, &stagingInfo, &stagingAlloc, &staging, &stagingAllocation, &stagingMapped) != VK_SUCCESS)
        {
            return std::unexpected(std::string{ "uploadMesh: staging vmaCreateBuffer failed" });
        }
        std::memcpy(stagingMapped.pMappedData, mesh.vertices.data(), vertexBytes);
        std::memcpy(static_cast<char*>(stagingMapped.pMappedData) + vertexBytes, mesh.indices.data(), indexBytes);
        vmaFlushAllocation(renderer.allocator, stagingAllocation, 0, VK_WHOLE_SIZE);

        auto makeDeviceBuffer = [&](vk::DeviceSize size, VkBufferUsageFlags usage, VkBuffer& outBuffer, VmaAllocation& outAlloc) -> bool
        {
            VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size = size;
            info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo alloc{};
            alloc.usage = VMA_MEMORY_USAGE_AUTO;
            return vmaCreateBuffer(renderer.allocator, &info, &alloc, &outBuffer, &outAlloc, nullptr) == VK_SUCCESS;
        };

        GpuMesh gpu;
        gpu.allocator = renderer.allocator;
        gpu.indexCount = static_cast<u32>(mesh.indices.size());
        gpu.submeshes = mesh.submeshes;

        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VmaAllocation vertexAlloc = nullptr;
        VmaAllocation indexAlloc = nullptr;
        if (!makeDeviceBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertexBuffer, vertexAlloc) ||
            !makeDeviceBuffer(indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexBuffer, indexAlloc))
        {
            if (vertexBuffer != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(renderer.allocator, vertexBuffer, vertexAlloc);
            }
            vmaDestroyBuffer(renderer.allocator, staging, stagingAllocation);
            return std::unexpected(std::string{ "uploadMesh: device vmaCreateBuffer failed" });
        }
        gpu.vertexBuffer = vk::Buffer{ vertexBuffer };
        gpu.vertexAlloc = vertexAlloc;
        gpu.indexBuffer = vk::Buffer{ indexBuffer };
        gpu.indexAlloc = indexAlloc;

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = renderer.frames[0].commandPool;
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(renderer.device.allocateCommandBuffers(cmdAlloc), "uploadMesh: allocateCommandBuffers");
        if (!cmds)
        {
            vmaDestroyBuffer(renderer.allocator, staging, stagingAllocation);
            return std::unexpected(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];

        vk::CommandBufferBeginInfo begin{};
        begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(begin));
        cmd.copyBuffer(vk::Buffer{ staging }, gpu.vertexBuffer, vk::BufferCopy{ 0, 0, vertexBytes });
        cmd.copyBuffer(vk::Buffer{ staging }, gpu.indexBuffer, vk::BufferCopy{ vertexBytes, 0, indexBytes });
        static_cast<void>(cmd.end());

        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = cmd;
        vk::SubmitInfo2 submitInfo{};
        submitInfo.setCommandBufferInfos(cmdInfo);
        static_cast<void>(renderer.graphicsQueue.submit2(submitInfo, nullptr));
        static_cast<void>(renderer.device.waitIdle());
        vmaDestroyBuffer(renderer.allocator, staging, stagingAllocation);

        const u32 handle = static_cast<u32>(renderer.meshes.size());
        renderer.meshes.push_back(std::move(gpu));
        logInfo(std::format("uploaded mesh: {} vertices, {} indices, {} submeshes",
                            mesh.vertices.size(), mesh.indices.size(), mesh.submeshes.size()));
        return handle;
    }

    void drawMesh(Renderer& renderer, u32 meshHandle, u32 pipelineHandle, const glm::mat4& mvp)
    {
        if (meshHandle >= renderer.meshes.size() || pipelineHandle >= renderer.pipelines.size())
        {
            return;
        }
        const GpuMesh& gpu = renderer.meshes[meshHandle];
        vk::Pipeline pipeline = renderer.pipelines[pipelineHandle].pipeline;
        vk::PipelineLayout layout = renderer.pipelines[pipelineHandle].layout;
        vk::Buffer vertexBuffer = gpu.vertexBuffer;
        vk::Buffer indexBuffer = gpu.indexBuffer;
        std::vector<Submesh> submeshes = gpu.submeshes;
        submit(renderer, [pipeline, layout, vertexBuffer, indexBuffer, submeshes, mvp](vk::CommandBuffer cmd)
        {
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
            cmd.pushConstants(layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(glm::mat4), &mvp);
            vk::DeviceSize offset = 0;
            cmd.bindVertexBuffers(0, vertexBuffer, offset);
            cmd.bindIndexBuffer(indexBuffer, 0, vk::IndexType::eUint32);
            for (const Submesh& submesh : submeshes)
            {
                cmd.drawIndexed(submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
            }
        });
    }

    std::expected<void, std::string> captureViewport(Renderer& renderer, const std::string& path)
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
            return std::unexpected(created.error());
        }

        vk::CommandBufferAllocateInfo allocInfo{};
        allocInfo.commandPool = renderer.frames[0].commandPool;
        allocInfo.level = vk::CommandBufferLevel::ePrimary;
        allocInfo.commandBufferCount = 1;
        auto cmds = checked(renderer.device.allocateCommandBuffers(allocInfo), "capture: allocateCommandBuffers");
        if (!cmds)
        {
            vmaDestroyBuffer(renderer.allocator, rawBuffer, bufferAllocation);
            return std::unexpected(cmds.error());
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
        vmaInvalidateAllocation(renderer.allocator, bufferAllocation, 0, VK_WHOLE_SIZE);

        std::expected<void, std::string> wrote = writeBufferToPng(
            static_cast<const unsigned char*>(bufferAllocInfo.pMappedData), width, height, img.format, path);
        vmaDestroyBuffer(renderer.allocator, rawBuffer, bufferAllocation);
        if (!wrote)
        {
            return std::unexpected(wrote.error());
        }
        logInfo(std::format("captured viewport ({}x{}) to {}", width, height, path));
        return {};
    }

    std::expected<void, std::string> requestWindowCapture(Renderer& renderer, std::string path)
    {
        if (!renderer.swapchainCaptureSupported)
        {
            return std::unexpected(std::string{ "window capture unsupported: surface lacks TRANSFER_SRC usage" });
        }
        renderer.captureNextSwapchainPath = std::move(path);
        return {};
    }
}
