module;

// Vulkan-Hpp in no-exceptions mode: every call returns a result we convert to
// Result. We never use vk::raii (it throws). Classic includes (no import std).
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
#include <cmath>
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

export module Saffron.Rendering;

export import :RenderGraph;
export import :Types;

import Saffron.Core;
import Saffron.Window;
import Saffron.Geometry;
import :Detail;

namespace se
{

    auto newRenderer(Window& window) -> Result<Renderer>
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
            return Err(std::format("instance creation failed: {}", instanceResult.error().message()));
        }
        renderer.context.vkbInstance = instanceResult.value();
        renderer.context.instance = vk::Instance{ renderer.context.vkbInstance.instance };

        VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(window.handle, renderer.context.vkbInstance.instance, nullptr, &rawSurface))
        {
            return Err(std::format("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError()));
        }
        renderer.context.surface = vk::SurfaceKHR{ rawSurface };

        VkPhysicalDeviceVulkan11Features features11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        features11.shaderDrawParameters = VK_TRUE;  // Slang SV_VertexID emits the DrawParameters capability

        VkPhysicalDeviceVulkan13Features features13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;

        // Bindless: one global texture array indexed per-instance. Core Vulkan 1.2
        // descriptor indexing — required (selection fails with a clear error if absent).
        VkPhysicalDeviceVulkan12Features features12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        features12.runtimeDescriptorArray = VK_TRUE;
        features12.descriptorBindingPartiallyBound = VK_TRUE;
        features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

        vkb::PhysicalDeviceSelector selector{ renderer.context.vkbInstance };
        auto physicalResult = selector
                                  .set_minimum_version(1, 3)
                                  .set_required_features_11(features11)
                                  .set_required_features_12(features12)
                                  .set_required_features_13(features13)
                                  .set_surface(rawSurface)
                                  .select();
        if (!physicalResult)
        {
            return Err(std::format("no suitable GPU: {}", physicalResult.error().message()));
        }

        vkb::DeviceBuilder deviceBuilder{ physicalResult.value() };
        auto deviceResult = deviceBuilder.build();
        if (!deviceResult)
        {
            return Err(std::format("device creation failed: {}", deviceResult.error().message()));
        }
        renderer.context.vkbDevice = deviceResult.value();
        renderer.context.physicalDevice = vk::PhysicalDevice{ physicalResult.value().physical_device };
        renderer.context.device = vk::Device{ renderer.context.vkbDevice.device };

        auto queueResult = renderer.context.vkbDevice.get_queue(vkb::QueueType::graphics);
        if (!queueResult)
        {
            return Err(std::format("no graphics queue: {}", queueResult.error().message()));
        }
        renderer.context.graphicsQueue = vk::Queue{ queueResult.value() };
        renderer.context.graphicsQueueFamily = renderer.context.vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

        // Highest MSAA level the device supports for both color + depth framebuffers (capped at 8x).
        vk::SampleCountFlags sampleCounts = renderer.context.physicalDevice.getProperties().limits.framebufferColorSampleCounts &
                                            renderer.context.physicalDevice.getProperties().limits.framebufferDepthSampleCounts;
        if (sampleCounts & vk::SampleCountFlagBits::e8) { renderer.targets.maxSampleCount = vk::SampleCountFlagBits::e8; }
        else if (sampleCounts & vk::SampleCountFlagBits::e4) { renderer.targets.maxSampleCount = vk::SampleCountFlagBits::e4; }
        else if (sampleCounts & vk::SampleCountFlagBits::e2) { renderer.targets.maxSampleCount = vk::SampleCountFlagBits::e2; }

        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.instance = renderer.context.vkbInstance.instance;
        allocatorInfo.physicalDevice = physicalResult.value().physical_device;
        allocatorInfo.device = renderer.context.vkbDevice.device;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        if (vmaCreateAllocator(&allocatorInfo, &renderer.context.allocator) != VK_SUCCESS)
        {
            return Err(std::string{ "vmaCreateAllocator failed" });
        }

        auto swapchainBuilt = buildSwapchain(renderer, window.width, window.height);
        if (!swapchainBuilt)
        {
            return Err(swapchainBuilt.error());
        }

        // Offscreen scene target shown in the editor Viewport panel. Same format
        // as the swapchain so the scene pipelines need no special format.
        auto offscreen = newColorImage(renderer, window.width, window.height, OffscreenColorFormat, true);
        if (!offscreen)
        {
            return Err(offscreen.error());
        }
        renderer.targets.offscreen = std::move(*offscreen);
        renderer.targets.desiredWidth = window.width;
        renderer.targets.desiredHeight = window.height;
        renderer.targets.generation = 1;

        auto depth = newDepthImage(renderer, window.width, window.height);
        if (!depth)
        {
            return Err(depth.error());
        }
        renderer.targets.depth = std::move(*depth);

        for (FrameData& frame : renderer.frame.frames)
        {
            vk::CommandPoolCreateInfo poolInfo{};
            poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
            poolInfo.queueFamilyIndex = renderer.context.graphicsQueueFamily;
            auto pool = checked(renderer.context.device.createCommandPool(poolInfo), "createCommandPool");
            if (!pool)
            {
                return Err(pool.error());
            }
            frame.commandPool = *pool;

            vk::CommandBufferAllocateInfo allocInfo{};
            allocInfo.commandPool = frame.commandPool;
            allocInfo.level = vk::CommandBufferLevel::ePrimary;
            allocInfo.commandBufferCount = 1;
            auto buffers = checked(renderer.context.device.allocateCommandBuffers(allocInfo), "allocateCommandBuffers");
            if (!buffers)
            {
                return Err(buffers.error());
            }
            frame.commandBuffer = (*buffers)[0];

            auto imageAvailable = checked(renderer.context.device.createSemaphore(vk::SemaphoreCreateInfo{}), "createSemaphore");
            if (!imageAvailable)
            {
                return Err(imageAvailable.error());
            }
            frame.imageAvailable = *imageAvailable;

            vk::FenceCreateInfo fenceInfo{};
            fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled;
            auto fence = checked(renderer.context.device.createFence(fenceInfo), "createFence");
            if (!fence)
            {
                return Err(fence.error());
            }
            frame.inFlight = *fence;
        }

        if (Result<void> descriptors = initDescriptorResources(renderer); !descriptors)
        {
            return Err(descriptors.error());
        }
        // Bake the IBL environment (procedural sky -> irradiance + prefiltered + BRDF LUT)
        // once; the mesh ambient samples it via set 3.
        if (Result<void> baked = bakeEnvironment(renderer); !baked)
        {
            return Err(baked.error());
        }
        setDirectionalLight(renderer, glm::vec3(-0.5f, -1.0f, -0.3f), glm::vec3(1.0f), 1.0f, 0.15f);

        const std::array<u8, 4> white{ 255, 255, 255, 255 };
        auto whiteTexture = uploadTexture(renderer, white.data(), 1, 1, false);
        if (!whiteTexture)
        {
            return Err(whiteTexture.error());
        }
        renderer.defaultWhiteTexture = *whiteTexture;

        // Seed every bindless slot with the default texture so no slot is ever unbound —
        // some drivers (llvmpipe) fault sampling a partially-bound array even on slots a
        // shader never reads. Real uploads overwrite their slot afterwards.
        {
            std::vector<vk::DescriptorImageInfo> infos(MaxBindlessTextures,
                vk::DescriptorImageInfo{ renderer.descriptors.linearSampler, (*whiteTexture)->view,
                                         vk::ImageLayout::eShaderReadOnlyOptimal });
            vk::WriteDescriptorSet fill{};
            fill.dstSet = renderer.descriptors.bindlessSet;
            fill.dstBinding = 0;
            fill.dstArrayElement = 0;
            fill.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            fill.setImageInfo(infos);
            renderer.context.device.updateDescriptorSets(fill, {});
        }

        logInfo(std::format("vulkan ready — gpu '{}', {} swapchain images",
                            renderer.context.vkbDevice.physical_device.name,
                            renderer.swapchain.images.size()));
        return renderer;
    }

    void destroyRenderer(Renderer& renderer)
    {
        if (renderer.context.device)
        {
            static_cast<void>(renderer.context.device.waitIdle());
        }

        // Drop any Refs the renderer itself still holds, plus the closure vectors
        // (which may capture Refs), before the descriptor pool / allocator / device
        // are torn down — a GpuTexture frees its material set from the pool.
        renderer.frame.sceneDrawList = SceneDrawList{};  // drops mesh/texture/pipeline Refs
        renderer.pipelines.cache.clear();            // drops the cached mesh PSOs
        renderer.frame.sceneSubmissions.clear();
        renderer.frame.uiSubmissions.clear();
        renderer.defaultWhiteTexture.reset();
        renderer.pipelines.cull.reset();        // RAII frees the compute pipeline + layout
        renderer.pipelines.thumbnail.reset();
        renderer.pipelines.tonemap.reset();
        renderer.pipelines.fxaa.reset();
        renderer.pipelines.depthPrepass.reset();
        renderer.pipelines.shadowDepth.reset();

        renderer.targets.offscreen.reset();  // free before the allocator/device
        renderer.targets.depth.reset();
        renderer.targets.shadowMap.reset();
        renderer.ibl.envCube.reset();
        renderer.ibl.irradianceCube.reset();
        renderer.ibl.prefilteredCube.reset();
        renderer.ibl.brdfLut.reset();
        renderer.targets.msaaColor.reset();
        renderer.targets.msaaDepth.reset();
        renderer.targets.scratch.reset();

        for (u32 i = 0; i < MaxFramesInFlight; i = i + 1)
        {
            renderer.instancing.buffers[i].reset();  // RAII frees the SSBO before the allocator
            renderer.lighting.lightListBuffers[i].reset();
            renderer.lighting.clusterBuffers[i].reset();
            if (renderer.lighting.lightBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(renderer.context.allocator, renderer.lighting.lightBuffers[i], renderer.lighting.lightAllocs[i]);
                renderer.lighting.lightBuffers[i] = VK_NULL_HANDLE;
            }
            if (renderer.lighting.clusterParamBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(renderer.context.allocator, renderer.lighting.clusterParamBuffers[i], renderer.lighting.clusterParamAllocs[i]);
                renderer.lighting.clusterParamBuffers[i] = VK_NULL_HANDLE;
            }
        }
        if (renderer.descriptors.descriptorPool)
        {
            renderer.context.device.destroyDescriptorPool(renderer.descriptors.descriptorPool);
        }
        if (renderer.descriptors.bindlessPool)
        {
            renderer.context.device.destroyDescriptorPool(renderer.descriptors.bindlessPool);
        }
        if (renderer.descriptors.bindlessSetLayout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.descriptors.bindlessSetLayout);
        }
        if (renderer.descriptors.lightSetLayout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.descriptors.lightSetLayout);
        }
        if (renderer.descriptors.instanceSetLayout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.descriptors.instanceSetLayout);
        }
        if (renderer.descriptors.clusterSetLayout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.descriptors.clusterSetLayout);
        }
        if (renderer.descriptors.tonemapSetLayout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.descriptors.tonemapSetLayout);
        }
        if (renderer.descriptors.fxaaSetLayout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.descriptors.fxaaSetLayout);
        }
        if (renderer.descriptors.linearSampler)
        {
            renderer.context.device.destroySampler(renderer.descriptors.linearSampler);
        }
        if (renderer.descriptors.shadowSampler)
        {
            renderer.context.device.destroySampler(renderer.descriptors.shadowSampler);
        }
        if (renderer.ibl.setLayout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.ibl.setLayout);
        }
        if (renderer.ibl.sampler)
        {
            renderer.context.device.destroySampler(renderer.ibl.sampler);
        }

        for (FrameData& frame : renderer.frame.frames)
        {
            renderer.context.device.destroyFence(frame.inFlight);
            renderer.context.device.destroySemaphore(frame.imageAvailable);
            renderer.context.device.destroyCommandPool(frame.commandPool);
        }

        destroySwapchainResources(renderer);

        if (renderer.context.allocator != nullptr)
        {
            vmaDestroyAllocator(renderer.context.allocator);
            renderer.context.allocator = nullptr;
        }
        if (renderer.context.surface)
        {
            vkb::destroy_surface(renderer.context.vkbInstance, static_cast<VkSurfaceKHR>(renderer.context.surface));
        }
        vkb::destroy_device(renderer.context.vkbDevice);
        vkb::destroy_instance(renderer.context.vkbInstance);
    }

    auto beginFrame(Renderer& renderer) -> bool
    {
        const u32 winW = renderer.window->width;
        const u32 winH = renderer.window->height;
        if (winW > 0 && winH > 0 &&
            (renderer.swapchain.extent.width != winW || renderer.swapchain.extent.height != winH))
        {
            static_cast<void>(renderer.context.device.waitIdle());
            recreateSwapchain(renderer);
            return false;
        }

        FrameData& frame = renderer.frame.frames[renderer.frame.index];

        static_cast<void>(renderer.context.device.waitForFences(frame.inFlight, VK_TRUE, UINT64_MAX));

        vk::ResultValue<u32> acquire = renderer.context.device.acquireNextImageKHR(
            renderer.swapchain.handle, UINT64_MAX, frame.imageAvailable, nullptr);
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
        renderer.frame.imageIndex = acquire.value;

        // Ensure the previous frame that used THIS image has finished before we
        // reuse the image's renderFinished semaphore.
        if (renderer.swapchain.imagesInFlight[renderer.frame.imageIndex])
        {
            static_cast<void>(renderer.context.device.waitForFences(renderer.swapchain.imagesInFlight[renderer.frame.imageIndex], VK_TRUE, UINT64_MAX));
        }
        renderer.swapchain.imagesInFlight[renderer.frame.imageIndex] = frame.inFlight;

        // Apply a pending Viewport resize (requested last frame). Single shared
        // target, so a full device idle is required before recreating it.
        if (renderer.targets.desiredWidth > 0 && renderer.targets.desiredHeight > 0 &&
            (renderer.targets.desiredWidth != renderer.targets.offscreen.extent.width ||
             renderer.targets.desiredHeight != renderer.targets.offscreen.extent.height))
        {
            static_cast<void>(renderer.context.device.waitIdle());
            auto resized = newColorImage(renderer, renderer.targets.desiredWidth,
                                         renderer.targets.desiredHeight, OffscreenColorFormat, true);
            if (resized)
            {
                renderer.targets.offscreen = std::move(*resized);
                renderer.targets.generation = renderer.targets.generation + 1;
                updateTonemapSet(renderer);  // the storage-image binding follows the new view
                auto resizedDepth = newDepthImage(renderer, renderer.targets.desiredWidth, renderer.targets.desiredHeight);
                if (resizedDepth)
                {
                    renderer.targets.depth = std::move(*resizedDepth);
                }
                else
                {
                    logError(resizedDepth.error());
                }
                recreateMsaaTargets(renderer);  // MSAA targets follow the offscreen extent
                recreateFxaaTarget(renderer);   // and the FXAA scratch target
            }
            else
            {
                logError(resized.error());
            }
        }

        static_cast<void>(renderer.context.device.resetFences(frame.inFlight));
        static_cast<void>(frame.commandBuffer.reset());
        renderer.frame.sceneDrawList = SceneDrawList{};  // last frame's geometry has presented
        renderer.frame.sceneSubmissions.clear();
        renderer.frame.uiSubmissions.clear();

        vk::CommandBufferBeginInfo beginInfo{};
        beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(frame.commandBuffer.begin(beginInfo));

        // Rendering scopes are opened in endFrame: pass 1 (scene → offscreen),
        // pass 2 (ui → swapchain).
        return true;
    }

    void submit(Renderer& renderer, RenderFn fn)
    {
        renderer.frame.sceneSubmissions.push_back(std::move(fn));
    }

    void submitUi(Renderer& renderer, RenderFn fn)
    {
        renderer.frame.uiSubmissions.push_back(std::move(fn));
    }

    void setViewportDesiredSize(Renderer& renderer, u32 width, u32 height)
    {
        renderer.targets.desiredWidth = width;
        renderer.targets.desiredHeight = height;
    }

    auto viewportImageView(const Renderer& renderer) -> vk::ImageView
    {
        return renderer.targets.offscreen.view;
    }

    auto viewportGeneration(const Renderer& renderer) -> u32
    {
        return renderer.targets.generation;
    }

    auto viewportWidth(const Renderer& renderer) -> u32
    {
        return renderer.targets.offscreen.extent.width;
    }

    auto viewportHeight(const Renderer& renderer) -> u32
    {
        return renderer.targets.offscreen.extent.height;
    }

    void beginFrameGraph(Renderer& renderer)
    {
        Image& offscreen = renderer.targets.offscreen;
        Image& depth = renderer.targets.depth;
        const u32 f = renderer.frame.index;
        const bool doCull = renderer.lighting.clusterDispatchPending && renderer.pipelines.cull;
        renderer.lighting.clusterDispatchPending = false;

        // The frame as a render graph: declare each pass's resource usage and let the
        // graph derive the barriers + layout transitions. The offscreen color carries
        // its layout across frames (sampled by ImGui last frame → WAR into this scene).
        renderer.graph.current = newRenderGraph();
        RenderGraph& graph = renderer.graph.current;
        // frameSceneColor is always the offscreen (what ImGui samples + tonemap reads). The
        // scene's 1x result lands in `sceneOutput`: the offscreen normally, or the FXAA
        // scratch when FXAA is on (FXAA then edge-blurs scratch → offscreen). With MSAA the
        // scene renders to msaaColor and resolves into sceneOutput. mutually exclusive via set-aa.
        const bool msaa = renderer.targets.sampleCount != vk::SampleCountFlagBits::e1 && renderer.targets.msaaColor.image;
        const bool fxaa = renderer.targets.fxaaEnabled && renderer.targets.scratch.image && renderer.pipelines.fxaa;
        renderer.graph.sceneColor = importImage(graph, offscreen.image, offscreen.view,
            vk::ImageAspectFlagBits::eColor, offscreen.layout, &offscreen.layout);
        RgResource sceneOutput = renderer.graph.sceneColor;
        if (fxaa)
        {
            sceneOutput = importImage(graph, renderer.targets.scratch.image, renderer.targets.scratch.view,
                vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined, nullptr);
        }
        RgResource sceneColorAttachment = sceneOutput;
        if (msaa)
        {
            sceneColorAttachment = importImage(graph, renderer.targets.msaaColor.image, renderer.targets.msaaColor.view,
                vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined, nullptr);
        }
        Image* depthTarget = &depth;
        if (msaa)
        {
            depthTarget = &renderer.targets.msaaDepth;
        }
        RgResource sceneDepth = importImage(graph, depthTarget->image, depthTarget->view,
            vk::ImageAspectFlagBits::eDepth, vk::ImageLayout::eUndefined, nullptr);
        renderer.graph.swapImage = importImage(graph, renderer.swapchain.images[renderer.frame.imageIndex],
            renderer.swapchain.imageViews[renderer.frame.imageIndex], vk::ImageAspectFlagBits::eColor,
            vk::ImageLayout::eUndefined, nullptr);

        // Directional shadow: a depth-only pass renders the scene from the light's view
        // into the shadow map; the scene pass then samples it. The graph derives the
        // DepthWrite -> ShaderReadOnly transition (and the cross-frame WAR) from the usages.
        const bool doShadow = renderer.lighting.shadowPending && renderer.pipelines.shadowDepth &&
                              renderer.targets.shadowMap.image;
        RgResource shadowRes{};
        if (doShadow)
        {
            Image& shadowMap = renderer.targets.shadowMap;
            shadowRes = importImage(graph, shadowMap.image, shadowMap.view,
                vk::ImageAspectFlagBits::eDepth, shadowMap.layout, &shadowMap.layout);
            RgPass shadowPass;
            shadowPass.name = "shadow";
            shadowPass.kind = RgPassKind::Graphics;
            shadowPass.depth = RgAttachment{ shadowRes, vk::AttachmentLoadOp::eClear,
                vk::AttachmentStoreOp::eStore, vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } } };
            shadowPass.renderArea = shadowMap.extent;
            const glm::mat4 lightViewProj = renderer.lighting.shadowViewProj;
            shadowPass.execute = [&renderer, lightViewProj](vk::CommandBuffer cmd)
        {
                recordShadowDepth(renderer, cmd, lightViewProj);
            };
            addPass(graph, std::move(shadowPass));
        }

        // Clustered forward: a compute pass culls the punctual lights into the froxel
        // grid; the scene fragment reads the result (the graph emits the compute→
        // fragment barrier from these declared usages).
        RgResource clusterBuffer{};
        if (doCull)
        {
            clusterBuffer = importBuffer(graph, renderer.lighting.clusterBuffers[f]->buffer);

            RgPass cull;
            cull.name = "light-cull";
            cull.kind = RgPassKind::Compute;
            cull.accesses = { RgAccess{ clusterBuffer, RgUsage::StorageWriteCompute } };
            cull.execute = [&renderer, f](vk::CommandBuffer cmd)
        {
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.cull->pipeline);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                    renderer.pipelines.cull->layout, 0, renderer.lighting.clusterSets[f], {});
                const u32 groups = (ClusterCount + 63) / 64;
                cmd.dispatch(groups, 1, 1);
            };
            addPass(graph, std::move(cull));
        }

        // Optional depth pre-pass: lay down scene depth first, so the scene pass loads it
        // and shades only the front-most fragments. The graph derives the depth WAW
        // barrier (pre-pass write → scene write) from the two declared depth usages.
        const bool doDepthPrepass = renderer.useDepthPrepass && renderer.pipelines.depthPrepass;
        if (doDepthPrepass)
        {
            RgPass depthPass;
            depthPass.name = "depth-prepass";
            depthPass.kind = RgPassKind::Graphics;
            depthPass.depth = RgAttachment{ sceneDepth, vk::AttachmentLoadOp::eClear,
                vk::AttachmentStoreOp::eStore, vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } } };
            depthPass.renderArea = offscreen.extent;
            depthPass.execute = [&renderer](vk::CommandBuffer cmd)
        {
                recordDepthPrepass(renderer, cmd);
            };
            addPass(graph, std::move(depthPass));
        }

        RgPass scene;
        scene.name = "scene";
        scene.kind = RgPassKind::Graphics;
        if (doCull)
        {
            scene.accesses = { RgAccess{ clusterBuffer, RgUsage::StorageReadFragment } };
        }
        if (doShadow)
        {
            scene.accesses.push_back(RgAccess{ shadowRes, RgUsage::SampledRead });
        }
        // MSAA: render to the multisampled color, resolve into the offscreen (don't store
        // the multisampled samples). Otherwise render straight into the offscreen.
        scene.color = RgAttachment{ sceneColorAttachment, vk::AttachmentLoadOp::eClear,
            vk::AttachmentStoreOp::eStore, vk::ClearValue{ vk::ClearColorValue{ renderer.frame.clearColor } } };
        if (msaa)
        {
            scene.color->storeOp = vk::AttachmentStoreOp::eDontCare;
            scene.color->resolve = sceneOutput;
        }
        // Load the pre-pass depth when present; otherwise clear it here as before.
        vk::AttachmentLoadOp depthLoad = vk::AttachmentLoadOp::eClear;
        if (doDepthPrepass)
        {
            depthLoad = vk::AttachmentLoadOp::eLoad;
        }
        scene.depth = RgAttachment{ sceneDepth, depthLoad, vk::AttachmentStoreOp::eDontCare,
            vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } } };
        scene.renderArea = offscreen.extent;
        scene.execute = [&renderer](vk::CommandBuffer cmd)
        {
            recordSceneDrawList(renderer, cmd);
            for (RenderFn& fn : renderer.frame.sceneSubmissions)
            {
                fn(cmd);
            }
        };
        addPass(graph, std::move(scene));

        // FXAA: edge-blur the scene scratch into the offscreen (a compute pass). Added here
        // so it runs before any app-authored post-process (e.g. tonemap) + the ui pass.
        if (fxaa)
        {
            RgPass fxaaPass;
            fxaaPass.name = "fxaa";
            fxaaPass.kind = RgPassKind::Compute;
            fxaaPass.accesses = { RgAccess{ sceneOutput, RgUsage::SampledReadCompute },
                                  RgAccess{ renderer.graph.sceneColor, RgUsage::StorageImageRWCompute } };
            const vk::Extent2D extent = offscreen.extent;
            fxaaPass.execute = [&renderer, extent](vk::CommandBuffer cmd)
        {
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.fxaa->pipeline);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                    renderer.pipelines.fxaa->layout, 0, renderer.descriptors.fxaaSet, {});
                cmd.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
            };
            addPass(graph, std::move(fxaaPass));
        }

        // HDR offscreen → display: the tonemap is mandatory (the scene wrote linear HDR
        // radiance). Added after the scene + AA passes, before any app-authored pass + ui.
        addTonemapPass(renderer, graph);
    }

    auto frameGraph(Renderer& renderer) -> RenderGraph&
    {
        return renderer.graph.current;
    }

    auto viewportColorResource(const Renderer& renderer) -> RgResource
    {
        return renderer.graph.sceneColor;
    }

    void addTonemapPass(Renderer& renderer, RenderGraph& graph)
    {
        RgPass pass;
        pass.name = "tonemap";
        pass.kind = RgPassKind::Compute;
        pass.accesses = { RgAccess{ renderer.graph.sceneColor, RgUsage::StorageImageRWCompute } };
        pass.execute = [&renderer](vk::CommandBuffer cmd)
        {
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.tonemap->pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                renderer.pipelines.tonemap->layout, 0, renderer.descriptors.tonemapSet, {});
            const f32 exposure = std::exp2(renderer.exposureEv);
            cmd.pushConstants(renderer.pipelines.tonemap->layout, vk::ShaderStageFlagBits::eCompute,
                              0, sizeof(f32), &exposure);
            const vk::Extent2D extent = renderer.targets.offscreen.extent;
            cmd.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        };
        addPass(graph, std::move(pass));
    }

    void endFrame(Renderer& renderer)
    {
        FrameData& frame = renderer.frame.frames[renderer.frame.index];
        RenderGraph& graph = renderer.graph.current;

        // The ui pass samples the (now post-processed) offscreen color and composites
        // ImGui into the swapchain. Added last so app-authored passes land before it.
        RgPass ui;
        ui.name = "ui";
        ui.kind = RgPassKind::Graphics;
        ui.accesses = { RgAccess{ renderer.graph.sceneColor, RgUsage::SampledRead } };
        ui.color = RgAttachment{ renderer.graph.swapImage, vk::AttachmentLoadOp::eClear,
            vk::AttachmentStoreOp::eStore, vk::ClearValue{ vk::ClearColorValue{ renderer.frame.clearColor } } };
        ui.renderArea = renderer.swapchain.extent;
        ui.execute = [&renderer](vk::CommandBuffer cmd)
        {
            for (RenderFn& fn : renderer.frame.uiSubmissions)
            {
                fn(cmd);
            }
        };
        addPass(graph, std::move(ui));

        executeRenderGraph(graph, frame.commandBuffer);

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
            captureExtent = renderer.swapchain.extent;
            const vk::DeviceSize bytes =
                static_cast<vk::DeviceSize>(captureExtent.width) * captureExtent.height * 4;
            Result<void> created =
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
                frame.commandBuffer, renderer.swapchain.images[renderer.frame.imageIndex], captureExtent,
                vk::ImageLayout::eColorAttachmentOptimal,
                vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                vk::ImageLayout::ePresentSrcKHR,
                vk::PipelineStageFlagBits2::eBottomOfPipe, vk::AccessFlagBits2::eNone,
                vk::Buffer{ captureBuffer });
        }
        else
        {
            transitionImage(
                frame.commandBuffer, renderer.swapchain.images[renderer.frame.imageIndex],
                vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR,
                vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                vk::PipelineStageFlagBits2::eBottomOfPipe, vk::AccessFlagBits2::eNone);
        }

        static_cast<void>(frame.commandBuffer.end());

        vk::Semaphore signalSemaphore = renderer.swapchain.renderFinished[renderer.frame.imageIndex];

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
        static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, frame.inFlight));

        vk::PresentInfoKHR presentInfo{};
        presentInfo.setWaitSemaphores(signalSemaphore);
        presentInfo.setSwapchains(renderer.swapchain.handle);
        presentInfo.setImageIndices(renderer.frame.imageIndex);
        vk::Result present = renderer.context.graphicsQueue.presentKHR(presentInfo);
        if (present == vk::Result::eErrorOutOfDateKHR || present == vk::Result::eSuboptimalKHR)
        {
            recreateSwapchain(renderer);
        }

        // The recorded copy is now submitted; idle so it completed, then write the PNG.
        if (doCapture && captureBuffer != VK_NULL_HANDLE)
        {
            static_cast<void>(renderer.context.device.waitIdle());
            vmaInvalidateAllocation(renderer.context.allocator, captureAlloc, 0, VK_WHOLE_SIZE);
            auto wrote = writeBufferToPng(
                static_cast<const unsigned char*>(captureInfo.pMappedData),
                captureExtent.width, captureExtent.height,
                renderer.swapchain.format, *renderer.captureNextSwapchainPath);
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
            vmaDestroyBuffer(renderer.context.allocator, captureBuffer, captureAlloc);
            renderer.captureNextSwapchainPath.reset();
        }

        renderer.frame.index = (renderer.frame.index + 1) % MaxFramesInFlight;
    }

    auto assetPath(std::string_view relative) -> std::string
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

    auto defaultTexture(const Renderer& renderer) -> const Ref<GpuTexture>&
    {
        return renderer.defaultWhiteTexture;
    }

    auto renderStats(const Renderer& renderer) -> RenderStats
    {
        return renderer.stats;
    }

    void setExposure(Renderer& renderer, f32 ev)
    {
        renderer.exposureEv = ev;
    }

    auto exposureEv(const Renderer& renderer) -> f32
    {
        return renderer.exposureEv;
    }

    void setIbl(Renderer& renderer, bool enabled)
    {
        renderer.ibl.useIbl = enabled;
    }

    auto iblEnabled(const Renderer& renderer) -> bool
    {
        return renderer.ibl.useIbl && renderer.ibl.ready;
    }

    void setShadows(Renderer& renderer, bool enabled)
    {
        renderer.lighting.useShadows = enabled;
    }

    auto shadowsEnabled(const Renderer& renderer) -> bool
    {
        return renderer.lighting.useShadows;
    }

    void setDirectionalShadow(Renderer& renderer, const glm::mat4& lightViewProj, bool casting)
    {
        renderer.lighting.shadowViewProj = lightViewProj;
        renderer.lighting.shadowPending = casting && renderer.lighting.useShadows;
    }

    void waitGpuIdle(Renderer& renderer)
    {
        if (renderer.context.device)
        {
            static_cast<void>(renderer.context.device.waitIdle());
        }
    }

}
