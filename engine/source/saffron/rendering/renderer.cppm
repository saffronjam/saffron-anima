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
        features12.bufferDeviceAddress = VK_TRUE;  // required by KHR acceleration structures

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

        // Ray tracing is OPTIONAL: enable the KHR AS + ray-query + deferred-host-ops
        // extensions only if the selected device has them (enable_extension_if_present does
        // not fail otherwise). RT support = both core extensions present AND their feature
        // bits set; we only chain the RT feature structs + enable RT then.
        const bool hasAsExt = physicalResult.value().enable_extension_if_present(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        const bool hasRqExt = physicalResult.value().enable_extension_if_present(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        physicalResult.value().enable_extension_if_present(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        bool rtSupported = false;
        if (hasAsExt && hasRqExt)
        {
            VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeat{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
            VkPhysicalDeviceRayQueryFeaturesKHR rqFeat{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
            asFeat.pNext = &rqFeat;
            VkPhysicalDeviceFeatures2 feat2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            feat2.pNext = &asFeat;
            vkGetPhysicalDeviceFeatures2(physicalResult.value().physical_device, &feat2);
            rtSupported = asFeat.accelerationStructure == VK_TRUE && rqFeat.rayQuery == VK_TRUE;
        }

        vkb::DeviceBuilder deviceBuilder{ physicalResult.value() };
        // Chain the RT feature structs into device creation only when supported.
        VkPhysicalDeviceAccelerationStructureFeaturesKHR asEnable{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
        VkPhysicalDeviceRayQueryFeaturesKHR rqEnable{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
        if (rtSupported)
        {
            asEnable.accelerationStructure = VK_TRUE;
            rqEnable.rayQuery = VK_TRUE;
            deviceBuilder.add_pNext(&asEnable);
            deviceBuilder.add_pNext(&rqEnable);
        }
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

        // Resolve the KHR acceleration-structure / ray-query device entry points (not
        // statically exported by the loader; the engine otherwise uses static dispatch).
        renderer.context.rtSupported = rtSupported;
        if (rtSupported)
        {
            VkDevice dev = renderer.context.vkbDevice.device;
            auto load = [dev](const char* n) { return vkGetDeviceProcAddr(dev, n); };
            renderer.context.rt.getBuildSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(load("vkGetAccelerationStructureBuildSizesKHR"));
            renderer.context.rt.createAccel = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(load("vkCreateAccelerationStructureKHR"));
            renderer.context.rt.destroyAccel = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(load("vkDestroyAccelerationStructureKHR"));
            renderer.context.rt.cmdBuild = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(load("vkCmdBuildAccelerationStructuresKHR"));
            renderer.context.rt.getAccelAddress = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(load("vkGetAccelerationStructureDeviceAddressKHR"));
            if (renderer.context.rt.getBuildSizes == nullptr || renderer.context.rt.createAccel == nullptr ||
                renderer.context.rt.destroyAccel == nullptr || renderer.context.rt.cmdBuild == nullptr ||
                renderer.context.rt.getAccelAddress == nullptr)
            {
                logWarn("ray tracing: failed to resolve AS entry points; disabling RT");
                renderer.context.rtSupported = false;
            }
            else
            {
                logInfo("ray tracing available (KHR acceleration_structure + ray_query)");
            }
        }

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
        if (renderer.context.rtSupported)
        {
            // BDA is needed to feed vertex/index/instance buffer addresses to AS builds.
            allocatorInfo.flags = allocatorInfo.flags | VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        }
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
        if (Result<void> baked = bakeEnvironment(renderer, renderer.ibl.bakedParams, true); !baked)
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
        renderer.pipelines.pointShadow.reset();
        renderer.pipelines.gbuffer.reset();
        renderer.pipelines.gtao.reset();
        renderer.pipelines.aoBlur.reset();
        renderer.pipelines.contact.reset();
        renderer.pipelines.ssgi.reset();
        renderer.pipelines.copyColor.reset();
        renderer.pipelines.motion.reset();
        renderer.pipelines.taa.reset();
        renderer.pipelines.ddgiVoxelize.reset();
        renderer.pipelines.ddgiTrace.reset();
        renderer.pipelines.ddgiBlendIrr.reset();
        renderer.pipelines.ddgiBlendDist.reset();
        renderer.pipelines.ddgiBorder.reset();
        renderer.pipelines.restirInitial.reset();
        renderer.pipelines.restirReuse.reset();
        renderer.pipelines.restirResolve.reset();
        renderer.sky.pipeline.reset();          // fullscreen sky PSO
        renderer.restir.radiance.reset();
        renderer.restir.initial.reset();
        renderer.restir.combined.reset();
        renderer.restir.previous.reset();
        renderer.ddgi.boxBuffer.reset();
        renderer.ddgi.voxels.reset();
        renderer.ddgi.irradiance.reset();
        renderer.ddgi.distance.reset();
        renderer.ddgi.rays.reset();
        for (u32 i = 0; i < MaxFramesInFlight; i = i + 1)
        {
            renderer.rt.tlas[i].reset();
            renderer.rt.instanceBuffers[i].reset();
            renderer.rt.scratchBuffers[i].reset();
        }
        // The last frame's RT scene holds Ref<GpuMesh> captured by setRtScene; beginFrame
        // would clear it next frame, but there is no next frame at teardown. Drop them so the
        // meshes (vertex + index + BLAS buffers) free before the allocator, not after.
        renderer.rt.frameMeshes.clear();
        renderer.rt.frameModels.clear();
        for (vk::ImageView& face : renderer.targets.pointShadowFaces)
        {
            if (face)
            {
                renderer.context.device.destroyImageView(face);
                face = nullptr;
            }
        }

        renderer.targets.offscreen.reset();  // free before the allocator/device
        renderer.targets.depth.reset();
        renderer.targets.shadowMap.reset();
        renderer.targets.spotShadowMap.reset();
        renderer.targets.pointShadowCube.reset();
        renderer.targets.pointShadowDepth.reset();
        renderer.targets.gNormal.reset();
        renderer.targets.gDepth.reset();
        renderer.targets.aoRaw.reset();
        renderer.targets.aoMap.reset();
        renderer.targets.contactMap.reset();
        renderer.targets.ssgiMap.reset();
        renderer.targets.prevColor.reset();
        renderer.targets.motion.reset();
        renderer.targets.motionDepth.reset();
        renderer.targets.history[0].reset();
        renderer.targets.history[1].reset();
        renderer.ibl.envCube.reset();
        renderer.ibl.irradianceCube.reset();
        renderer.ibl.prefilteredCube.reset();
        renderer.ibl.brdfLut.reset();
        renderer.ibl.envPanorama.reset();
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
        if (renderer.sky.setLayout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.sky.setLayout);
        }
        if (renderer.ssao.compute2Layout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.ssao.compute2Layout);
        }
        if (renderer.ssao.compute3Layout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.ssao.compute3Layout);
        }
        if (renderer.ssao.meshSetLayout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.ssao.meshSetLayout);
        }
        if (renderer.ssao.sampler)
        {
            renderer.context.device.destroySampler(renderer.ssao.sampler);
        }
        if (renderer.descriptors.taaSetLayout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.descriptors.taaSetLayout);
        }
        for (vk::DescriptorSetLayout l : { renderer.ddgi.voxelLayout, renderer.ddgi.traceLayout,
                                           renderer.ddgi.blendIrrLayout, renderer.ddgi.blendDistLayout,
                                           renderer.ddgi.borderLayout, renderer.ddgi.meshLayout })
        {
            if (l) { renderer.context.device.destroyDescriptorSetLayout(l); }
        }
        if (renderer.ddgi.sampler)
        {
            renderer.context.device.destroySampler(renderer.ddgi.sampler);
        }
        if (renderer.rt.meshLayout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.rt.meshLayout);
        }
        for (vk::DescriptorSetLayout l : { renderer.restir.initialLayout, renderer.restir.reuseLayout,
                                           renderer.restir.resolveLayout, renderer.restir.meshLayout })
        {
            if (l) { renderer.context.device.destroyDescriptorSetLayout(l); }
        }
        if (renderer.restir.sampler)
        {
            renderer.context.device.destroySampler(renderer.restir.sampler);
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
                recreateFxaaTarget(renderer);   // and the FXAA/TAA scratch target
                recreateSsaoTargets(renderer);  // and the SSAO G-buffer + AO map
                recreateTaaTargets(renderer);   // and the TAA motion + history (after scratch)
                if (renderer.context.rtSupported) { recreateRestirTargets(renderer); }  // ReSTIR reservoirs
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
        // Re-bake the IBL environment if the sky inputs changed (the directional light moved).
        // Deferred to here — a GPU-idle point after beginFrame — so the visible sky + IBL relight
        // together. waitIdle inside the bake stalls; it is an editor-time event, not per-frame hot.
        if (renderer.ibl.rebakePending)
        {
            if (Result<void> r = bakeEnvironment(renderer, renderer.ibl.pendingParams, false); r)
            {
                renderer.ibl.bakedParams = renderer.ibl.pendingParams;
                renderer.ibl.bakedSource = renderer.ibl.source;
            }
            else
            {
                logError(r.error());
            }
            renderer.ibl.rebakePending = false;
        }

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
        const bool taa = renderer.targets.taaEnabled && renderer.targets.scratch.image &&
                         renderer.pipelines.motion && renderer.pipelines.taa;
        renderer.graph.sceneColor = importImage(graph, offscreen.image, offscreen.view,
            vk::ImageAspectFlagBits::eColor, offscreen.layout, &offscreen.layout);
        RgResource sceneOutput = renderer.graph.sceneColor;
        // FXAA + TAA both render the scene's 1x result into the scratch image, then a
        // compute pass resolves scratch -> offscreen.
        if (fxaa || taa)
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

        // Spot shadow: the first shadow-casting spot light gets its own depth pass into the
        // spot shadow map, with the spot's perspective light-space transform.
        const bool doSpotShadow = renderer.lighting.spotShadowPending && renderer.pipelines.shadowDepth &&
                                  renderer.targets.spotShadowMap.image;
        RgResource spotShadowRes{};
        if (doSpotShadow)
        {
            Image& spotMap = renderer.targets.spotShadowMap;
            spotShadowRes = importImage(graph, spotMap.image, spotMap.view,
                vk::ImageAspectFlagBits::eDepth, spotMap.layout, &spotMap.layout);
            RgPass spotPass;
            spotPass.name = "spot-shadow";
            spotPass.kind = RgPassKind::Graphics;
            spotPass.depth = RgAttachment{ spotShadowRes, vk::AttachmentLoadOp::eClear,
                vk::AttachmentStoreOp::eStore, vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } } };
            spotPass.renderArea = spotMap.extent;
            const glm::mat4 spotViewProj = renderer.lighting.spotShadowViewProj;
            spotPass.execute = [&renderer, spotViewProj](vk::CommandBuffer cmd)
        {
                recordShadowDepth(renderer, cmd, spotViewProj);
            };
            addPass(graph, std::move(spotPass));
        }

        // Point shadow: the omnidirectional distance cube can't be a graph attachment (its
        // 6 layers exceed the graph's single-layer barrier), so it runs as a Compute-kind
        // pass whose body opens its own 6 face rendering scopes + manages the cube's
        // layout, ending ShaderReadOnly with a fragment-shader barrier for the scene sample.
        const bool doPointShadow = renderer.lighting.pointShadowPending && renderer.pipelines.pointShadow &&
                                   renderer.targets.pointShadowCube.image;
        if (doPointShadow)
        {
            RgPass pointPass;
            pointPass.name = "point-shadow";
            pointPass.kind = RgPassKind::Compute;
            const glm::vec3 lightPos = renderer.lighting.pointShadowPos;
            const f32 farPlane = renderer.lighting.pointShadowFar;
            pointPass.execute = [&renderer, lightPos, farPlane](vk::CommandBuffer cmd)
        {
                recordPointShadow(renderer, cmd, lightPos, farPlane);
            };
            addPass(graph, std::move(pointPass));
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

        // Screen-space effects off a thin G-buffer (view normal + view-Z): GTAO + bilateral
        // denoise, directional contact shadows, one-bounce SSGI. The G-buffer prepass runs
        // when ANY of them is on; each effect's compute pass is gated by its toggle. The
        // scene pass samples whichever maps are produced (flags in the light UBO gate the
        // shader reads). The graph derives all the ColorWrite/Storage/SampledRead barriers.
        const bool gbufReady = renderer.ssao.ready && renderer.pipelines.gbuffer &&
                               renderer.targets.gNormal.image;
        const bool doSsao = gbufReady && renderer.ssao.useSsao && renderer.pipelines.gtao && renderer.pipelines.aoBlur;
        const bool doContact = gbufReady && renderer.ssao.useContact && renderer.pipelines.contact;
        const bool doSsgi = gbufReady && renderer.ssao.useSsgi && renderer.pipelines.ssgi && renderer.pipelines.copyColor;
        // ReSTIR also needs the G-buffer (world pos/normal). Force the prepass when ReSTIR is
        // on even with no screen effect; the gNormal handle is captured for its sets.
        const bool wantRestir = renderer.restir.useRestir && renderer.restir.ready &&
                                renderer.context.rtSupported && gbufReady;
        const bool doScreen = doSsao || doContact || doSsgi || wantRestir;
        const vk::Extent2D ssExtent = offscreen.extent;
        const auto ssGroups = [](u32 n) -> u32 { return (n + 7) / 8; };
        renderer.graph.hasAo = false;
        renderer.graph.hasContact = false;
        renderer.graph.hasSsgi = false;
        renderer.graph.hasGbuffer = false;
        if (doScreen)
        {
            renderer.graph.hasGbuffer = true;
            RgResource gNormalRes = importImage(graph, renderer.targets.gNormal.image, renderer.targets.gNormal.view,
                vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined, nullptr);
            RgResource gDepthRes = importImage(graph, renderer.targets.gDepth.image, renderer.targets.gDepth.view,
                vk::ImageAspectFlagBits::eDepth, vk::ImageLayout::eUndefined, nullptr);

            RgPass gpass;
            gpass.name = "gbuffer";
            gpass.kind = RgPassKind::Graphics;
            gpass.colors.push_back(RgAttachment{ gNormalRes, vk::AttachmentLoadOp::eClear,
                vk::AttachmentStoreOp::eStore, vk::ClearValue{ vk::ClearColorValue{ std::array<f32, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } } } });
            gpass.depth = RgAttachment{ gDepthRes, vk::AttachmentLoadOp::eClear,
                vk::AttachmentStoreOp::eStore, vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } } };
            gpass.renderArea = offscreen.extent;
            gpass.execute = [&renderer](vk::CommandBuffer cmd) { recordGbuffer(renderer, cmd); };
            addPass(graph, std::move(gpass));

            const glm::mat4 proj = renderer.ssao.projection;
            const glm::mat4 invProj = renderer.ssao.invProjection;

            if (doSsao)
            {
                RgResource aoRawRes = importImage(graph, renderer.targets.aoRaw.image, renderer.targets.aoRaw.view,
                    vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined, nullptr);
                RgResource aoRes = importImage(graph, renderer.targets.aoMap.image, renderer.targets.aoMap.view,
                    vk::ImageAspectFlagBits::eColor, renderer.targets.aoMap.layout, &renderer.targets.aoMap.layout);
                RgPass aopass;
                aopass.name = "gtao";
                aopass.kind = RgPassKind::Compute;
                aopass.accesses = { RgAccess{ gNormalRes, RgUsage::SampledReadCompute },
                                    RgAccess{ aoRawRes, RgUsage::StorageImageRWCompute } };
                const f32 radius = renderer.ssao.radius;
                const f32 strength = renderer.ssao.strength;
                aopass.execute = [&renderer, ssExtent, ssGroups, invProj, radius, strength](vk::CommandBuffer cmd)
        {
                    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.gtao->pipeline);
                    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.gtao->layout, 0, renderer.ssao.gtaoSet, {});
                    struct { glm::mat4 invProjection; glm::vec4 params; } push{ invProj, glm::vec4(radius, strength, 0.0f, 0.0f) };
                    cmd.pushConstants(renderer.pipelines.gtao->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
                    cmd.dispatch(ssGroups(ssExtent.width), ssGroups(ssExtent.height), 1);
                };
                addPass(graph, std::move(aopass));

                RgPass blurpass;
                blurpass.name = "ao-blur";
                blurpass.kind = RgPassKind::Compute;
                blurpass.accesses = { RgAccess{ aoRawRes, RgUsage::SampledReadCompute },
                                      RgAccess{ gNormalRes, RgUsage::SampledReadCompute },
                                      RgAccess{ aoRes, RgUsage::StorageImageRWCompute } };
                blurpass.execute = [&renderer, ssExtent, ssGroups](vk::CommandBuffer cmd)
        {
                    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.aoBlur->pipeline);
                    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.aoBlur->layout, 0, renderer.ssao.aoBlurSet, {});
                    cmd.dispatch(ssGroups(ssExtent.width), ssGroups(ssExtent.height), 1);
                };
                addPass(graph, std::move(blurpass));
                renderer.graph.aoResource = aoRes;
                renderer.graph.hasAo = true;
            }

            if (doContact)
            {
                RgResource contactRes = importImage(graph, renderer.targets.contactMap.image, renderer.targets.contactMap.view,
                    vk::ImageAspectFlagBits::eColor, renderer.targets.contactMap.layout, &renderer.targets.contactMap.layout);
                RgPass cpass;
                cpass.name = "contact-shadows";
                cpass.kind = RgPassKind::Compute;
                cpass.accesses = { RgAccess{ gNormalRes, RgUsage::SampledReadCompute },
                                   RgAccess{ contactRes, RgUsage::StorageImageRWCompute } };
                const glm::vec3 sunView = renderer.ssao.sunDirView;
                cpass.execute = [&renderer, ssExtent, ssGroups, proj, invProj, sunView](vk::CommandBuffer cmd)
        {
                    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.contact->pipeline);
                    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.contact->layout, 0, renderer.ssao.contactSet, {});
                    struct { glm::mat4 projection; glm::mat4 invProjection; glm::vec4 lightDirView; glm::vec4 params; }
                        push{ proj, invProj, glm::vec4(sunView, 0.0f), glm::vec4(0.6f, 12.0f, 0.6f, 0.0f) };
                    cmd.pushConstants(renderer.pipelines.contact->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
                    cmd.dispatch(ssGroups(ssExtent.width), ssGroups(ssExtent.height), 1);
                };
                addPass(graph, std::move(cpass));
                renderer.graph.contactResource = contactRes;
                renderer.graph.hasContact = true;
            }

            if (doSsgi)
            {
                // SSGI gathers the PREVIOUS frame's color (prevColor), captured last frame.
                // Import prevColor ONCE here (read now, written by the copy_color pass after
                // the scene); aliasing it as a second graph resource would mis-track layout.
                // prevColor rests in ShaderReadOnly between frames (copy_color leaves it
                // there); seed that and DON'T write the layout back — the graph internally
                // transitions General for the copy write then back to ShaderReadOnly.
                RgResource prevColorRes = importImage(graph, renderer.targets.prevColor.image, renderer.targets.prevColor.view,
                    vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eShaderReadOnlyOptimal, nullptr);
                renderer.graph.prevColorResource = prevColorRes;
                RgResource ssgiRes = importImage(graph, renderer.targets.ssgiMap.image, renderer.targets.ssgiMap.view,
                    vk::ImageAspectFlagBits::eColor, renderer.targets.ssgiMap.layout, &renderer.targets.ssgiMap.layout);
                RgPass gipass;
                gipass.name = "ssgi";
                gipass.kind = RgPassKind::Compute;
                gipass.accesses = { RgAccess{ gNormalRes, RgUsage::SampledReadCompute },
                                    RgAccess{ prevColorRes, RgUsage::SampledReadCompute },
                                    RgAccess{ ssgiRes, RgUsage::StorageImageRWCompute } };
                const f32 radius = renderer.ssao.radius;
                gipass.execute = [&renderer, ssExtent, ssGroups, proj, invProj, radius](vk::CommandBuffer cmd)
        {
                    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.ssgi->pipeline);
                    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.ssgi->layout, 0, renderer.ssao.ssgiSet, {});
                    struct { glm::mat4 projection; glm::mat4 invProjection; glm::vec4 params; }
                        push{ proj, invProj, glm::vec4(radius * 2.0f, 1.0f, 8.0f, 0.0f) };
                    cmd.pushConstants(renderer.pipelines.ssgi->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
                    cmd.dispatch(ssGroups(ssExtent.width), ssGroups(ssExtent.height), 1);
                };
                addPass(graph, std::move(gipass));
                renderer.graph.ssgiResource = ssgiRes;
                renderer.graph.hasSsgi = true;
            }
        }

        // TAA motion vectors: a depth-tested prepass writing per-pixel screen motion from
        // camera reprojection. Produced before the scene so the TAA resolve (after the
        // scene) can read it; the graph derives ColorWrite -> SampledReadCompute.
        RgResource motionRes{};
        if (taa)
        {
            motionRes = importImage(graph, renderer.targets.motion.image, renderer.targets.motion.view,
                vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined, nullptr);
            RgResource motionDepthRes = importImage(graph, renderer.targets.motionDepth.image, renderer.targets.motionDepth.view,
                vk::ImageAspectFlagBits::eDepth, vk::ImageLayout::eUndefined, nullptr);
            RgPass motionPass;
            motionPass.name = "motion";
            motionPass.kind = RgPassKind::Graphics;
            motionPass.colors.push_back(RgAttachment{ motionRes, vk::AttachmentLoadOp::eClear,
                vk::AttachmentStoreOp::eStore, vk::ClearValue{ vk::ClearColorValue{ std::array<f32, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } } } });
            motionPass.depth = RgAttachment{ motionDepthRes, vk::AttachmentLoadOp::eClear,
                vk::AttachmentStoreOp::eStore, vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } } };
            motionPass.renderArea = offscreen.extent;
            motionPass.execute = [&renderer](vk::CommandBuffer cmd)
        {
                recordMotion(renderer, cmd);
            };
            addPass(graph, std::move(motionPass));
        }

        // DDGI: voxelize the scene proxy -> trace probe rays (software) -> blend irradiance +
        // distance atlases -> octahedral border copy. All compute, before the scene pass that
        // samples the atlases. The voxel image lives in GENERAL (storage read+write); the
        // atlases ping General (blend/border write) <-> ShaderReadOnly (trace prev-read + mesh).
        const bool doDdgi = renderer.ddgi.useDdgi && renderer.ddgi.ready &&
                            renderer.pipelines.ddgiVoxelize && renderer.pipelines.ddgiTrace &&
                            renderer.pipelines.ddgiBlendIrr && renderer.pipelines.ddgiBlendDist &&
                            renderer.pipelines.ddgiBorder;
        if (doDdgi)
        {
            Ddgi& d = renderer.ddgi;
            const glm::uvec4 probeCount{ DdgiProbesX, DdgiProbesY, DdgiProbesZ, DdgiRaysPerProbe };
            const u32 probeTotal = DdgiProbesX * DdgiProbesY * DdgiProbesZ;
            RgResource voxelRes = importImage3D(graph, d.voxels.image, d.voxels.view, d.voxels.layout, &d.voxels.layout);
            RgResource rayRes = importImage(graph, d.rays.image, d.rays.view,
                vk::ImageAspectFlagBits::eColor, d.rays.layout, &d.rays.layout);
            RgResource irrRes = importImage(graph, d.irradiance.image, d.irradiance.view,
                vk::ImageAspectFlagBits::eColor, d.irradiance.layout, &d.irradiance.layout);
            RgResource distRes = importImage(graph, d.distance.image, d.distance.view,
                vk::ImageAspectFlagBits::eColor, d.distance.layout, &d.distance.layout);
            renderer.graph.ddgiIrradiance = irrRes;
            renderer.graph.ddgiDistance = distRes;
            renderer.graph.hasDdgi = true;

            const u32 g3 = (DdgiVoxelRes + 3) / 4;
            const glm::vec3 volMin = d.volumeMin;
            const glm::vec3 volExt = d.volumeExtent;
            const glm::vec3 sunDir = d.sunDir;
            const glm::vec3 sunColor = d.sunColor;
            const f32 sunI = d.sunIntensity;
            const glm::vec3 sky = d.skyColor;
            const u32 boxCount = d.frameBoxCount;
            const f32 frameIdx = static_cast<f32>(d.frameIndex);
            const u32 firstFrame = d.historyReset ? 1u : 0u;
            const f32 maxDist = glm::length(volExt);

            // 1. Voxelize.
            RgPass vox;
            vox.name = "ddgi-voxelize";
            vox.kind = RgPassKind::Compute;
            vox.accesses = { RgAccess{ voxelRes, RgUsage::StorageImageRWCompute } };
            vox.execute = [&renderer, g3, probeCount, volMin, volExt, boxCount](vk::CommandBuffer cmd)
        {
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.ddgiVoxelize->pipeline);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.ddgiVoxelize->layout, 0, renderer.ddgi.voxelSet, {});
                struct { glm::uvec4 gridCount; glm::vec4 volumeMin; glm::vec4 volumeExtent; }
                    push{ glm::uvec4(DdgiVoxelRes, DdgiVoxelRes, DdgiVoxelRes, boxCount),
                          glm::vec4(volMin, 0.0f), glm::vec4(volExt, 0.0f) };
                cmd.pushConstants(renderer.pipelines.ddgiVoxelize->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
                cmd.dispatch(g3, g3, g3);
            };
            addPass(graph, std::move(vox));

            // 2. Trace (reads voxel storage + prev irradiance sampler -> ray storage).
            RgPass trace;
            trace.name = "ddgi-trace";
            trace.kind = RgPassKind::Compute;
            // Voxel read uses the image RW-storage usage (GENERAL) — StorageReadCompute is
            // modeled for buffers (layout Undefined) and would mis-transition a 3D image.
            trace.accesses = { RgAccess{ voxelRes, RgUsage::StorageImageRWCompute },
                               RgAccess{ irrRes, RgUsage::SampledReadCompute },
                               RgAccess{ rayRes, RgUsage::StorageImageRWCompute } };
            trace.execute = [&renderer, probeCount, probeTotal, volMin, volExt, sunDir, sunColor, sunI, sky, frameIdx](vk::CommandBuffer cmd)
        {
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.ddgiTrace->pipeline);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.ddgiTrace->layout, 0, renderer.ddgi.traceSet, {});
                struct {
                    glm::uvec4 probeCount; glm::uvec4 gridCount; glm::vec4 volumeMin; glm::vec4 volumeExtent;
                    glm::vec4 sunDir; glm::vec4 sunColor; glm::vec4 skyColor;
                } push{ probeCount, glm::uvec4(DdgiVoxelRes, DdgiVoxelRes, DdgiVoxelRes, DdgiIrrInterior),
                        glm::vec4(volMin, 0.0f), glm::vec4(volExt, 0.0f),
                        glm::vec4(sunDir, sunI), glm::vec4(sunColor, frameIdx), glm::vec4(sky, 0.0f) };
                cmd.pushConstants(renderer.pipelines.ddgiTrace->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
                cmd.dispatch((DdgiRaysPerProbe + 63) / 64, probeTotal, 1);
            };
            addPass(graph, std::move(trace));

            // 3. Blend irradiance (ray sampler -> irradiance storage).
            RgPass bi;
            bi.name = "ddgi-blend-irr";
            bi.kind = RgPassKind::Compute;
            bi.accesses = { RgAccess{ rayRes, RgUsage::SampledReadCompute },
                            RgAccess{ irrRes, RgUsage::StorageImageRWCompute } };
            const u32 irrAtlasW = DdgiProbesX * DdgiProbesY * (DdgiIrrInterior + 2);
            const u32 irrAtlasH = DdgiProbesZ * (DdgiIrrInterior + 2);
            bi.execute = [&renderer, probeCount, firstFrame, irrAtlasW, irrAtlasH](vk::CommandBuffer cmd)
        {
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.ddgiBlendIrr->pipeline);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.ddgiBlendIrr->layout, 0, renderer.ddgi.blendIrrSet, {});
                struct { glm::uvec4 probeCount; glm::uvec4 tile; glm::vec4 params; }
                    push{ probeCount, glm::uvec4(DdgiIrrInterior, firstFrame, 0, 0), glm::vec4(DdgiHysteresis, 0, 0, 0) };
                cmd.pushConstants(renderer.pipelines.ddgiBlendIrr->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
                cmd.dispatch((irrAtlasW + 7) / 8, (irrAtlasH + 7) / 8, 1);
            };
            addPass(graph, std::move(bi));

            // 4. Blend distance (ray sampler -> moment storage).
            RgPass bd;
            bd.name = "ddgi-blend-dist";
            bd.kind = RgPassKind::Compute;
            bd.accesses = { RgAccess{ rayRes, RgUsage::SampledReadCompute },
                            RgAccess{ distRes, RgUsage::StorageImageRWCompute } };
            const u32 distAtlasW = DdgiProbesX * DdgiProbesY * (DdgiDistInterior + 2);
            const u32 distAtlasH = DdgiProbesZ * (DdgiDistInterior + 2);
            bd.execute = [&renderer, probeCount, firstFrame, maxDist, distAtlasW, distAtlasH](vk::CommandBuffer cmd)
        {
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.ddgiBlendDist->pipeline);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.ddgiBlendDist->layout, 0, renderer.ddgi.blendDistSet, {});
                struct { glm::uvec4 probeCount; glm::uvec4 tile; glm::vec4 params; }
                    push{ probeCount, glm::uvec4(DdgiDistInterior, firstFrame, 0, 0), glm::vec4(DdgiHysteresis, maxDist, 0, 0) };
                cmd.pushConstants(renderer.pipelines.ddgiBlendDist->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
                cmd.dispatch((distAtlasW + 7) / 8, (distAtlasH + 7) / 8, 1);
            };
            addPass(graph, std::move(bd));

            // 5. Border copy (fix irradiance octahedral gutters) -> leaves irradiance in
            // ShaderReadOnly for the scene pass via the scene's SampledRead declaration.
            RgPass bo;
            bo.name = "ddgi-border";
            bo.kind = RgPassKind::Compute;
            bo.accesses = { RgAccess{ irrRes, RgUsage::StorageImageRWCompute } };
            bo.execute = [&renderer, probeCount, irrAtlasW, irrAtlasH](vk::CommandBuffer cmd)
        {
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.ddgiBorder->pipeline);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.ddgiBorder->layout, 0, renderer.ddgi.borderSet, {});
                struct { glm::uvec4 probeCount; glm::uvec4 tile; }
                    push{ probeCount, glm::uvec4(DdgiIrrInterior, 0, 0, 0) };
                cmd.pushConstants(renderer.pipelines.ddgiBorder->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
                cmd.dispatch((irrAtlasW + 7) / 8, (irrAtlasH + 7) / 8, 1);
            };
            addPass(graph, std::move(bo));

            renderer.ddgi.frameIndex = renderer.ddgi.frameIndex + 1;
            renderer.ddgi.historyReset = false;
        }
        else
        {
            renderer.graph.hasDdgi = false;
        }

        // RT: build the per-frame TLAS over the scene's mesh instances (a Compute-kind pass;
        // recordTlasBuild self-manages the AS-build -> fragment ray-query barrier). The mesh
        // fragment then traces inline ray-query shadows when rtShadows is on.
        renderer.rt.tlasReady = false;
        if (renderer.rt.buildPending && renderer.pipelines.cull /*any frame work*/)
        {
            RgPass tlasPass;
            tlasPass.name = "tlas-build";
            tlasPass.kind = RgPassKind::Compute;
            tlasPass.execute = [&renderer](vk::CommandBuffer cmd)
        {
                buildTlas(renderer, cmd, renderer.rt.frameModels, renderer.rt.frameMeshes);
            };
            addPass(graph, std::move(tlasPass));
        }

        // ReSTIR DI: initial candidate sampling -> temporal+spatial reuse -> resolve (1 shadow
        // ray) + shade, writing a per-pixel direct-radiance image the scene pass samples.
        // Needs the G-buffer (forced on below), the per-frame light + cluster SSBOs, and the
        // TLAS. Three compute passes; the reservoir SSBOs serialize via memory barriers the
        // graph derives from StorageWrite->StorageRead on a representative resource.
        renderer.graph.hasRestir = false;
        const bool doRestir = renderer.restir.useRestir && renderer.restir.ready &&
                              renderer.context.rtSupported && renderer.rt.tlasReady &&
                              renderer.graph.hasGbuffer && doCull /* froxel candidate lists */;
        if (doRestir)
        {
            // Per-frame writes: light SSBO + cluster SSBO (they grow), G-buffer + motion
            // samplers, and the TLAS into the resolve set.
            writeRestirFrameBindings(renderer, f);
            RgResource radianceRes = importImage(graph, renderer.restir.radiance.image, renderer.restir.radiance.view,
                vk::ImageAspectFlagBits::eColor, renderer.restir.radiance.layout, &renderer.restir.radiance.layout);
            // A sentinel buffer access so consecutive ReSTIR passes get RAW barriers on the
            // reservoir storage (initial->reuse->resolve write/read the same buffers).
            RgResource resvSentinel = importBuffer(graph, renderer.restir.combined->buffer);

            const vk::Extent2D ex = offscreen.extent;
            const glm::mat4 invView = glm::inverse(renderer.ssao.view);
            const glm::mat4 invProj = renderer.ssao.invProjection;
            const u32 fi = renderer.restir.frameIndex;
            const bool histValid = !renderer.restir.historyReset;

            RgPass init;
            init.name = "restir-initial";
            init.kind = RgPassKind::Compute;
            init.accesses = { RgAccess{ resvSentinel, RgUsage::StorageWriteCompute } };
            init.execute = [&renderer, ex, invView, invProj, fi](vk::CommandBuffer cmd)
        {
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.restirInitial->pipeline);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.restirInitial->layout, 0, renderer.restir.initialSet, {});
                struct { glm::mat4 invView; glm::mat4 invProjection; glm::uvec4 gridSize; glm::uvec4 screenSize; glm::vec4 zPlanes; }
                    push{ invView, invProj,
                          glm::uvec4(ClusterGridX, ClusterGridY, ClusterGridZ, renderer.lighting.frameLightCount),
                          glm::uvec4(ex.width, ex.height, renderer.restir.candidateCount, fi),
                          glm::vec4(0.1f, 100.0f, 0.0f, 0.0f) };
                cmd.pushConstants(renderer.pipelines.restirInitial->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
                cmd.dispatch((ex.width + 7) / 8, (ex.height + 7) / 8, 1);
            };
            addPass(graph, std::move(init));

            RgPass reuse;
            reuse.name = "restir-reuse";
            reuse.kind = RgPassKind::Compute;
            reuse.accesses = { RgAccess{ resvSentinel, RgUsage::StorageReadCompute } };
            reuse.execute = [&renderer, ex, invView, invProj, fi, histValid](vk::CommandBuffer cmd)
        {
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.restirReuse->pipeline);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.restirReuse->layout, 0, renderer.restir.reuseSet, {});
                struct { glm::mat4 invView; glm::mat4 invProjection; glm::uvec4 screenSize; glm::vec4 params; }
                    push{ invView, invProj, glm::uvec4(ex.width, ex.height, 20u, fi),
                          glm::vec4(16.0f, histValid ? 1.0f : 0.0f, 0.0f, 0.0f) };
                cmd.pushConstants(renderer.pipelines.restirReuse->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
                cmd.dispatch((ex.width + 7) / 8, (ex.height + 7) / 8, 1);
            };
            addPass(graph, std::move(reuse));

            RgPass resolve;
            resolve.name = "restir-resolve";
            resolve.kind = RgPassKind::Compute;
            resolve.accesses = { RgAccess{ resvSentinel, RgUsage::StorageReadCompute },
                                 RgAccess{ radianceRes, RgUsage::StorageImageRWCompute } };
            const glm::vec3 eye = glm::vec3(invView[3]);
            resolve.execute = [&renderer, ex, invView, invProj, eye](vk::CommandBuffer cmd)
        {
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.restirResolve->pipeline);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.restirResolve->layout, 0, renderer.restir.resolveSet, {});
                struct { glm::mat4 invView; glm::mat4 invProjection; glm::uvec4 screenSize; glm::vec4 eyePosition; }
                    push{ invView, invProj, glm::uvec4(ex.width, ex.height, 0, 0), glm::vec4(eye, 0.0f) };
                cmd.pushConstants(renderer.pipelines.restirResolve->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
                cmd.dispatch((ex.width + 7) / 8, (ex.height + 7) / 8, 1);
            };
            addPass(graph, std::move(resolve));
            renderer.graph.restirRadiance = radianceRes;
            renderer.graph.hasRestir = true;
            renderer.restir.frameIndex = renderer.restir.frameIndex + 1;
            renderer.restir.historyReset = false;
        }

        // Visible sky: a fullscreen pass that fills the scene color target before the geometry.
        // It writes the SAME target the scene pass uses (offscreen / msaaColor / scratch), so
        // the MSAA resolve + FXAA/TAA filtering treat sky and geometry alike. When present it
        // owns the color clear, and the scene pass loads instead of clearing.
        const bool doSky = renderer.sky.visible && renderer.sky.ready && renderer.sky.pipeline;
        if (doSky)
        {
            RgPass skyPass;
            skyPass.name = "sky";
            skyPass.kind = RgPassKind::Graphics;
            skyPass.colors.push_back(RgAttachment{ sceneColorAttachment, vk::AttachmentLoadOp::eClear,
                vk::AttachmentStoreOp::eStore, vk::ClearValue{ vk::ClearColorValue{ renderer.frame.clearColor } } });
            skyPass.renderArea = offscreen.extent;
            skyPass.execute = [&renderer](vk::CommandBuffer cmd) { recordSky(renderer, cmd); };
            addPass(graph, std::move(skyPass));
        }

        RgPass scene;
        scene.name = "scene";
        scene.kind = RgPassKind::Graphics;
        if (doCull)
        {
            scene.accesses = { RgAccess{ clusterBuffer, RgUsage::StorageReadFragment } };
        }
        if (renderer.graph.hasAo)
        {
            scene.accesses.push_back(RgAccess{ renderer.graph.aoResource, RgUsage::SampledRead });
        }
        if (renderer.graph.hasContact)
        {
            scene.accesses.push_back(RgAccess{ renderer.graph.contactResource, RgUsage::SampledRead });
        }
        if (renderer.graph.hasSsgi)
        {
            scene.accesses.push_back(RgAccess{ renderer.graph.ssgiResource, RgUsage::SampledRead });
        }
        if (renderer.graph.hasDdgi)
        {
            scene.accesses.push_back(RgAccess{ renderer.graph.ddgiIrradiance, RgUsage::SampledRead });
            scene.accesses.push_back(RgAccess{ renderer.graph.ddgiDistance, RgUsage::SampledRead });
        }
        if (renderer.graph.hasRestir)
        {
            scene.accesses.push_back(RgAccess{ renderer.graph.restirRadiance, RgUsage::SampledRead });
        }
        if (doShadow)
        {
            scene.accesses.push_back(RgAccess{ shadowRes, RgUsage::SampledRead });
        }
        if (doSpotShadow)
        {
            scene.accesses.push_back(RgAccess{ spotShadowRes, RgUsage::SampledRead });
        }
        // MSAA: render to the multisampled color, resolve into the offscreen (don't store
        // the multisampled samples). Otherwise render straight into the offscreen.
        RgAttachment sceneColorAtt{ sceneColorAttachment,
            doSky ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eClear,
            vk::AttachmentStoreOp::eStore, vk::ClearValue{ vk::ClearColorValue{ renderer.frame.clearColor } } };
        if (msaa)
        {
            sceneColorAtt.storeOp = vk::AttachmentStoreOp::eDontCare;
            sceneColorAtt.resolve = sceneOutput;
        }
        scene.colors.push_back(sceneColorAtt);
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

        // TAA resolve: reproject history through the motion vector, neighborhood-clamp, and
        // blend with the current scene (scratch) into the offscreen + the next-frame history.
        // Parity p writes history[p]; the GTAO-style compute pass declares its image usages.
        if (taa)
        {
            const u32 p = renderer.targets.historyIndex;
            RgResource histReadRes = importImage(graph, renderer.targets.history[1 - p].image,
                renderer.targets.history[1 - p].view, vk::ImageAspectFlagBits::eColor,
                renderer.targets.history[1 - p].layout, &renderer.targets.history[1 - p].layout);
            RgResource histWriteRes = importImage(graph, renderer.targets.history[p].image,
                renderer.targets.history[p].view, vk::ImageAspectFlagBits::eColor,
                renderer.targets.history[p].layout, &renderer.targets.history[p].layout);
            RgPass taaPass;
            taaPass.name = "taa";
            taaPass.kind = RgPassKind::Compute;
            taaPass.accesses = { RgAccess{ sceneOutput, RgUsage::SampledReadCompute },
                                 RgAccess{ motionRes, RgUsage::SampledReadCompute },
                                 RgAccess{ histReadRes, RgUsage::SampledReadCompute },
                                 RgAccess{ renderer.graph.sceneColor, RgUsage::StorageImageRWCompute },
                                 RgAccess{ histWriteRes, RgUsage::StorageImageRWCompute } };
            const vk::Extent2D extent = offscreen.extent;
            const bool historyValid = renderer.targets.historyValid;
            taaPass.execute = [&renderer, extent, p, historyValid](vk::CommandBuffer cmd)
        {
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.taa->pipeline);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                    renderer.pipelines.taa->layout, 0, renderer.descriptors.taaSets[p], {});
                struct TaaPush
                {
                    glm::vec4 params;
                } push{ glm::vec4(TaaHistoryWeight, historyValid ? 1.0f : 0.0f, 0.0f, 0.0f) };
                cmd.pushConstants(renderer.pipelines.taa->layout, vk::ShaderStageFlagBits::eCompute,
                                  0, sizeof(push), &push);
                cmd.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
            };
            addPass(graph, std::move(taaPass));
            // Next frame reads this frame's history; mark it valid + flip parity.
            renderer.targets.historyValid = true;
            renderer.targets.historyIndex = 1 - p;
        }

        // SSGI history capture: copy the scene's resolved LINEAR-HDR color into prevColor
        // (before the in-place tonemap turns it display-referred) so next frame's SSGI can
        // gather it. Runs only when SSGI is on; the graph derives the offscreen
        // SampledRead + prevColor General transitions.
        if (renderer.graph.hasSsgi)
        {
            // Reuse the single prevColor handle imported by the SSGI pass (it was read
            // there; here it is written) so the graph tracks its layout across both.
            RgPass copyPass;
            copyPass.name = "ssgi-history";
            copyPass.kind = RgPassKind::Compute;
            copyPass.accesses = { RgAccess{ renderer.graph.sceneColor, RgUsage::SampledReadCompute },
                                  RgAccess{ renderer.graph.prevColorResource, RgUsage::StorageImageRWCompute } };
            const vk::Extent2D extent = offscreen.extent;
            copyPass.execute = [&renderer, extent, ssGroups](vk::CommandBuffer cmd)
        {
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.copyColor->pipeline);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                    renderer.pipelines.copyColor->layout, 0, renderer.ssao.copyColorSet, {});
                cmd.dispatch(ssGroups(extent.width), ssGroups(extent.height), 1);
            };
            addPass(graph, std::move(copyPass));
            // Restore prevColor to ShaderReadOnly (its resting layout) so next frame's SSGI
            // sampler read + this frame's seed agree. A no-attachment SampledRead pass would
            // be overkill; instead transition it directly after the graph (it's not used
            // again this frame). Done via a trailing one-off below in endFrame? No — keep it
            // local: declare a final compute SampledRead so the graph emits the transition.
            RgPass restorePass;
            restorePass.name = "ssgi-history-restore";
            restorePass.kind = RgPassKind::Compute;
            restorePass.accesses = { RgAccess{ renderer.graph.prevColorResource, RgUsage::SampledReadCompute } };
            restorePass.execute = [](vk::CommandBuffer) {};  // barrier-only: General -> ShaderReadOnly
            addPass(graph, std::move(restorePass));
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
        ui.colors.push_back(RgAttachment{ renderer.graph.swapImage, vk::AttachmentLoadOp::eClear,
            vk::AttachmentStoreOp::eStore, vk::ClearValue{ vk::ClearColorValue{ renderer.frame.clearColor } } });
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

        // Store this frame's camera viewProj as next frame's "previous" for TAA motion
        // vectors. Only valid once a scene draw list was submitted this frame.
        if (renderer.frame.sceneDrawList.valid)
        {
            renderer.prevViewProj = renderer.frame.sceneDrawList.viewProj;
            renderer.prevViewProjValid = true;
        }

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

    void setSsao(Renderer& renderer, bool enabled)
    {
        renderer.ssao.useSsao = enabled;
    }

    auto ssaoEnabled(const Renderer& renderer) -> bool
    {
        return renderer.ssao.useSsao && renderer.ssao.ready;
    }

    void setContactShadows(Renderer& renderer, bool enabled)
    {
        renderer.ssao.useContact = enabled;
    }

    auto contactShadowsEnabled(const Renderer& renderer) -> bool
    {
        return renderer.ssao.useContact && renderer.ssao.ready;
    }

    void setSsgi(Renderer& renderer, bool enabled)
    {
        renderer.ssao.useSsgi = enabled;
    }

    auto ssgiEnabled(const Renderer& renderer) -> bool
    {
        return renderer.ssao.useSsgi && renderer.ssao.ready;
    }

    auto screenEffectsEnabled(const Renderer& renderer) -> bool
    {
        return renderer.ssao.ready &&
               (renderer.ssao.useSsao || renderer.ssao.useContact || renderer.ssao.useSsgi);
    }

    auto rtSupported(const Renderer& renderer) -> bool
    {
        return renderer.context.rtSupported;
    }

    void setRtShadows(Renderer& renderer, bool enabled)
    {
        renderer.rt.useRtShadows = enabled && renderer.context.rtSupported;
    }

    auto rtShadowsEnabled(const Renderer& renderer) -> bool
    {
        return renderer.rt.useRtShadows && renderer.context.rtSupported && renderer.rt.tlasReady;
    }

    auto rtBlasCount(const Renderer& renderer) -> u32
    {
        return renderer.rt.blasCount;
    }

    void setRtScene(Renderer& renderer, std::vector<glm::mat4> models, std::vector<Ref<GpuMesh>> meshes)
    {
        // Capture this frame's instances; the TLAS-build graph pass consumes them when RT
        // shadows are on. Only worth building when something will trace against it.
        renderer.rt.frameModels = std::move(models);
        renderer.rt.frameMeshes = std::move(meshes);
        renderer.rt.buildPending = renderer.context.rtSupported && renderer.rt.useRtShadows &&
                                   !renderer.rt.frameModels.empty();
    }

    void buildTlas(Renderer& renderer, vk::CommandBuffer cmd,
                   const std::vector<glm::mat4>& models, const std::vector<Ref<GpuMesh>>& meshes)
    {
        renderer.rt.tlasReady = false;
        if (!renderer.context.rtSupported || models.empty())
        {
            return;
        }
        const u32 f = renderer.frame.index;
        // Pack one VkAccelerationStructureInstanceKHR per mesh instance that has a BLAS.
        std::vector<VkAccelerationStructureInstanceKHR> instances;
        instances.reserve(models.size());
        for (std::size_t i = 0; i < models.size() && i < meshes.size(); i = i + 1)
        {
            if (!meshes[i] || !meshes[i]->blas)
            {
                continue;
            }
            VkAccelerationStructureInstanceKHR inst{};
            // VkTransformMatrixKHR is row-major 3x4; glm is column-major — transpose into rows.
            const glm::mat4& m = models[i];
            for (u32 r = 0; r < 3; r = r + 1)
            {
                for (u32 c = 0; c < 4; c = c + 1)
                {
                    inst.transform.matrix[r][c] = m[c][r];
                }
            }
            inst.instanceCustomIndex = static_cast<u32>(i);
            inst.mask = 0xFF;
            inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            inst.accelerationStructureReference = static_cast<u64>(meshes[i]->blas->address);
            instances.push_back(inst);
        }
        const u32 count = static_cast<u32>(instances.size());
        if (count == 0)
        {
            return;
        }
        if (Result<void> ok = ensureTlasCapacity(renderer, f, count); !ok)
        {
            logError(ok.error());
            return;
        }
        std::memcpy(renderer.rt.instanceBuffers[f]->mapped, instances.data(),
                    count * sizeof(VkAccelerationStructureInstanceKHR));
        vmaFlushAllocation(renderer.context.allocator, renderer.rt.instanceBuffers[f]->alloc, 0,
                           count * sizeof(VkAccelerationStructureInstanceKHR));
        recordTlasBuild(renderer, cmd, f, count);
        renderer.rt.frameInstanceCount = count;
        renderer.rt.tlasReady = true;
    }

    void setRestir(Renderer& renderer, bool enabled)
    {
        const bool on = enabled && renderer.context.rtSupported && renderer.restir.ready;
        if (on && !renderer.restir.useRestir)
        {
            renderer.restir.historyReset = true;
        }
        renderer.restir.useRestir = on;
    }

    auto restirEnabled(const Renderer& renderer) -> bool
    {
        return renderer.restir.useRestir && renderer.context.rtSupported && renderer.restir.ready;
    }

    void setDdgi(Renderer& renderer, bool enabled)
    {
        if (enabled && !renderer.ddgi.useDdgi)
        {
            renderer.ddgi.historyReset = true;  // re-converge probes from scratch on enable
        }
        renderer.ddgi.useDdgi = enabled;
    }

    auto ddgiEnabled(const Renderer& renderer) -> bool
    {
        return renderer.ddgi.useDdgi && renderer.ddgi.ready;
    }

    void setDdgiScene(Renderer& renderer, const std::vector<glm::vec4>& boxMins,
                      const std::vector<glm::vec4>& boxMaxs, const std::vector<glm::vec4>& boxAlbedos,
                      glm::vec3 volumeMin, glm::vec3 volumeExtent,
                      glm::vec3 sunDir, glm::vec3 sunColor, f32 sunIntensity, glm::vec3 skyColor)
    {
        Ddgi& d = renderer.ddgi;
        if (!d.ready || d.boxBuffer == nullptr)
        {
            return;
        }
        u32 count = static_cast<u32>(boxMins.size());
        if (count > d.boxCapacity)
        {
            count = d.boxCapacity;
        }
        // Interleave [min, max, albedo] per box into the mapped SSBO (matches the Box struct).
        auto* dst = static_cast<glm::vec4*>(d.boxBuffer->mapped);
        for (u32 i = 0; i < count; i = i + 1)
        {
            dst[i * 3 + 0] = boxMins[i];
            dst[i * 3 + 1] = boxMaxs[i];
            dst[i * 3 + 2] = boxAlbedos[i];
        }
        if (count > 0)
        {
            vmaFlushAllocation(renderer.context.allocator, d.boxBuffer->alloc, 0,
                               static_cast<vk::DeviceSize>(count) * 3 * sizeof(glm::vec4));
        }
        d.frameBoxCount = count;
        d.volumeMin = volumeMin;
        d.volumeExtent = volumeExtent;
        d.sunDir = sunDir;
        d.sunColor = sunColor;
        d.sunIntensity = sunIntensity;
        d.skyColor = skyColor;
    }

    void setSsaoCamera(Renderer& renderer, const glm::mat4& view, const glm::mat4& proj,
                       glm::vec3 sunDirectionWorld)
    {
        renderer.ssao.view = view;
        renderer.ssao.viewProj = proj * view;
        renderer.ssao.projection = proj;
        renderer.ssao.invProjection = glm::inverse(proj);
        // Contact shadows march toward the sun; the G-buffer is view space, so transform
        // the (incoming) light direction into view space. directionWorld points the way the
        // light travels, so the direction TO the light is its negation.
        renderer.ssao.sunDirView = glm::normalize(glm::vec3(view * glm::vec4(-sunDirectionWorld, 0.0f)));
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

    void setSpotShadow(Renderer& renderer, const glm::mat4& lightViewProj, u32 lightIndex, bool casting)
    {
        renderer.lighting.spotShadowViewProj = lightViewProj;
        renderer.lighting.spotShadowLightIndex = lightIndex;
        renderer.lighting.spotShadowPending = casting && renderer.lighting.useShadows;
    }

    void setPointShadow(Renderer& renderer, glm::vec3 lightPos, f32 farPlane, u32 lightIndex, bool casting)
    {
        renderer.lighting.pointShadowPos = lightPos;
        renderer.lighting.pointShadowFar = farPlane;
        renderer.lighting.pointShadowLightIndex = lightIndex;
        renderer.lighting.pointShadowPending = casting && renderer.lighting.useShadows;
    }

    void waitGpuIdle(Renderer& renderer)
    {
        if (renderer.context.device)
        {
            static_cast<void>(renderer.context.device.waitIdle());
        }
    }

}
