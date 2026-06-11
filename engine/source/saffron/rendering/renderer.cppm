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

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

export module Saffron.Rendering;

export import :RenderGraph;
export import :Types;

import Saffron.Core;
import Saffron.Window;
import Saffron.Geometry;
import :Detail;

namespace se
{
    // Host-mapped vertex buffer for the editor overlay (grown per frame by the overlay pass).
    auto makeMappedVertexBuffer(Renderer& renderer, vk::DeviceSize bytes) -> Result<Ref<Buffer>>;

    // GPU-profiler timestamp pools, freed in destroyRenderer (defined below beginFrame).
    void destroyProfilerPools(Renderer& renderer);

    namespace
    {
        // Funnels loader + validation-layer messages into the engine log as one line:
        // `[saffron:vulkan] <level>: [<kind>] <id>: <message>`. Two known-noise classes
        // are dropped unless SAFFRON_VK_VERBOSE=1: general-only warnings (loader ICD
        // probing, e.g. a skipped driver) and OutputNotConsumed performance warnings —
        // depth-only and sky pipelines bind the full mesh vertex layout by design.
        VKAPI_ATTR VkBool32 VKAPI_CALL onVulkanMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                       VkDebugUtilsMessageTypeFlagsEXT type,
                                                       const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                       void* /*userData*/)
        {
            static const bool verbose = std::getenv("SAFFRON_VK_VERBOSE") != nullptr;
            std::string_view id;
            if (data->pMessageIdName != nullptr)
            {
                id = data->pMessageIdName;
            }
            const bool loaderChatter = type == VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT &&
                                       severity < VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            if (!verbose && (loaderChatter || id.contains("OutputNotConsumed")))
            {
                return VK_FALSE;
            }

            LogLevel level = LogLevel::Info;
            if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
            {
                level = LogLevel::Error;
            }
            else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
            {
                level = LogLevel::Warn;
            }

            std::string_view kind = "general";
            if ((type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0)
            {
                kind = "validation";
            }
            else if ((type & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) != 0)
            {
                kind = "performance";
            }

            std::string_view message;
            if (data->pMessage != nullptr)
            {
                message = data->pMessage;
            }
            if (id.empty())
            {
                log(level, "vulkan", std::format("[{}] {}", kind, message));
            }
            else
            {
                log(level, "vulkan", std::format("[{}] {}: {}", kind, id, message));
            }
            return VK_FALSE;
        }
    }

    auto newRenderer(Window& window) -> Result<Renderer>
    {
        Renderer renderer;
        renderer.window = &window;

        u32 sdlExtensionCount = 0;
        const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);

        vkb::InstanceBuilder instanceBuilder;
        instanceBuilder.set_app_name("Saffron Editor")
            .set_engine_name("Saffron Engine")
            .require_api_version(1, 4, 0)
            .request_validation_layers(true)
            .set_debug_callback(onVulkanMessage);
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

        VkPhysicalDeviceVulkan14Features features14{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES };

        // Bindless: one global texture array indexed per-instance. Core Vulkan 1.2
        // descriptor indexing — required (selection fails with a clear error if absent).
        VkPhysicalDeviceVulkan12Features features12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        features12.runtimeDescriptorArray = VK_TRUE;
        features12.descriptorBindingPartiallyBound = VK_TRUE;
        features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        features12.bufferDeviceAddress = VK_TRUE;  // required by KHR acceleration structures

        vkb::PhysicalDeviceSelector selector{ renderer.context.vkbInstance };
        auto physicalResult = selector.set_minimum_version(1, 4)
                                  .set_required_features_11(features11)
                                  .set_required_features_12(features12)
                                  .set_required_features_13(features13)
                                  .set_required_features_14(features14)
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
        const bool hasAsExt =
            physicalResult.value().enable_extension_if_present(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        const bool hasRqExt = physicalResult.value().enable_extension_if_present(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        physicalResult.value().enable_extension_if_present(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);

        // VK_EXT_memory_budget feeds vmaGetHeapBudgets (the per-frame VRAM telemetry); opt-in,
        // off-by-default. pipelineStatisticsQuery is the deepest profiler level's input — both
        // are optional, never gating device selection.
        const bool hasMemoryBudget =
            physicalResult.value().enable_extension_if_present(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        // VK_EXT_calibrated_timestamps samples the device + host clocks together so the profiler
        // can project GPU spans onto the CPU steady_clock axis (one merged timeline). Optional;
        // absent => the profiler keeps GPU spans on their own axis (correlated = false).
        const bool hasCalibratedTs =
            physicalResult.value().enable_extension_if_present(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        VkPhysicalDeviceFeatures pipelineStatsFeat{};
        pipelineStatsFeat.pipelineStatisticsQuery = VK_TRUE;
        const bool hasPipelineStats = physicalResult.value().enable_features_if_present(pipelineStatsFeat);
        bool rtSupported = false;
        if (hasAsExt && hasRqExt)
        {
            VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeat{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR
            };
            VkPhysicalDeviceRayQueryFeaturesKHR rqFeat{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
            asFeat.pNext = &rqFeat;
            VkPhysicalDeviceFeatures2 feat2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            feat2.pNext = &asFeat;
            vkGetPhysicalDeviceFeatures2(physicalResult.value().physical_device, &feat2);
            rtSupported = asFeat.accelerationStructure == VK_TRUE && rqFeat.rayQuery == VK_TRUE;
        }

        vkb::DeviceBuilder deviceBuilder{ physicalResult.value() };
        // Chain the RT feature structs into device creation only when supported.
        VkPhysicalDeviceAccelerationStructureFeaturesKHR asEnable{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR
        };
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
        renderer.context.graphicsQueueFamily =
            renderer.context.vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

        // Profiler capabilities, read once. Timestamps need a non-zero validBits on the
        // graphics queue; the period converts ticks→ns. A software rasterizer (llvmpipe/
        // lavapipe) reports GPU timings that are really CPU rasterization time — flag it so
        // downstream never draws hardware conclusions.
        {
            const vk::PhysicalDeviceProperties props = renderer.context.physicalDevice.getProperties();
            const std::vector<vk::QueueFamilyProperties> families =
                renderer.context.physicalDevice.getQueueFamilyProperties();
            const u32 validBits = families[renderer.context.graphicsQueueFamily].timestampValidBits;
            renderer.profiler.timestampPeriod = props.limits.timestampPeriod;
            renderer.profiler.timestampMask = validBits >= 64 ? ~0ULL : ((1ULL << validBits) - 1ULL);
            renderer.profiler.timestampsSupported = validBits != 0;
            renderer.profiler.pipelineStatsSupported = hasPipelineStats;
            // Opt-in nested GPU sub-scopes (the deeper, larger-pool capture). Phase 5 will drive
            // this from the capture's include-flags; until then an env var enables it.
            renderer.profiler.subScopes = std::getenv("SAFFRON_PROFILE_SUBSCOPES") != nullptr;

            std::string name = renderer.context.vkbDevice.physical_device.name;
            for (char& c : name)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            renderer.softwareGpu = name.find("llvmpipe") != std::string::npos ||
                                   name.find("lavapipe") != std::string::npos ||
                                   name.find("software") != std::string::npos;
        }

        // Resolve the KHR acceleration-structure / ray-query device entry points (not
        // statically exported by the loader; the engine otherwise uses static dispatch).
        renderer.context.rtSupported = rtSupported;
        if (rtSupported)
        {
            VkDevice dev = renderer.context.vkbDevice.device;
            auto load = [dev](const char* n) { return vkGetDeviceProcAddr(dev, n); };
            renderer.context.rt.getBuildSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
                load("vkGetAccelerationStructureBuildSizesKHR"));
            renderer.context.rt.createAccel =
                reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(load("vkCreateAccelerationStructureKHR"));
            renderer.context.rt.destroyAccel =
                reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(load("vkDestroyAccelerationStructureKHR"));
            renderer.context.rt.cmdBuild =
                reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(load("vkCmdBuildAccelerationStructuresKHR"));
            renderer.context.rt.getAccelAddress = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
                load("vkGetAccelerationStructureDeviceAddressKHR"));
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

        // VK_EXT_debug_utils command-buffer labels name every render-graph pass region in
        // external capture tools. The extension is enabled with the validation/debug-messenger
        // path above; its cmd-label functions are device-level commands of an instance
        // extension, so resolve them via the instance loader. Absent => both null => no markers.
        {
            VkInstance inst = renderer.context.vkbInstance.instance;
            renderer.context.debugLabels.begin = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
                vkGetInstanceProcAddr(inst, "vkCmdBeginDebugUtilsLabelEXT"));
            renderer.context.debugLabels.end = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
                vkGetInstanceProcAddr(inst, "vkCmdEndDebugUtilsLabelEXT"));
            if (renderer.context.debugLabels.begin == nullptr || renderer.context.debugLabels.end == nullptr)
            {
                renderer.context.debugLabels = {};  // both or neither
            }
        }

        // VK_EXT_calibrated_timestamps: resolve its entry points and confirm both a device domain
        // and the host CLOCK_MONOTONIC domain (what libc++ steady_clock samples) are calibrateable.
        // Only then can readbackGpuTimings project GPU spans onto the CPU axis; otherwise correlation
        // stays off and GPU spans keep their own frame-relative zero. The env var forces that
        // own-axis fallback (for testing it on hardware that does support the extension).
        if (hasCalibratedTs && std::getenv("SAFFRON_DISABLE_CALIBRATION") == nullptr)
        {
            VkInstance inst = renderer.context.vkbInstance.instance;
            renderer.context.calibratedTs.getDomains =
                reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(
                    vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
            renderer.context.calibratedTs.getTimestamps = reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(
                vkGetInstanceProcAddr(inst, "vkGetCalibratedTimestampsEXT"));
            if (renderer.context.calibratedTs.getDomains != nullptr &&
                renderer.context.calibratedTs.getTimestamps != nullptr)
            {
                u32 domainCount = 0;
                renderer.context.calibratedTs.getDomains(renderer.context.physicalDevice, &domainCount, nullptr);
                std::vector<VkTimeDomainEXT> domains(domainCount);
                renderer.context.calibratedTs.getDomains(renderer.context.physicalDevice, &domainCount, domains.data());
                const bool hasDevice =
                    std::find(domains.begin(), domains.end(), VK_TIME_DOMAIN_DEVICE_EXT) != domains.end();
                const bool hasMonotonic =
                    std::find(domains.begin(), domains.end(), VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT) != domains.end();
                if (hasDevice && hasMonotonic)
                {
                    renderer.profiler.calibration.available = true;
                    renderer.profiler.calibration.hostDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
                }
            }
            logInfo(renderer.profiler.calibration.available
                        ? "calibrated timestamps available — GPU spans correlate to the CPU clock"
                        : "calibrated timestamps unavailable — GPU spans stay on their own axis");
        }

        // Sample counts valid for the MSAA targets: the framebuffer limits intersected with each
        // target format's own imageCreateSampleCounts. The generic limits are not enough — llvmpipe,
        // e.g., accepts 2x as a framebuffer count yet rejects it for R16G16B16A16_SFLOAT / D32_SFLOAT.
        vk::SampleCountFlags sampleCounts =
            renderer.context.physicalDevice.getProperties().limits.framebufferColorSampleCounts &
            renderer.context.physicalDevice.getProperties().limits.framebufferDepthSampleCounts;
        auto colorFmt = renderer.context.physicalDevice.getImageFormatProperties(
            OffscreenColorFormat, vk::ImageType::e2D, vk::ImageTiling::eOptimal,
            vk::ImageUsageFlagBits::eColorAttachment, {});
        if (colorFmt.result == vk::Result::eSuccess)
        {
            sampleCounts = sampleCounts & colorFmt.value.sampleCounts;
        }
        auto depthFmt = renderer.context.physicalDevice.getImageFormatProperties(
            DepthFormat, vk::ImageType::e2D, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eDepthStencilAttachment,
            {});
        if (depthFmt.result == vk::Result::eSuccess)
        {
            sampleCounts = sampleCounts & depthFmt.value.sampleCounts;
        }
        renderer.targets.supportedSampleCounts = sampleCounts;
        if (sampleCounts & vk::SampleCountFlagBits::e8)
        {
            renderer.targets.maxSampleCount = vk::SampleCountFlagBits::e8;
        }
        else if (sampleCounts & vk::SampleCountFlagBits::e4)
        {
            renderer.targets.maxSampleCount = vk::SampleCountFlagBits::e4;
        }
        else if (sampleCounts & vk::SampleCountFlagBits::e2)
        {
            renderer.targets.maxSampleCount = vk::SampleCountFlagBits::e2;
        }

        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.instance = renderer.context.vkbInstance.instance;
        allocatorInfo.physicalDevice = physicalResult.value().physical_device;
        allocatorInfo.device = renderer.context.vkbDevice.device;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_4;
        if (renderer.context.rtSupported)
        {
            // BDA is needed to feed vertex/index/instance buffer addresses to AS builds.
            allocatorInfo.flags = allocatorInfo.flags | VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
        }
        if (hasMemoryBudget)
        {
            // Lets vmaGetHeapBudgets report driver-reported usage/budget (not just VMA's own).
            allocatorInfo.flags = allocatorInfo.flags | VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
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

            auto imageAvailable =
                checked(renderer.context.device.createSemaphore(vk::SemaphoreCreateInfo{}), "createSemaphore");
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
        // Seed every reflection-probe slot (IBL set, bindings 3-5) with the global IBL cubes so the bind is
        // valid before any probe is captured; real probes overwrite their slot on capture.
        seedReflectionProbeSet(renderer);
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
            std::vector<vk::DescriptorImageInfo> infos(
                MaxBindlessTextures, vk::DescriptorImageInfo{ renderer.descriptors.linearSampler, (*whiteTexture)->view,
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
                            renderer.context.vkbDevice.physical_device.name, renderer.swapchain.images.size()));
        return renderer;
    }

    void destroyRenderer(Renderer& renderer)
    {
        if (renderer.context.device)
        {
            static_cast<void>(renderer.context.device.waitIdle());
        }

        destroyShmPublish(renderer);  // slot images/buffers + the shm segment

        // Drop any Refs the renderer itself still holds, plus the closure vectors
        // (which may capture Refs), before the descriptor pool / allocator / device
        // are torn down — a GpuTexture frees its material set from the pool.
        renderer.frame.sceneDrawList = SceneDrawList{};  // drops mesh/texture/pipeline Refs
        renderer.pipelines.cache.clear();                // drops the cached mesh PSOs
        renderer.frame.sceneSubmissions.clear();
        renderer.frame.uiSubmissions.clear();
        renderer.defaultWhiteTexture.reset();
        renderer.previewSphere.reset();           // material-preview sphere mesh
        renderer.pipelines.cull.reset();          // RAII frees the compute pipeline + layout
        renderer.pipelines.overlay.reset();       // editor gizmo + billboard PSO
        renderer.pipelines.overlayDepth.reset();  // depth-tested overlay PSO (frustums)
        renderer.pipelines.thumbnail.reset();
        renderer.pipelines.preview.reset();       // material-preview PSO
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
        renderer.sky.pipeline.reset();  // fullscreen sky PSO
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
            renderer.rt.blasScratchBuffers[i].reset();
            renderer.overlay.buffers[i].reset();  // host-mapped overlay vertex buffers
        }
        // The last frame's RT scene holds Ref<GpuMesh> captured by setRtScene; beginFrame
        // would clear it next frame, but there is no next frame at teardown. Drop them so the
        // meshes (vertex + index + BLAS buffers) free before the allocator, not after.
        renderer.rt.frameMeshes.clear();
        renderer.rt.frameModels.clear();
        // The per-skinned-instance refit BLASes (per slot, grow-only) must free their AS +
        // backing buffer before the allocator/device, like the per-frame TLAS above.
        for (u32 i = 0; i < MaxFramesInFlight; i = i + 1)
        {
            renderer.rt.skinnedBlas[i].clear();
        }
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
        renderer.ibl.transmittanceLut.reset();
        renderer.ibl.multiScatterLut.reset();
        renderer.ibl.skyViewLut.reset();
        renderer.ibl.irradianceCube.reset();
        renderer.ibl.prefilteredCube.reset();
        renderer.ibl.brdfLut.reset();
        renderer.ibl.envPanorama.reset();
        // Reflection probes: free each slot's cubes + per-face views, the metadata SSBO, and
        // the sampler before the allocator/device (the lazily-allocated probe cubes are the
        // easy-to-miss VMA-leak source — release them here, mirroring rt.frameMeshes).
        for (ReflectionProbe& probe : renderer.reflection.probes)
        {
            for (vk::ImageView& face : probe.faceViews)
            {
                if (face)
                {
                    renderer.context.device.destroyImageView(face);
                    face = nullptr;
                }
            }
            probe.envCube.reset();
            probe.envDepth.reset();
            probe.irradianceCube.reset();
            probe.prefilteredCube.reset();
        }
        renderer.reflection.metaBuffer.reset();
        if (renderer.reflection.sampler)
        {
            renderer.context.device.destroySampler(renderer.reflection.sampler);
            renderer.reflection.sampler = nullptr;
        }
        renderer.targets.msaaColor.reset();
        renderer.targets.msaaDepth.reset();
        renderer.targets.scratch.reset();

        for (u32 i = 0; i < MaxFramesInFlight; i = i + 1)
        {
            renderer.instancing.buffers[i].reset();  // RAII frees the SSBO before the allocator
            renderer.instancing.materialBuffers[i].reset();
            renderer.instancing.jointBuffers[i].reset();
            renderer.instancing.prevJointBuffers[i].reset();
            renderer.skinning.deformedBuffers[i].reset();
            renderer.skinning.prevDeformedBuffers[i].reset();
            renderer.lighting.lightListBuffers[i].reset();
            renderer.lighting.clusterBuffers[i].reset();
            if (renderer.lighting.lightBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(renderer.context.allocator, renderer.lighting.lightBuffers[i],
                                 renderer.lighting.lightAllocs[i]);
                renderer.lighting.lightBuffers[i] = VK_NULL_HANDLE;
            }
            if (renderer.lighting.clusterParamBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(renderer.context.allocator, renderer.lighting.clusterParamBuffers[i],
                                 renderer.lighting.clusterParamAllocs[i]);
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
        if (renderer.skinning.setLayout)
        {
            renderer.context.device.destroyDescriptorSetLayout(renderer.skinning.setLayout);
        }
        for (vk::DescriptorPool pool : renderer.skinning.pools)
        {
            if (pool)
            {
                renderer.context.device.destroyDescriptorPool(pool);
            }
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
        for (vk::DescriptorSetLayout l :
             { renderer.ddgi.voxelLayout, renderer.ddgi.traceLayout, renderer.ddgi.blendIrrLayout,
               renderer.ddgi.blendDistLayout, renderer.ddgi.borderLayout, renderer.ddgi.meshLayout })
        {
            if (l)
            {
                renderer.context.device.destroyDescriptorSetLayout(l);
            }
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
            if (l)
            {
                renderer.context.device.destroyDescriptorSetLayout(l);
            }
        }
        if (renderer.restir.sampler)
        {
            renderer.context.device.destroySampler(renderer.restir.sampler);
        }

        destroyProfilerPools(renderer);

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

    namespace
    {
        auto steadyNowNs() -> u64
        {
            return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
        }
    }

    // A CpuRecorder pointing at the current frame slot's span buffer, or an inactive one
    // (null buffer => every CpuScope is a no-op) when the profiler is Off or its pools are
    // not yet armed — the same gate the GPU timestamp path uses.
    auto cpuRecorder(Renderer& renderer) -> CpuRecorder
    {
        if (renderer.profiler.mode == ProfilerMode::Off || !renderer.profiler.poolsReady)
        {
            return {};
        }
        return CpuRecorder{ &renderer.cpuProfiler.registry, &renderer.cpuProfiler.buffers[renderer.frame.index] };
    }

    // The active GPU scope recorder for a pass body to open sub-scopes against, or null when
    // sub-scopes are not opted in. The recorder's pool is only armed (non-null) while a frame
    // is being recorded, so a stray call outside executeRenderGraph is a harmless no-op.
    auto gpuScopeRecorder(Renderer& renderer) -> RgTimestamps*
    {
        return renderer.profiler.subScopes ? &renderer.profiler.scopeRecorder : nullptr;
    }

    // The pipeline-statistics counters captured per pass, in ascending VkQueryPipelineStatisticFlagBits
    // bit order (the order vkGetQueryPoolResults returns them). The decode in readbackGpuTimings
    // reads them positionally, so this order and PipelineStatsCount must stay in lockstep.
    inline constexpr vk::QueryPipelineStatisticFlags PipelineStatsFlags =
        vk::QueryPipelineStatisticFlagBits::eInputAssemblyVertices |
        vk::QueryPipelineStatisticFlagBits::eVertexShaderInvocations |
        vk::QueryPipelineStatisticFlagBits::eClippingInvocations |
        vk::QueryPipelineStatisticFlagBits::eClippingPrimitives |
        vk::QueryPipelineStatisticFlagBits::eFragmentShaderInvocations |
        vk::QueryPipelineStatisticFlagBits::eComputeShaderInvocations;
    inline constexpr u32 PipelineStatsCount = 6;

    auto allocateProfilerPools(Renderer& renderer) -> bool
    {
        GpuProfiler& prof = renderer.profiler;
        if (prof.poolsReady)
        {
            return true;
        }
        for (vk::QueryPool& pool : prof.timestampPools)
        {
            vk::QueryPoolCreateInfo info{};
            info.queryType = vk::QueryType::eTimestamp;
            info.queryCount = 2 * MaxProfiledScopes;
            auto created = checked(renderer.context.device.createQueryPool(info), "createQueryPool(timestamp)");
            if (!created)
            {
                logError(created.error());
                return false;
            }
            pool = *created;
        }
        // Pipeline-statistics pools (one slot per top-level pass), only if the device feature is
        // present. Absent => statsPools stay null and PipelineStats mode falls back to timestamps.
        if (prof.pipelineStatsSupported)
        {
            for (vk::QueryPool& pool : prof.statsPools)
            {
                vk::QueryPoolCreateInfo info{};
                info.queryType = vk::QueryType::ePipelineStatistics;
                info.queryCount = MaxProfiledScopes;
                info.pipelineStatistics = PipelineStatsFlags;
                auto created = checked(renderer.context.device.createQueryPool(info), "createQueryPool(stats)");
                if (!created)
                {
                    logError(created.error());
                    return false;
                }
                pool = *created;
            }
        }
        prof.poolsReady = true;
        return true;
    }

    void destroyProfilerPools(Renderer& renderer)
    {
        GpuProfiler& prof = renderer.profiler;
        for (vk::QueryPool& pool : prof.timestampPools)
        {
            if (pool)
            {
                renderer.context.device.destroyQueryPool(pool);
                pool = nullptr;
            }
        }
        for (vk::QueryPool& pool : prof.statsPools)
        {
            if (pool)
            {
                renderer.context.device.destroyQueryPool(pool);
                pool = nullptr;
            }
        }
        for (std::vector<ScopeRecord>& records : prof.recordedScopes)
        {
            records.clear();
        }
        prof.poolsReady = false;
    }

    // Sample the device and host clocks together and store the offset that maps a GPU tick onto
    // the CPU steady_clock axis. Cheap (no queue work); called periodically to track drift. A
    // no-op when calibration is unavailable, leaving correlated = false (own-axis fallback).
    void calibrateTimestamps(Renderer& renderer)
    {
        GpuProfiler& prof = renderer.profiler;
        if (!prof.calibration.available || renderer.context.calibratedTs.getTimestamps == nullptr)
        {
            return;
        }
        std::array<VkCalibratedTimestampInfoEXT, 2> infos{};
        infos[0].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT;
        infos[0].timeDomain = VK_TIME_DOMAIN_DEVICE_EXT;
        infos[1].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT;
        infos[1].timeDomain = prof.calibration.hostDomain;
        std::array<u64, 2> samples{};
        u64 maxDeviation = 0;
        const VkResult r = renderer.context.calibratedTs.getTimestamps(renderer.context.device, 2, infos.data(),
                                                                       samples.data(), &maxDeviation);
        if (r != VK_SUCCESS)
        {
            return;
        }
        // samples[0] is device ticks (same units as the query pool); samples[1] is host ns
        // (CLOCK_MONOTONIC). offset = hostNs - deviceNs lets readback add a single term.
        const u64 deviceTick = samples[0] & prof.timestampMask;
        const double deviceNs = static_cast<double>(deviceTick) * prof.timestampPeriod;
        prof.calibration.deviceToHostNsOffset = static_cast<i64>(static_cast<double>(samples[1]) - deviceNs);
        prof.calibration.maxDeviationNs = maxDeviation;
        prof.calibration.correlated = true;
        prof.calibration.lastCalibratedSerial = renderer.frameSerial;
    }

    // Read back slot's timestamp pool (its GPU work completed at the beginFrame fence wait,
    // so this never blocks) into the profiler's last-timings + EMA GPU frame time. Each pass
    // contributes two queries; eWithAvailability gives a [value, available] pair per query.
    void readbackGpuTimings(Renderer& renderer, u32 slot)
    {
        GpuProfiler& prof = renderer.profiler;
        const std::vector<ScopeRecord>& records = prof.recordedScopes[slot];
        if (records.empty() || !prof.timestampPools[slot])
        {
            return;  // nothing recorded into this slot yet (first frames after enabling)
        }
        const u32 scopeCount = static_cast<u32>(records.size());
        const u32 queryCount = 2 * scopeCount;
        std::vector<u64> raw(static_cast<std::size_t>(queryCount) * 2, 0);
        const vk::Result r = renderer.context.device.getQueryPoolResults(
            prof.timestampPools[slot], 0, queryCount, raw.size() * sizeof(u64), raw.data(), 2 * sizeof(u64),
            vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
        if (r != vk::Result::eSuccess && r != vk::Result::eNotReady)
        {
            return;  // keep the last good read-back
        }

        // Pipeline statistics: read the stats pool only if this frame actually recorded stats (a
        // record carries a statsSlot). That implies the frame ran in PipelineStats mode, so the
        // pool was reset in beginFrame — reading an unreset pool is a validation error, so a
        // timestamps-only frame (where the pool exists but was never reset) must not read it.
        std::vector<u64> statsRaw;
        bool statsAvail = false;
        if (prof.statsPools[slot])
        {
            for (const ScopeRecord& rec : records)
            {
                if (rec.statsSlot >= 0)
                {
                    statsAvail = true;
                    break;
                }
            }
        }
        if (statsAvail)
        {
            statsRaw.assign(static_cast<std::size_t>(MaxProfiledScopes) * (PipelineStatsCount + 1), 0);
            static_cast<void>(renderer.context.device.getQueryPoolResults(
                prof.statsPools[slot], 0, MaxProfiledScopes, statsRaw.size() * sizeof(u64), statsRaw.data(),
                (PipelineStatsCount + 1) * sizeof(u64),
                vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability));
        }

        // The i-th record owns query slots 2i (begin) / 2i+1 (end). The frame span is the
        // earliest begin to the latest end across all scopes — NOT a sum, since sibling/async
        // scopes overlap and a parent brackets its children, so a nested last-record-end is wrong.
        u64 spanBegin = 0;
        u64 spanEnd = 0;
        bool spanValid = false;
        for (u32 i = 0; i < scopeCount; i = i + 1)
        {
            if (raw[4 * i + 1] != 0 && raw[4 * i + 3] != 0)
            {
                const u64 b = raw[4 * i] & prof.timestampMask;
                const u64 e = raw[4 * i + 2] & prof.timestampMask;
                if (!spanValid)
                {
                    spanBegin = b;
                    spanEnd = e;
                    spanValid = true;
                }
                spanBegin = std::min(spanBegin, b);
                spanEnd = std::max(spanEnd, e);
            }
        }

        std::vector<PassTiming> timings;
        timings.reserve(scopeCount);
        for (u32 i = 0; i < scopeCount; i = i + 1)
        {
            const ScopeRecord& rec = records[i];
            PassTiming t;
            t.name = rec.name;
            t.parentIndex = rec.parentIndex;
            t.depth = rec.depth;
            if (raw[4 * i + 1] != 0 && raw[4 * i + 3] != 0)
            {
                const u64 b = raw[4 * i] & prof.timestampMask;
                const u64 e = raw[4 * i + 2] & prof.timestampMask;
                const u64 ticks = e >= b ? e - b : 0;
                t.gpuMs = static_cast<f32>(static_cast<double>(ticks) * prof.timestampPeriod / 1.0e6);
                // Correlated: project onto the CPU steady_clock axis (absolute host ns) so the GPU
                // and CPU lanes share one zero. Otherwise stay frame-relative (own-axis fallback).
                if (prof.calibration.correlated)
                {
                    const double offset = static_cast<double>(prof.calibration.deviceToHostNsOffset);
                    t.startNs = static_cast<u64>((static_cast<double>(b) * prof.timestampPeriod) + offset);
                    t.endNs = static_cast<u64>((static_cast<double>(e) * prof.timestampPeriod) + offset);
                }
                else
                {
                    if (spanValid && b >= spanBegin)
                    {
                        t.startNs = static_cast<u64>(static_cast<double>(b - spanBegin) * prof.timestampPeriod);
                    }
                    if (spanValid && e >= spanBegin)
                    {
                        t.endNs = static_cast<u64>(static_cast<double>(e - spanBegin) * prof.timestampPeriod);
                    }
                }
            }
            if (statsAvail && rec.statsSlot >= 0)
            {
                const std::size_t base = static_cast<std::size_t>(rec.statsSlot) * (PipelineStatsCount + 1);
                if (statsRaw[base + PipelineStatsCount] != 0)  // availability word
                {
                    t.hasStats = true;
                    t.stats.inputVertices = statsRaw[base + 0];
                    t.stats.vertexInvocations = statsRaw[base + 1];
                    t.stats.clippingInvocations = statsRaw[base + 2];
                    t.stats.clippingPrimitives = statsRaw[base + 3];
                    t.stats.fragmentInvocations = statsRaw[base + 4];
                    t.stats.computeInvocations = statsRaw[base + 5];
                    t.stats.pixels = rec.pixels;
                }
            }
            timings.push_back(std::move(t));
        }
        prof.lastTimings = std::move(timings);
        prof.lastGpuTotalMs =
            spanValid && spanEnd >= spanBegin
                ? static_cast<f32>(static_cast<double>(spanEnd - spanBegin) * prof.timestampPeriod / 1.0e6)
                : 0.0f;
        renderer.gpuFrameMs =
            renderer.gpuFrameMs == 0.0f ? prof.lastGpuTotalMs : renderer.gpuFrameMs * 0.9f + prof.lastGpuTotalMs * 0.1f;
    }

    // Sum the VMA budget across device-local heaps (cheap, unlike vmaCalculateStatistics).
    void readVramBudget(Renderer& renderer)
    {
        const VkPhysicalDeviceMemoryProperties* memProps = nullptr;
        vmaGetMemoryProperties(renderer.context.allocator, &memProps);
        std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
        vmaGetHeapBudgets(renderer.context.allocator, budgets.data());
        u64 usage = 0;
        u64 budget = 0;
        for (u32 h = 0; h < memProps->memoryHeapCount; h = h + 1)
        {
            if ((memProps->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
            {
                usage = usage + budgets[h].usage;
                budget = budget + budgets[h].budget;
            }
        }
        renderer.stats.vramUsageBytes = usage;
        renderer.stats.vramBudgetBytes = budget;
    }

    void setProfilerMode(Renderer& renderer, ProfilerMode mode)
    {
        // Degrade a request the device cannot satisfy: no timestamps ⇒ off; no pipeline
        // statistics ⇒ fall back to plain timestamps.
        if (mode != ProfilerMode::Off && !renderer.profiler.timestampsSupported)
        {
            mode = ProfilerMode::Off;
        }
        if (mode == ProfilerMode::PipelineStats && !renderer.profiler.pipelineStatsSupported)
        {
            mode = ProfilerMode::Timestamps;
        }
        if (mode != ProfilerMode::Off && !renderer.profiler.poolsReady && !allocateProfilerPools(renderer))
        {
            mode = ProfilerMode::Off;
        }
        renderer.profiler.mode = mode;
        if (mode == ProfilerMode::Off)
        {
            renderer.profiler.lastTimings.clear();
            renderer.profiler.lastGpuTotalMs = 0.0f;
            renderer.gpuFrameMs = 0.0f;
            renderer.profiler.calibration.correlated = false;  // re-enable forces a fresh calibration
        }
    }

    auto profilerMode(const Renderer& renderer) -> ProfilerMode
    {
        return renderer.profiler.mode;
    }

    auto profilerTimestampsSupported(const Renderer& renderer) -> bool
    {
        return renderer.profiler.timestampsSupported;
    }

    auto profilerPipelineStatsSupported(const Renderer& renderer) -> bool
    {
        return renderer.profiler.pipelineStatsSupported;
    }

    auto softwareGpu(const Renderer& renderer) -> bool
    {
        return renderer.softwareGpu;
    }

    auto passTimings(const Renderer& renderer) -> std::vector<PassTiming>
    {
        return renderer.profiler.lastTimings;
    }

    auto passTimingsTotalMs(const Renderer& renderer) -> f32
    {
        return renderer.profiler.lastGpuTotalMs;
    }

    // Copy this slot's finalized frame into the capture: the CPU lane (buffers[slot], still
    // intact at the read-back seam) then the GPU lane (lastTimings, just read). parentIndex is
    // rebased into the merged span vector so each lane's tree stays intact across frames.
    void appendCaptureFrame(Renderer& renderer)
    {
        CaptureRecorder& cap = renderer.captureRecorder;
        const std::vector<std::string>& names = renderer.cpuProfiler.registry.names;
        const u32 base = static_cast<u32>(cap.capture.spans.size());
        u32 cpuCount = 0;
        if (cap.includeCpu)
        {
            for (const CpuSpan& s : renderer.cpuProfiler.buffers[renderer.frame.index].spans)
            {
                ProfileSpan ps;
                ps.name = names[s.marker];
                ps.lane = ProfileLane::Cpu;
                ps.startNs = s.startNs;
                ps.endNs = s.endNs;
                ps.depth = s.depth;
                ps.parentIndex = s.parent >= 0 ? static_cast<i32>(base) + s.parent : -1;
                cap.capture.spans.push_back(std::move(ps));
                cpuCount = cpuCount + 1;
            }
        }
        const u32 gpuBase = base + cpuCount;
        for (const PassTiming& t : renderer.profiler.lastTimings)
        {
            ProfileSpan ps;
            ps.name = t.name;
            ps.lane = ProfileLane::Gpu;
            ps.startNs = t.startNs;
            ps.endNs = t.endNs;
            ps.depth = t.depth;
            ps.parentIndex = t.parentIndex >= 0 ? static_cast<i32>(gpuBase) + t.parentIndex : -1;
            ps.hasStats = t.hasStats;
            ps.stats = t.stats;
            cap.capture.spans.push_back(std::move(ps));
        }
    }

    // Advance the capture state machine once per finalized frame (at the read-back seam). Arming
    // burns down the read-back delay so recorded frames reflect the arm-time settings.
    void tickCapture(Renderer& renderer)
    {
        CaptureRecorder& cap = renderer.captureRecorder;
        if (cap.state == CaptureState::Arming)
        {
            if (cap.warmup > 0)
            {
                cap.warmup = cap.warmup - 1;
            }
            if (cap.warmup == 0)
            {
                cap.state = CaptureState::Recording;
            }
            return;
        }
        if (cap.state == CaptureState::Recording)
        {
            appendCaptureFrame(renderer);
            cap.capturedFrames = cap.capturedFrames + 1;
            if (cap.capturedFrames >= cap.targetFrames)
            {
                cap.state = CaptureState::Ready;
            }
        }
    }

    auto startProfileCapture(Renderer& renderer, CaptureMode mode, u32 frames, std::string filter, bool includeCpu,
                             bool includeStats) -> u32
    {
        CaptureRecorder& cap = renderer.captureRecorder;
        cap.mode = mode;
        cap.targetFrames = mode == CaptureMode::Single ? 1u : std::clamp(frames, 1u, MaxCaptureFrames);
        cap.capturedFrames = 0;
        cap.warmup = MaxFramesInFlight + 1;  // flush the read-back delay before recording
        cap.filter = std::move(filter);
        cap.includeCpu = includeCpu;
        cap.includeStats = includeStats && renderer.profiler.pipelineStatsSupported;
        cap.capture = ProfileCapture{};
        cap.captureId = cap.nextCaptureId;
        cap.nextCaptureId = cap.nextCaptureId + 1;
        cap.priorMode = renderer.profiler.mode;
        cap.priorSubScopes = renderer.profiler.subScopes;
        // PipelineStats mode is the heaviest level (it adds the per-pass statistics queries on
        // top of timestamps); request it only when stats are wanted + supported, else timestamps.
        const ProfilerMode wanted = cap.includeStats ? ProfilerMode::PipelineStats : ProfilerMode::Timestamps;
        if (renderer.profiler.mode != wanted)
        {
            setProfilerMode(renderer, wanted);
        }
        renderer.profiler.subScopes = true;  // capture the full nested tree; restored on stop
        cap.state = CaptureState::Arming;
        return cap.captureId;
    }

    auto profileStatsSupported(const Renderer& renderer) -> bool
    {
        return renderer.profiler.pipelineStatsSupported;
    }

    auto profileCaptureState(const Renderer& renderer) -> CaptureState
    {
        return renderer.captureRecorder.state;
    }

    auto profileCaptureReady(const Renderer& renderer) -> bool
    {
        return renderer.captureRecorder.state == CaptureState::Ready;
    }

    auto profileCaptureMode(const Renderer& renderer) -> CaptureMode
    {
        return renderer.captureRecorder.mode;
    }

    auto profileCaptureCapturedFrames(const Renderer& renderer) -> u32
    {
        return renderer.captureRecorder.capturedFrames;
    }

    auto profileCaptureTargetFrames(const Renderer& renderer) -> u32
    {
        return renderer.captureRecorder.targetFrames;
    }

    auto stopProfileCapture(Renderer& renderer) -> ProfileCapture
    {
        CaptureRecorder& cap = renderer.captureRecorder;
        const bool wasActive = cap.state != CaptureState::Idle;
        ProfileCapture out = std::move(cap.capture);
        out.meta.frameCount = cap.capturedFrames;
        out.meta.filter = cap.filter;
        out.meta.softwareGpu = renderer.softwareGpu;
        out.meta.correlated = renderer.profiler.calibration.correlated;
        out.meta.deviceName = renderer.context.vkbDevice.physical_device.name;
        out.meta.timestampPeriod = renderer.profiler.timestampPeriod;
        out.meta.targetFps = renderer.perfConfig.targetFps;
        out.meta.mode = renderer.profiler.mode;
        if (wasActive)
        {
            renderer.profiler.subScopes = cap.priorSubScopes;
            if (cap.priorMode != renderer.profiler.mode)
            {
                setProfilerMode(renderer, cap.priorMode);
            }
        }
        cap.capture = ProfileCapture{};
        cap.capturedFrames = 0;
        cap.state = CaptureState::Idle;
        return out;
    }

    auto perfBudgetMs(const PerfConfig& config) -> f32
    {
        return config.targetFps > 0.0f ? 1000.0f / config.targetFps : 0.0f;
    }

    auto perfConfig(const Renderer& renderer) -> PerfConfig
    {
        return renderer.perfConfig;
    }

    void setPerfConfig(Renderer& renderer, const PerfConfig& config)
    {
        PerfConfig c = config;
        c.targetFps = std::clamp(c.targetFps, 1.0f, 10000.0f);
        c.greenBudgetFrac = std::clamp(c.greenBudgetFrac, 0.0f, 1.0f);
        c.greenMedianMul = std::max(1.0f, c.greenMedianMul);
        c.amberMedianMul = std::max(c.greenMedianMul, c.amberMedianMul);
        c.frozenMs = std::max(0.0f, c.frozenMs);
        c.vramWarnFrac = std::clamp(c.vramWarnFrac, 0.0f, 1.0f);
        c.vramCritFrac = std::clamp(c.vramCritFrac, c.vramWarnFrac, 1.0f);
        renderer.perfConfig = c;
    }

    // The ring's oldest→newest physical index for logical position i in [0, count).
    auto frameRingIndex(const Renderer& renderer, u32 i) -> u32
    {
        const u32 start =
            (renderer.frameRingHead + FrameHistoryCapacity - renderer.frameRingCount) % FrameHistoryCapacity;
        return (start + i) % FrameHistoryCapacity;
    }

    auto frameHistoryStats(const Renderer& renderer) -> FrameHistoryStats
    {
        FrameHistoryStats out;
        out.sampleCount = renderer.frameRingCount;
        out.stutterCount = renderer.stutterCount;
        if (renderer.frameRingCount == 0)
        {
            return out;
        }
        // Frame time = cpu busy + fence wait (the render-thread wall clock).
        std::vector<f32> times;
        times.reserve(renderer.frameRingCount);
        f32 sum = 0.0f;
        for (u32 i = 0; i < renderer.frameRingCount; i = i + 1)
        {
            const FrameSample& s = renderer.frameRing[frameRingIndex(renderer, i)];
            const f32 t = s.cpuMs + s.cpuWaitMs;
            times.push_back(t);
            sum = sum + t;
        }
        out.meanMs = sum / static_cast<f32>(times.size());
        f32 variance = 0.0f;
        for (const f32 t : times)
        {
            const f32 d = t - out.meanMs;
            variance = variance + d * d;
        }
        out.stddevMs = std::sqrt(variance / static_cast<f32>(times.size()));
        std::sort(times.begin(), times.end());
        const auto percentile = [&times](f32 p) -> f32
        {
            const std::size_t last = times.size() - 1;
            const std::size_t idx = static_cast<std::size_t>(p * static_cast<f32>(last) + 0.5f);
            return times[std::min(idx, last)];
        };
        out.p50Ms = percentile(0.50f);
        out.p95Ms = percentile(0.95f);
        out.p99Ms = percentile(0.99f);
        out.p999Ms = percentile(0.999f);
        out.maxMs = times.back();
        return out;
    }

    auto frameSamples(const Renderer& renderer, u32 maxSamples) -> std::vector<FrameSample>
    {
        const u32 take = std::min(maxSamples, renderer.frameRingCount);
        std::vector<FrameSample> out;
        out.reserve(take);
        for (u32 i = renderer.frameRingCount - take; i < renderer.frameRingCount; i = i + 1)
        {
            out.push_back(renderer.frameRing[frameRingIndex(renderer, i)]);
        }
        return out;
    }

    // FNV-1a over metric + "|" + pass — the alarm fingerprint that coalesces repeats.
    auto alarmFingerprint(std::string_view metric, std::string_view pass) -> u64
    {
        u64 hash = 1469598103934665603ULL;
        const auto mix = [&hash](std::string_view text)
        {
            for (const char c : text)
            {
                hash = hash ^ static_cast<u8>(c);
                hash = hash * 1099511628211ULL;
            }
        };
        mix(metric);
        mix("|");
        mix(pass);
        return hash;
    }

    auto pushAlarmEvent(AlarmState& alarms, AlarmEvent event) -> u64
    {
        const u64 seq = alarms.nextSeq;
        event.seq = seq;
        alarms.nextSeq = alarms.nextSeq + 1;
        alarms.events[alarms.eventHead] = std::move(event);
        alarms.eventHead = (alarms.eventHead + 1) % AlarmEventRingCapacity;
        if (alarms.eventCount < AlarmEventRingCapacity)
        {
            alarms.eventCount = alarms.eventCount + 1;
        }
        return seq;
    }

    // Raise (or refresh) an alarm. The first breach emits one FIRING; while active the
    // count/peak update in place and only a severity escalation emits another FIRING.
    void raiseAlarm(AlarmState& alarms, u64 nowNs, std::string metric, std::string pass, AlarmSeverity severity,
                    f32 value, f32 threshold)
    {
        const u64 fingerprint = alarmFingerprint(metric, pass);
        for (ActiveAlarm& active : alarms.active)
        {
            if (active.fingerprint == fingerprint)
            {
                active.lastSeenNs = nowNs;
                active.count = active.count + 1;
                active.value = value;
                active.threshold = threshold;
                active.peak = std::max(active.peak, value);
                if (static_cast<int>(severity) > static_cast<int>(active.severity))
                {
                    active.severity = severity;
                    AlarmEvent escalation;
                    escalation.fingerprint = fingerprint;
                    escalation.metric = metric;
                    escalation.pass = pass;
                    escalation.severity = severity;
                    escalation.kind = AlarmEventKind::Firing;
                    escalation.value = value;
                    escalation.threshold = threshold;
                    escalation.sinceFrame = active.sinceFrame;
                    escalation.count = active.count;
                    pushAlarmEvent(alarms, std::move(escalation));
                }
                return;
            }
        }
        ActiveAlarm fresh;
        fresh.fingerprint = fingerprint;
        fresh.metric = metric;
        fresh.pass = pass;
        fresh.severity = severity;
        fresh.value = value;
        fresh.threshold = threshold;
        fresh.peak = value;
        fresh.sinceFrame = alarms.frameCounter;
        fresh.sinceNs = nowNs;
        fresh.lastSeenNs = nowNs;
        fresh.count = 1;
        alarms.active.push_back(fresh);
        AlarmEvent firing;
        firing.fingerprint = fingerprint;
        firing.metric = std::move(metric);
        firing.pass = std::move(pass);
        firing.severity = severity;
        firing.kind = AlarmEventKind::Firing;
        firing.value = value;
        firing.threshold = threshold;
        firing.sinceFrame = fresh.sinceFrame;
        firing.count = 1;
        pushAlarmEvent(alarms, std::move(firing));
    }

    // Clear an active alarm if present, emitting one RESOLVED (with duration + peak).
    void clearAlarm(AlarmState& alarms, u64 nowNs, std::string_view metric, std::string_view pass)
    {
        const u64 fingerprint = alarmFingerprint(metric, pass);
        for (auto it = alarms.active.begin(); it != alarms.active.end(); ++it)
        {
            if (it->fingerprint == fingerprint)
            {
                AlarmEvent resolved;
                resolved.fingerprint = fingerprint;
                resolved.metric = it->metric;
                resolved.pass = it->pass;
                resolved.severity = it->severity;
                resolved.kind = AlarmEventKind::Resolved;
                resolved.value = it->peak;
                resolved.threshold = it->threshold;
                resolved.sinceFrame = it->sinceFrame;
                resolved.count = it->count;
                resolved.durationMs = static_cast<f32>(nowNs - it->sinceNs) / 1.0e6f;
                pushAlarmEvent(alarms, std::move(resolved));
                alarms.active.erase(it);
                return;
            }
        }
    }

    // The mean over a recent frame-time window — the fraction-over-budget SLI for burn-rate.
    auto frameWindowOverBudget(const Renderer& renderer, u32 window, f32 budget) -> f32
    {
        const u32 w = std::min(window, renderer.frameRingCount);
        if (w == 0)
        {
            return 0.0f;
        }
        u32 over = 0;
        for (u32 i = renderer.frameRingCount - w; i < renderer.frameRingCount; i = i + 1)
        {
            const FrameSample& s = renderer.frameRing[frameRingIndex(renderer, i)];
            if (s.cpuMs + s.cpuWaitMs > budget)
            {
                over = over + 1;
            }
        }
        return static_cast<f32>(over) / static_cast<f32>(w);
    }

    // One per-frame alarm tick: smooth, then gate. Runs detectors on the smoothed series
    // (never raw per-frame values) and against the shared PerfConfig thresholds.
    void tickAlarms(Renderer& renderer, f32 frameTimeMs, f32 dtSec, u64 nowNs)
    {
        AlarmState& alarms = renderer.alarms;
        alarms.frameCounter = alarms.frameCounter + 1;
        const f32 budget = perfBudgetMs(renderer.perfConfig);

        // Irregular-interval EMA (tau ≈ 300 ms): alpha = 1 − exp(−dt / tau).
        if (dtSec > 0.0f)
        {
            const f32 alpha = 1.0f - std::exp(-dtSec / 0.3f);
            alarms.emaFrameMs =
                alarms.emaFrameMs == 0.0f ? frameTimeMs : alarms.emaFrameMs + alpha * (frameTimeMs - alarms.emaFrameMs);
        }
        else if (alarms.emaFrameMs == 0.0f)
        {
            alarms.emaFrameMs = frameTimeMs;
        }

        // frame-budget: sustained over-budget. Hysteresis (enter 1.2× / exit 1.0× budget) +
        // a debounce so a one-frame breach never fires; escalates to critical at 2× budget.
        if (budget > 0.0f)
        {
            const f32 enterTh = 1.2f * budget;
            const f32 exitTh = 1.0f * budget;
            const f32 criticalTh = 2.0f * budget;  // ~< 30 FPS at a 60 Hz budget
            alarms.budgetWarnHeldSec = alarms.emaFrameMs > enterTh ? alarms.budgetWarnHeldSec + dtSec : 0.0f;
            alarms.budgetCritHeldSec = alarms.emaFrameMs > criticalTh ? alarms.budgetCritHeldSec + dtSec : 0.0f;
            const bool warnReady = alarms.budgetWarnHeldSec >= 0.3f;
            const bool critReady = alarms.budgetCritHeldSec >= 0.5f;
            if (warnReady)
            {
                raiseAlarm(alarms, nowNs, "frame-budget", "",
                           critReady ? AlarmSeverity::Critical : AlarmSeverity::Warning, alarms.emaFrameMs, enterTh);
            }
            else if (alarms.emaFrameMs < exitTh)
            {
                clearAlarm(alarms, nowNs, "frame-budget", "");
            }
        }

        // frame-hitch: a robust spike. Modified z-score over a recent window — median/MAD
        // beat mean/stddev because the outlier inflates stddev and masks itself.
        const u32 window = std::min<u32>(renderer.frameRingCount, 64);
        if (window >= 8)
        {
            std::vector<f32> times;
            times.reserve(window);
            for (u32 i = renderer.frameRingCount - window; i < renderer.frameRingCount; i = i + 1)
            {
                const FrameSample& s = renderer.frameRing[frameRingIndex(renderer, i)];
                times.push_back(s.cpuMs + s.cpuWaitMs);
            }
            std::vector<f32> sorted = times;
            std::sort(sorted.begin(), sorted.end());
            const f32 median = sorted[sorted.size() / 2];
            for (f32& v : sorted)
            {
                v = std::fabs(v - median);
            }
            std::sort(sorted.begin(), sorted.end());
            const f32 mad = std::max(sorted[sorted.size() / 2], 0.05f);  // floor guards MAD == 0
            const f32 modZ = 0.6745f * (frameTimeMs - median) / mad;
            // A spike is only a hitch if it also blew the frame budget in absolute terms —
            // a statistical outlier at 2 ms (≈ 500 FPS) is not worth flagging. Modest
            // single-frame budget misses are info (log only); a hard spike (> 2× budget,
            // ≈ < 30 FPS for one frame) is a warning that toasts.
            if (modZ > 3.5f && budget > 0.0f && frameTimeMs > budget)
            {
                alarms.hitchClearFrames = 0;
                const AlarmSeverity severity =
                    frameTimeMs > 2.0f * budget ? AlarmSeverity::Warning : AlarmSeverity::Info;
                raiseAlarm(alarms, nowNs, "frame-hitch", "", severity, frameTimeMs, median + mad * 3.5f / 0.6745f);
            }
            else
            {
                alarms.hitchClearFrames = alarms.hitchClearFrames + 1;
                if (alarms.hitchClearFrames >= 10)
                {
                    clearAlarm(alarms, nowNs, "frame-hitch", "");
                }
            }
        }

        // burn-rate: the sustained-user-pain SLI. A short and a long window must both
        // breach (fast detect, low false-positive, clears quickly when the problem stops).
        if (budget > 0.0f && renderer.frameRingCount >= 60)
        {
            const f32 sliShort = frameWindowOverBudget(renderer, 60, budget);  // ~1 s @ 60 Hz
            const f32 sliLong = frameWindowOverBudget(renderer, 600, budget);  // ~10 s
            if (sliShort > 0.5f && sliLong > 0.5f)
            {
                raiseAlarm(alarms, nowNs, "burn-rate", "", AlarmSeverity::Critical, sliShort * 100.0f, 50.0f);
            }
            else if (sliShort > 0.1f && sliLong > 0.1f)
            {
                raiseAlarm(alarms, nowNs, "burn-rate", "", AlarmSeverity::Warning, sliShort * 100.0f, 10.0f);
            }
            else if (sliShort < 0.05f)
            {
                clearAlarm(alarms, nowNs, "burn-rate", "");
            }
        }

        // vram: usage fraction of the device-local budget (only known when profiling).
        if (renderer.stats.vramBudgetBytes > 0)
        {
            const f32 frac =
                static_cast<f32>(renderer.stats.vramUsageBytes) / static_cast<f32>(renderer.stats.vramBudgetBytes);
            if (frac >= renderer.perfConfig.vramCritFrac)
            {
                raiseAlarm(alarms, nowNs, "vram", "", AlarmSeverity::Critical, frac * 100.0f,
                           renderer.perfConfig.vramCritFrac * 100.0f);
            }
            else if (frac >= renderer.perfConfig.vramWarnFrac)
            {
                raiseAlarm(alarms, nowNs, "vram", "", AlarmSeverity::Warning, frac * 100.0f,
                           renderer.perfConfig.vramWarnFrac * 100.0f);
            }
            else if (frac < renderer.perfConfig.vramWarnFrac * 0.95f)
            {
                clearAlarm(alarms, nowNs, "vram", "");
            }
        }

        // pso-compile: a PSO built mid-frame is a hitch on a steady-state frame (info only).
        if (renderer.stats.pipelinesCreated > 0)
        {
            raiseAlarm(alarms, nowNs, "pso-compile", "", AlarmSeverity::Info,
                       static_cast<f32>(renderer.stats.pipelinesCreated), 0.0f);
        }
        else
        {
            clearAlarm(alarms, nowNs, "pso-compile", "");
        }
    }

    auto drainAlarms(const Renderer& renderer, u64 since) -> AlarmDrain
    {
        const AlarmState& alarms = renderer.alarms;
        AlarmDrain out;
        out.highWaterSeq = alarms.nextSeq - 1;
        u64 oldest = 0;
        if (alarms.eventCount > 0)
        {
            const u32 oldestIdx =
                (alarms.eventHead + AlarmEventRingCapacity - alarms.eventCount) % AlarmEventRingCapacity;
            oldest = alarms.events[oldestIdx].seq;
        }
        out.oldestSeq = oldest;
        // Events (since+1 .. oldest-1) fell off the ring: the client must resync from the active set.
        out.overflowed = oldest > since + 1;
        for (u32 i = 0; i < alarms.eventCount; i = i + 1)
        {
            const u32 idx =
                (alarms.eventHead + AlarmEventRingCapacity - alarms.eventCount + i) % AlarmEventRingCapacity;
            if (alarms.events[idx].seq > since)
            {
                out.events.push_back(alarms.events[idx]);
            }
        }
        return out;
    }

    auto activeAlarms(const Renderer& renderer) -> std::vector<ActiveAlarm>
    {
        return renderer.alarms.active;
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

        // Open the CPU-time window for this frame. Blocking waits are subtracted out so
        // cpuFrameMs is render-thread busy time and cpuWaitMs is the GPU-bound signal.
        renderer.frameCpuStartNs = steadyNowNs();
        renderer.frameWaitNs = 0;

        const u64 fenceWaitStart = steadyNowNs();
        static_cast<void>(renderer.context.device.waitForFences(frame.inFlight, VK_TRUE, UINT64_MAX));
        renderer.frameWaitNs = renderer.frameWaitNs + (steadyNowNs() - fenceWaitStart);

        // This slot's GPU work (from MaxFramesInFlight frames ago) is now complete, so its
        // timestamp pool reads back without blocking. Then clear the slot's scope records so
        // executeRenderGraph can repopulate them for the frame about to be recorded.
        if (renderer.profiler.mode != ProfilerMode::Off && renderer.profiler.poolsReady)
        {
            // Re-sample the CPU/GPU clock offset on enable and roughly once a second after, so the
            // merged timeline tracks clock drift over a capture.
            if (!renderer.profiler.calibration.correlated ||
                renderer.frameSerial - renderer.profiler.calibration.lastCalibratedSerial >= 64)
            {
                calibrateTimestamps(renderer);
            }
            readbackGpuTimings(renderer, renderer.frame.index);
            // Drain this slot's merged spans into an in-flight capture before they are cleared.
            tickCapture(renderer);
            renderer.profiler.recordedScopes[renderer.frame.index].clear();
            renderer.cpuProfiler.buffers[renderer.frame.index].reset();
        }

        if (renderer.shmPublish.enabled)
        {
            // The fence wait above guarantees this slot's recorded readback (from
            // MaxFramesInFlight frames ago) has completed: hand it to the editor. The
            // swapchain is never acquired or presented in this mode.
            ShmPublishSlot& slot = renderer.shmPublish.slots[renderer.frame.index];
            if (slot.valid)
            {
                publishShmPublishSlot(renderer, slot);
            }
        }
        else
        {
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
                const u64 imageWaitStart = steadyNowNs();
                static_cast<void>(renderer.context.device.waitForFences(
                    renderer.swapchain.imagesInFlight[renderer.frame.imageIndex], VK_TRUE, UINT64_MAX));
                renderer.frameWaitNs = renderer.frameWaitNs + (steadyNowNs() - imageWaitStart);
            }
            renderer.swapchain.imagesInFlight[renderer.frame.imageIndex] = frame.inFlight;
        }

        // Apply a pending Viewport resize (requested last frame). Single shared
        // target, so a full device idle is required before recreating it.
        if (renderer.targets.desiredWidth > 0 && renderer.targets.desiredHeight > 0 &&
            (renderer.targets.desiredWidth != renderer.targets.offscreen.extent.width ||
             renderer.targets.desiredHeight != renderer.targets.offscreen.extent.height))
        {
            static_cast<void>(renderer.context.device.waitIdle());
            auto resized = newColorImage(renderer, renderer.targets.desiredWidth, renderer.targets.desiredHeight,
                                         OffscreenColorFormat, true);
            if (resized)
            {
                renderer.targets.offscreen = std::move(*resized);
                renderer.targets.generation = renderer.targets.generation + 1;
                updateTonemapSet(renderer);  // the storage-image binding follows the new view
                auto resizedDepth =
                    newDepthImage(renderer, renderer.targets.desiredWidth, renderer.targets.desiredHeight);
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
                if (renderer.context.rtSupported)
                {
                    recreateRestirTargets(renderer);
                }  // ReSTIR reservoirs
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
        renderer.overlay.vertices.clear();    // editor overlay re-submits its geometry each frame
        renderer.stats.descriptorBinds = 0;   // re-accumulated by recordSceneDrawList this frame
        renderer.stats.pipelinesCreated = 0;  // PSO compiles counted across this frame

        vk::CommandBufferBeginInfo beginInfo{};
        beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(frame.commandBuffer.begin(beginInfo));

        // Timestamp queries are uninitialized until reset; reset this slot's whole pool
        // before executeRenderGraph writes into it (reading an unreset pool risks device loss).
        if (renderer.profiler.mode != ProfilerMode::Off && renderer.profiler.poolsReady)
        {
            frame.commandBuffer.resetQueryPool(renderer.profiler.timestampPools[renderer.frame.index], 0,
                                               2 * MaxProfiledScopes);
            if (renderer.profiler.mode == ProfilerMode::PipelineStats &&
                renderer.profiler.statsPools[renderer.frame.index])
            {
                frame.commandBuffer.resetQueryPool(renderer.profiler.statsPools[renderer.frame.index], 0,
                                                   MaxProfiledScopes);
            }
        }

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

        // Capture any dirty reflection probes at this same GPU-idle point (on demand, never per
        // frame). Each capture renders the scene 6x + convolves; gated on the cull PSO being
        // ready (so the cached mesh draw list / pipelines exist).
        if (renderer.reflection.capturePending && renderer.pipelines.cull)
        {
            for (u32 slot = 0; slot < renderer.reflection.count; slot = slot + 1)
            {
                ReflectionProbe& probe = renderer.reflection.probes[slot];
                if (!probe.dirty)
                {
                    continue;
                }
                if (Result<void> r = captureReflectionProbe(renderer, probe, slot); r)
                {
                    logInfo(std::format(
                        "reflection probe captured — slot {}, origin ({:.1f}, {:.1f}, {:.1f}), radius {:.1f}", slot,
                        probe.origin.x, probe.origin.y, probe.origin.z, probe.influenceRadius));
                }
                else
                {
                    logError(r.error());
                }
                probe.dirty = false;
            }
            renderer.reflection.capturePending = false;
        }

        // CPU span for building this frame's render graph (cull + scene/lighting/post pass
        // declarations). Excludes the rare IBL/probe bakes above (editor-time, not per-frame).
        const CpuRecorder buildRec = cpuRecorder(renderer);
        const CpuScope buildScope(&buildRec, "build-frame-graph");

        Image& offscreen = renderer.targets.offscreen;
        Image& depth = renderer.targets.depth;
        const u32 f = renderer.frame.index;
        const bool doCull = renderer.lighting.clusterDispatchPending && renderer.pipelines.cull;
        renderer.lighting.clusterDispatchPending = false;

        // The frame as a render graph: declare each pass's resource usage and let the
        // graph derive the barriers + layout transitions. The offscreen color carries
        // its layout across frames (sampled by the present blit last frame → WAR into this scene).
        renderer.graph.current = newRenderGraph();
        RenderGraph& graph = renderer.graph.current;
        // frameSceneColor is always the offscreen (what the present blit samples + tonemap reads). The
        // scene's 1x result lands in `sceneOutput`: the offscreen normally, or the FXAA
        // scratch when FXAA is on (FXAA then edge-blurs scratch → offscreen). With MSAA the
        // scene renders to msaaColor and resolves into sceneOutput. mutually exclusive via set-aa.
        const bool msaa =
            renderer.targets.sampleCount != vk::SampleCountFlagBits::e1 && renderer.targets.msaaColor.image;
        const bool fxaa = renderer.targets.fxaaEnabled && renderer.targets.scratch.image && renderer.pipelines.fxaa;
        const bool taa = renderer.targets.taaEnabled && renderer.targets.scratch.image && renderer.pipelines.motion &&
                         renderer.pipelines.taa;
        renderer.graph.sceneColor = importImage(graph, offscreen.image, offscreen.view, vk::ImageAspectFlagBits::eColor,
                                                offscreen.layout, &offscreen.layout);
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
        // The 1x depth is always imported: it is the scene depth (no MSAA) or the resolve
        // target the scene pass writes the multisampled depth into (MSAA). The post-tonemap
        // editor overlay reads it to occlude camera frustums against scene geometry.
        RgResource depth1x = importImage(graph, depth.image, depth.view, vk::ImageAspectFlagBits::eDepth,
                                         vk::ImageLayout::eUndefined, nullptr);
        RgResource sceneDepth = depth1x;
        if (msaa)
        {
            sceneDepth = importImage(graph, renderer.targets.msaaDepth.image, renderer.targets.msaaDepth.view,
                                     vk::ImageAspectFlagBits::eDepth, vk::ImageLayout::eUndefined, nullptr);
        }
        renderer.graph.swapImage = importImage(graph, renderer.swapchain.images[renderer.frame.imageIndex],
                                               renderer.swapchain.imageViews[renderer.frame.imageIndex],
                                               vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined, nullptr);

        // Compute skinning pre-pass: deform each skinned mesh-instance once into the frame's
        // deformed-vertex buffer. It runs before EVERY geometry pass (shadows, depth pre-pass,
        // G-buffer, scene), which all read it as a static vertex stream — the deform-once win.
        // The graph derives the compute-write→vertex-input barrier from the StorageWriteCompute
        // here + each consumer's VertexInputRead. The static/skin/palette reads need no barrier:
        // the meshes were uploaded long ago, and the host-written palette is covered by the
        // submit's implicit host-write dependency.
        RgResource deformedBuffer{};
        RgResource prevDeformedBuffer{};
        const bool doSkin = !renderer.frame.sceneDrawList.skinDispatches.empty() && renderer.pipelines.skin &&
                            renderer.skinning.deformedBuffers[f] && renderer.skinning.prevDeformedBuffers[f];
        if (doSkin)
        {
            deformedBuffer = importBuffer(graph, renderer.skinning.deformedBuffers[f]->buffer);
            prevDeformedBuffer = importBuffer(graph, renderer.skinning.prevDeformedBuffers[f]->buffer);
            RgPass skinPass;
            skinPass.name = "skin";
            skinPass.kind = RgPassKind::Compute;
            // Both deformed buffers are written this pass (current pose + previous pose), so the
            // graph emits the compute-write barrier for each before the consumers read them.
            skinPass.accesses = { RgAccess{ deformedBuffer, RgUsage::StorageWriteCompute },
                                  RgAccess{ prevDeformedBuffer, RgUsage::StorageWriteCompute } };
            skinPass.execute = [&renderer](vk::CommandBuffer cmd)
            {
                const SceneDrawList& drawList = renderer.frame.sceneDrawList;
                if (drawList.skinDispatches.empty())
                {
                    return;
                }
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.skin->pipeline);
                auto run = [&](const SkinDispatch& d)
                {
                    if (!d.set)
                    {
                        return;
                    }
                    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.skin->layout, 0, d.set,
                                           {});
                    struct SkinPush
                    {
                        u32 vertexCount;
                        u32 jointOffset;
                        u32 deformedOffset;
                        u32 pad;
                    } push{ d.vertexCount, d.jointOffset, d.deformedOffset, 0 };
                    cmd.pushConstants(renderer.pipelines.skin->layout, vk::ShaderStageFlagBits::eCompute, 0,
                                      sizeof(push), &push);
                    cmd.dispatch((d.vertexCount + 63) / 64, 1, 1);
                };
                // Current pose into the deformed buffer, previous pose into the prev-deformed
                // buffer; the kernel is identical, only the bound palette + output differ.
                for (const SkinDispatch& d : drawList.skinDispatches)
                {
                    run(d);
                }
                for (const SkinDispatch& d : drawList.prevSkinDispatches)
                {
                    run(d);
                }
            };
            addPass(graph, std::move(skinPass));
        }

        // Directional shadow: a depth-only pass renders the scene from the light's view
        // into the shadow map; the scene pass then samples it. The graph derives the
        // DepthWrite -> ShaderReadOnly transition (and the cross-frame WAR) from the usages.
        const bool doShadow =
            renderer.lighting.shadowPending && renderer.pipelines.shadowDepth && renderer.targets.shadowMap.image;
        RgResource shadowRes{};
        if (doShadow)
        {
            Image& shadowMap = renderer.targets.shadowMap;
            shadowRes = importImage(graph, shadowMap.image, shadowMap.view, vk::ImageAspectFlagBits::eDepth,
                                    shadowMap.layout, &shadowMap.layout);
            RgPass shadowPass;
            shadowPass.name = "shadow";
            shadowPass.kind = RgPassKind::Graphics;
            shadowPass.depth = RgAttachment{ shadowRes, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
                                             vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } } };
            shadowPass.renderArea = shadowMap.extent;
            const glm::mat4 lightViewProj = renderer.lighting.shadowViewProj;
            shadowPass.execute = [&renderer, lightViewProj](vk::CommandBuffer cmd)
            { recordShadowDepth(renderer, cmd, lightViewProj); };
            if (doSkin)
            {
                shadowPass.accesses.push_back(RgAccess{ deformedBuffer, RgUsage::VertexInputRead });
            }
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
            spotShadowRes = importImage(graph, spotMap.image, spotMap.view, vk::ImageAspectFlagBits::eDepth,
                                        spotMap.layout, &spotMap.layout);
            RgPass spotPass;
            spotPass.name = "spot-shadow";
            spotPass.kind = RgPassKind::Graphics;
            spotPass.depth = RgAttachment{ spotShadowRes, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
                                           vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } } };
            spotPass.renderArea = spotMap.extent;
            const glm::mat4 spotViewProj = renderer.lighting.spotShadowViewProj;
            spotPass.execute = [&renderer, spotViewProj](vk::CommandBuffer cmd)
            { recordShadowDepth(renderer, cmd, spotViewProj); };
            if (doSkin)
            {
                spotPass.accesses.push_back(RgAccess{ deformedBuffer, RgUsage::VertexInputRead });
            }
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
            { recordPointShadow(renderer, cmd, lightPos, farPlane); };
            if (doSkin)
            {
                pointPass.accesses.push_back(RgAccess{ deformedBuffer, RgUsage::VertexInputRead });
            }
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
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.cull->layout, 0,
                                       renderer.lighting.clusterSets[f], {});
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
            depthPass.depth = RgAttachment{ sceneDepth, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
                                            vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } } };
            depthPass.renderArea = offscreen.extent;
            depthPass.execute = [&renderer](vk::CommandBuffer cmd) { recordDepthPrepass(renderer, cmd); };
            if (doSkin)
            {
                depthPass.accesses.push_back(RgAccess{ deformedBuffer, RgUsage::VertexInputRead });
            }
            addPass(graph, std::move(depthPass));
        }

        // Screen-space effects off a thin G-buffer (view normal + view-Z): GTAO + bilateral
        // denoise, directional contact shadows, one-bounce SSGI. The G-buffer prepass runs
        // when ANY of them is on; each effect's compute pass is gated by its toggle. The
        // scene pass samples whichever maps are produced (flags in the light UBO gate the
        // shader reads). The graph derives all the ColorWrite/Storage/SampledRead barriers.
        const bool gbufReady = renderer.ssao.ready && renderer.pipelines.gbuffer && renderer.targets.gNormal.image;
        const bool doSsao = gbufReady && renderer.ssao.useSsao && renderer.pipelines.gtao && renderer.pipelines.aoBlur;
        const bool doContact = gbufReady && renderer.ssao.useContact && renderer.pipelines.contact;
        const bool doSsgi =
            gbufReady && renderer.ssao.useSsgi && renderer.pipelines.ssgi && renderer.pipelines.copyColor;
        // ReSTIR also needs the G-buffer (world pos/normal). Force the prepass when ReSTIR is
        // on even with no screen effect; the gNormal handle is captured for its sets.
        const bool wantRestir =
            renderer.restir.useRestir && renderer.restir.ready && renderer.context.rtSupported && gbufReady;
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
            gpass.colors.push_back(
                RgAttachment{ gNormalRes, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
                              vk::ClearValue{ vk::ClearColorValue{ std::array<f32, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } } } });
            gpass.depth = RgAttachment{ gDepthRes, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
                                        vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } } };
            gpass.renderArea = offscreen.extent;
            gpass.execute = [&renderer](vk::CommandBuffer cmd) { recordGbuffer(renderer, cmd); };
            if (doSkin)
            {
                gpass.accesses.push_back(RgAccess{ deformedBuffer, RgUsage::VertexInputRead });
            }
            addPass(graph, std::move(gpass));

            const glm::mat4 proj = renderer.ssao.projection;
            const glm::mat4 invProj = renderer.ssao.invProjection;

            if (doSsao)
            {
                RgResource aoRawRes =
                    importImage(graph, renderer.targets.aoRaw.image, renderer.targets.aoRaw.view,
                                vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined, nullptr);
                RgResource aoRes = importImage(graph, renderer.targets.aoMap.image, renderer.targets.aoMap.view,
                                               vk::ImageAspectFlagBits::eColor, renderer.targets.aoMap.layout,
                                               &renderer.targets.aoMap.layout);
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
                    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.gtao->layout, 0,
                                           renderer.ssao.gtaoSet, {});
                    struct
                    {
                        glm::mat4 invProjection;
                        glm::vec4 params;
                    } push{ invProj, glm::vec4(radius, strength, 0.0f, 0.0f) };
                    cmd.pushConstants(renderer.pipelines.gtao->layout, vk::ShaderStageFlagBits::eCompute, 0,
                                      sizeof(push), &push);
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
                    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.aoBlur->layout, 0,
                                           renderer.ssao.aoBlurSet, {});
                    cmd.dispatch(ssGroups(ssExtent.width), ssGroups(ssExtent.height), 1);
                };
                addPass(graph, std::move(blurpass));
                renderer.graph.aoResource = aoRes;
                renderer.graph.hasAo = true;
            }

            if (doContact)
            {
                RgResource contactRes =
                    importImage(graph, renderer.targets.contactMap.image, renderer.targets.contactMap.view,
                                vk::ImageAspectFlagBits::eColor, renderer.targets.contactMap.layout,
                                &renderer.targets.contactMap.layout);
                RgPass cpass;
                cpass.name = "contact-shadows";
                cpass.kind = RgPassKind::Compute;
                cpass.accesses = { RgAccess{ gNormalRes, RgUsage::SampledReadCompute },
                                   RgAccess{ contactRes, RgUsage::StorageImageRWCompute } };
                const glm::vec3 sunView = renderer.ssao.sunDirView;
                cpass.execute = [&renderer, ssExtent, ssGroups, proj, invProj, sunView](vk::CommandBuffer cmd)
                {
                    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.contact->pipeline);
                    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.contact->layout, 0,
                                           renderer.ssao.contactSet, {});
                    struct
                    {
                        glm::mat4 projection;
                        glm::mat4 invProjection;
                        glm::vec4 lightDirView;
                        glm::vec4 params;
                    } push{ proj, invProj, glm::vec4(sunView, 0.0f), glm::vec4(0.2f, 12.0f, 0.1f, 0.0f) };
                    cmd.pushConstants(renderer.pipelines.contact->layout, vk::ShaderStageFlagBits::eCompute, 0,
                                      sizeof(push), &push);
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
                RgResource prevColorRes =
                    importImage(graph, renderer.targets.prevColor.image, renderer.targets.prevColor.view,
                                vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eShaderReadOnlyOptimal, nullptr);
                renderer.graph.prevColorResource = prevColorRes;
                RgResource ssgiRes = importImage(graph, renderer.targets.ssgiMap.image, renderer.targets.ssgiMap.view,
                                                 vk::ImageAspectFlagBits::eColor, renderer.targets.ssgiMap.layout,
                                                 &renderer.targets.ssgiMap.layout);
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
                    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.ssgi->layout, 0,
                                           renderer.ssao.ssgiSet, {});
                    struct
                    {
                        glm::mat4 projection;
                        glm::mat4 invProjection;
                        glm::vec4 params;
                    } push{ proj, invProj, glm::vec4(radius * 2.0f, 1.0f, 8.0f, 0.0f) };
                    cmd.pushConstants(renderer.pipelines.ssgi->layout, vk::ShaderStageFlagBits::eCompute, 0,
                                      sizeof(push), &push);
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
            RgResource motionDepthRes =
                importImage(graph, renderer.targets.motionDepth.image, renderer.targets.motionDepth.view,
                            vk::ImageAspectFlagBits::eDepth, vk::ImageLayout::eUndefined, nullptr);
            RgPass motionPass;
            motionPass.name = "motion";
            motionPass.kind = RgPassKind::Graphics;
            motionPass.colors.push_back(
                RgAttachment{ motionRes, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
                              vk::ClearValue{ vk::ClearColorValue{ std::array<f32, 4>{ 0.0f, 0.0f, 0.0f, 0.0f } } } });
            motionPass.depth =
                RgAttachment{ motionDepthRes, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
                              vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } } };
            motionPass.renderArea = offscreen.extent;
            // The motion pass reads both deformed buffers as vertex input (cur + prev position),
            // so it must wait on the skin compute writes — declare the reads to derive the barrier.
            if (doSkin)
            {
                motionPass.accesses.push_back(RgAccess{ deformedBuffer, RgUsage::VertexInputRead });
                motionPass.accesses.push_back(RgAccess{ prevDeformedBuffer, RgUsage::VertexInputRead });
            }
            motionPass.execute = [&renderer](vk::CommandBuffer cmd) { recordMotion(renderer, cmd); };
            addPass(graph, std::move(motionPass));
        }

        // DDGI: voxelize the scene proxy -> trace probe rays (software) -> blend irradiance +
        // distance atlases -> octahedral border copy. All compute, before the scene pass that
        // samples the atlases. The voxel image lives in GENERAL (storage read+write); the
        // atlases ping General (blend/border write) <-> ShaderReadOnly (trace prev-read + mesh).
        const bool doDdgi = renderer.ddgi.useDdgi && renderer.ddgi.ready && renderer.pipelines.ddgiVoxelize &&
                            renderer.pipelines.ddgiTrace && renderer.pipelines.ddgiBlendIrr &&
                            renderer.pipelines.ddgiBlendDist && renderer.pipelines.ddgiBorder;
        if (doDdgi)
        {
            Ddgi& d = renderer.ddgi;
            const glm::uvec4 probeCount{ DdgiProbesX, DdgiProbesY, DdgiProbesZ, DdgiRaysPerProbe };
            const u32 probeTotal = DdgiProbesX * DdgiProbesY * DdgiProbesZ;
            RgResource voxelRes =
                importImage3D(graph, d.voxels.image, d.voxels.view, d.voxels.layout, &d.voxels.layout);
            RgResource rayRes = importImage(graph, d.rays.image, d.rays.view, vk::ImageAspectFlagBits::eColor,
                                            d.rays.layout, &d.rays.layout);
            RgResource irrRes = importImage(graph, d.irradiance.image, d.irradiance.view,
                                            vk::ImageAspectFlagBits::eColor, d.irradiance.layout, &d.irradiance.layout);
            RgResource distRes = importImage(graph, d.distance.image, d.distance.view, vk::ImageAspectFlagBits::eColor,
                                             d.distance.layout, &d.distance.layout);
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
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.ddgiVoxelize->layout, 0,
                                       renderer.ddgi.voxelSet, {});
                struct
                {
                    glm::uvec4 gridCount;
                    glm::vec4 volumeMin;
                    glm::vec4 volumeExtent;
                } push{ glm::uvec4(DdgiVoxelRes, DdgiVoxelRes, DdgiVoxelRes, boxCount), glm::vec4(volMin, 0.0f),
                        glm::vec4(volExt, 0.0f) };
                cmd.pushConstants(renderer.pipelines.ddgiVoxelize->layout, vk::ShaderStageFlagBits::eCompute, 0,
                                  sizeof(push), &push);
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
            trace.execute = [&renderer, probeCount, probeTotal, volMin, volExt, sunDir, sunColor, sunI, sky,
                             frameIdx](vk::CommandBuffer cmd)
            {
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.pipelines.ddgiTrace->pipeline);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.ddgiTrace->layout, 0,
                                       renderer.ddgi.traceSet, {});
                struct
                {
                    glm::uvec4 probeCount;
                    glm::uvec4 gridCount;
                    glm::vec4 volumeMin;
                    glm::vec4 volumeExtent;
                    glm::vec4 sunDir;
                    glm::vec4 sunColor;
                    glm::vec4 skyColor;
                } push{ probeCount,
                        glm::uvec4(DdgiVoxelRes, DdgiVoxelRes, DdgiVoxelRes, DdgiIrrInterior),
                        glm::vec4(volMin, 0.0f),
                        glm::vec4(volExt, 0.0f),
                        glm::vec4(sunDir, sunI),
                        glm::vec4(sunColor, frameIdx),
                        glm::vec4(sky, 0.0f) };
                cmd.pushConstants(renderer.pipelines.ddgiTrace->layout, vk::ShaderStageFlagBits::eCompute, 0,
                                  sizeof(push), &push);
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
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.ddgiBlendIrr->layout, 0,
                                       renderer.ddgi.blendIrrSet, {});
                struct
                {
                    glm::uvec4 probeCount;
                    glm::uvec4 tile;
                    glm::vec4 params;
                } push{ probeCount, glm::uvec4(DdgiIrrInterior, firstFrame, 0, 0), glm::vec4(DdgiHysteresis, 0, 0, 0) };
                cmd.pushConstants(renderer.pipelines.ddgiBlendIrr->layout, vk::ShaderStageFlagBits::eCompute, 0,
                                  sizeof(push), &push);
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
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.ddgiBlendDist->layout, 0,
                                       renderer.ddgi.blendDistSet, {});
                struct
                {
                    glm::uvec4 probeCount;
                    glm::uvec4 tile;
                    glm::vec4 params;
                } push{ probeCount, glm::uvec4(DdgiDistInterior, firstFrame, 0, 0),
                        glm::vec4(DdgiHysteresis, maxDist, 0, 0) };
                cmd.pushConstants(renderer.pipelines.ddgiBlendDist->layout, vk::ShaderStageFlagBits::eCompute, 0,
                                  sizeof(push), &push);
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
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.ddgiBorder->layout, 0,
                                       renderer.ddgi.borderSet, {});
                struct
                {
                    glm::uvec4 probeCount;
                    glm::uvec4 tile;
                } push{ probeCount, glm::uvec4(DdgiIrrInterior, 0, 0, 0) };
                cmd.pushConstants(renderer.pipelines.ddgiBorder->layout, vk::ShaderStageFlagBits::eCompute, 0,
                                  sizeof(push), &push);
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
        // fragment then traces inline ray-query shadows when rtShadows is on. Skinned instances
        // refit a per-instance BLAS from the deformed buffer first (in buildTlas), so the pass
        // declares an AccelStructBuildRead on it: the graph derives the skin-compute-write ->
        // AS-build-read barrier, and the pass is ordered after the skin pass above.
        renderer.rt.tlasReady = false;
        const bool hasSkinnedRt = !renderer.frame.sceneDrawList.skinnedRtInstances.empty();
        const bool hasRtInstances = !renderer.rt.frameModels.empty() || hasSkinnedRt;
        if (renderer.rt.buildPending && hasRtInstances && renderer.pipelines.cull /*any frame work*/)
        {
            RgPass tlasPass;
            tlasPass.name = "tlas-build";
            tlasPass.kind = RgPassKind::Compute;
            if (hasSkinnedRt && doSkin)
            {
                tlasPass.accesses = { RgAccess{ deformedBuffer, RgUsage::AccelStructBuildRead } };
            }
            tlasPass.execute = [&renderer](vk::CommandBuffer cmd)
            { buildTlas(renderer, cmd, renderer.rt.frameModels, renderer.rt.frameMeshes); };
            addPass(graph, std::move(tlasPass));
        }

        // ReSTIR DI: initial candidate sampling -> temporal+spatial reuse -> resolve (1 shadow
        // ray) + shade, writing a per-pixel direct-radiance image the scene pass samples.
        // Needs the G-buffer (forced on below), the per-frame light + cluster SSBOs, and the
        // TLAS. Three compute passes; the reservoir SSBOs serialize via memory barriers the
        // graph derives from StorageWrite->StorageRead on a representative resource.
        renderer.graph.hasRestir = false;
        const bool doRestir = renderer.restir.useRestir && renderer.restir.ready && renderer.context.rtSupported &&
                              renderer.rt.tlasReady && renderer.graph.hasGbuffer && doCull /* froxel candidate lists */;
        if (doRestir)
        {
            // Per-frame writes: light SSBO + cluster SSBO (they grow), G-buffer + motion
            // samplers, and the TLAS into the resolve set.
            writeRestirFrameBindings(renderer, f);
            RgResource radianceRes = importImage(graph, renderer.restir.radiance.image, renderer.restir.radiance.view,
                                                 vk::ImageAspectFlagBits::eColor, renderer.restir.radiance.layout,
                                                 &renderer.restir.radiance.layout);
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
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.restirInitial->layout, 0,
                                       renderer.restir.initialSet, {});
                struct
                {
                    glm::mat4 invView;
                    glm::mat4 invProjection;
                    glm::uvec4 gridSize;
                    glm::uvec4 screenSize;
                    glm::vec4 zPlanes;
                } push{ invView, invProj,
                        glm::uvec4(ClusterGridX, ClusterGridY, ClusterGridZ, renderer.lighting.frameLightCount),
                        glm::uvec4(ex.width, ex.height, renderer.restir.candidateCount, fi),
                        glm::vec4(0.1f, 100.0f, 0.0f, 0.0f) };
                cmd.pushConstants(renderer.pipelines.restirInitial->layout, vk::ShaderStageFlagBits::eCompute, 0,
                                  sizeof(push), &push);
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
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.restirReuse->layout, 0,
                                       renderer.restir.reuseSet, {});
                struct
                {
                    glm::mat4 invView;
                    glm::mat4 invProjection;
                    glm::uvec4 screenSize;
                    glm::vec4 params;
                } push{ invView, invProj, glm::uvec4(ex.width, ex.height, 20u, fi),
                        glm::vec4(16.0f, histValid ? 1.0f : 0.0f, 0.0f, 0.0f) };
                cmd.pushConstants(renderer.pipelines.restirReuse->layout, vk::ShaderStageFlagBits::eCompute, 0,
                                  sizeof(push), &push);
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
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.restirResolve->layout, 0,
                                       renderer.restir.resolveSet, {});
                struct
                {
                    glm::mat4 invView;
                    glm::mat4 invProjection;
                    glm::uvec4 screenSize;
                    glm::vec4 eyePosition;
                } push{ invView, invProj, glm::uvec4(ex.width, ex.height, 0, 0), glm::vec4(eye, 0.0f) };
                cmd.pushConstants(renderer.pipelines.restirResolve->layout, vk::ShaderStageFlagBits::eCompute, 0,
                                  sizeof(push), &push);
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
            skyPass.colors.push_back(
                RgAttachment{ sceneColorAttachment, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore,
                              vk::ClearValue{ vk::ClearColorValue{ renderer.frame.clearColor } } });
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
        // The scene pass draws the compute-deformed vertices as a vertex stream; declaring
        // the read makes the graph wait the vertex-input stage on the skin compute write.
        if (doSkin)
        {
            scene.accesses.push_back(RgAccess{ deformedBuffer, RgUsage::VertexInputRead });
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
                                    vk::AttachmentStoreOp::eStore,
                                    vk::ClearValue{ vk::ClearColorValue{ renderer.frame.clearColor } } };
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
        // Persist the 1x scene depth for the post-tonemap editor overlay: store it directly
        // (no MSAA), or resolve the multisampled depth into the 1x target (MSAA samples are
        // then discarded). Without this the scene depth would not survive the pass.
        scene.depth = RgAttachment{ sceneDepth, depthLoad,
                                    msaa ? vk::AttachmentStoreOp::eDontCare : vk::AttachmentStoreOp::eStore,
                                    vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } } };
        if (msaa)
        {
            scene.depth->resolve = depth1x;
        }
        scene.renderArea = offscreen.extent;
        scene.execute = [&renderer](vk::CommandBuffer cmd)
        {
            // Opt-in sub-scopes: split the scene pass into its batched geometry and the
            // ad-hoc submission replay (editor overlay/gizmo), nested under the "scene" pass.
            {
                const GpuScope geom(gpuScopeRecorder(renderer), cmd, "scene.geometry");
                recordSceneDrawList(renderer, cmd);
            }
            {
                const GpuScope subs(gpuScopeRecorder(renderer), cmd, "scene.submissions");
                for (RenderFn& fn : renderer.frame.sceneSubmissions)
                {
                    fn(cmd);
                }
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
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.fxaa->layout, 0,
                                       renderer.descriptors.fxaaSet, {});
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
            RgResource histReadRes =
                importImage(graph, renderer.targets.history[1 - p].image, renderer.targets.history[1 - p].view,
                            vk::ImageAspectFlagBits::eColor, renderer.targets.history[1 - p].layout,
                            &renderer.targets.history[1 - p].layout);
            RgResource histWriteRes =
                importImage(graph, renderer.targets.history[p].image, renderer.targets.history[p].view,
                            vk::ImageAspectFlagBits::eColor, renderer.targets.history[p].layout,
                            &renderer.targets.history[p].layout);
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
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.taa->layout, 0,
                                       renderer.descriptors.taaSets[p], {});
                struct TaaPush
                {
                    glm::vec4 params;
                } push{ glm::vec4(TaaHistoryWeight, historyValid ? 1.0f : 0.0f, 0.0f, 0.0f) };
                cmd.pushConstants(renderer.pipelines.taa->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                                  &push);
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
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.copyColor->layout, 0,
                                       renderer.ssao.copyColorSet, {});
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

        // Editor overlay: gizmo handles + entity billboards, composited into the 1x resolved
        // sceneColor AFTER tonemap (the last sceneColor writer) so present-only blits it too.
        // The overlay runs at e1: by here sceneColor is always the 1x offscreen regardless of AA.
        if (!renderer.overlay.vertices.empty() && renderer.pipelines.overlay)
        {
            RgPass overlay;
            overlay.name = "editor-overlay";
            overlay.kind = RgPassKind::Graphics;
            overlay.colors.push_back(RgAttachment{
                renderer.graph.sceneColor, vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore, {} });
            // Read-only scene depth: the depth-tested range (camera frustums) is occluded by
            // scene geometry; the on-top range ignores it. Bound for both so the PSOs, which
            // declare the depth format, stay render-pass compatible.
            overlay.depth = RgAttachment{ depth1x, vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eDontCare, {} };
            overlay.renderArea = offscreen.extent;
            const u32 vertexCount = static_cast<u32>(renderer.overlay.vertices.size());
            const u32 depthTestedCount = std::min(renderer.overlay.depthTestedCount, vertexCount);
            overlay.execute = [&renderer, vertexCount, depthTestedCount](vk::CommandBuffer cmd)
            {
                const u32 f = renderer.frame.index;
                if (renderer.overlay.capacity[f] < vertexCount)
                {
                    auto buffer = makeMappedVertexBuffer(renderer, static_cast<vk::DeviceSize>(vertexCount) *
                                                                       sizeof(OverlayVertex));
                    if (!buffer)
                    {
                        logError(buffer.error());
                        return;
                    }
                    renderer.overlay.buffers[f] = *buffer;
                    renderer.overlay.capacity[f] = vertexCount;
                }
                Ref<Buffer> buffer = renderer.overlay.buffers[f];
                if (!buffer || buffer->mapped == nullptr)
                {
                    return;
                }
                std::memcpy(buffer->mapped, renderer.overlay.vertices.data(),
                            static_cast<std::size_t>(vertexCount) * sizeof(OverlayVertex));

                vk::Viewport viewport{};
                viewport.x = 0.0f;
                viewport.y = 0.0f;
                viewport.width = static_cast<float>(renderer.targets.offscreen.extent.width);
                viewport.height = static_cast<float>(renderer.targets.offscreen.extent.height);
                viewport.minDepth = 0.0f;
                viewport.maxDepth = 1.0f;
                cmd.setViewport(0, viewport);
                cmd.setScissor(0, vk::Rect2D{ vk::Offset2D{ 0, 0 }, renderer.targets.offscreen.extent });
                const vk::DeviceSize offset = 0;
                cmd.bindVertexBuffers(0, buffer->buffer, offset);
                // Depth-tested range first (occluded by geometry), then the on-top range
                // composited over it.
                if (depthTestedCount > 0 && renderer.pipelines.overlayDepth)
                {
                    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, renderer.pipelines.overlayDepth->pipeline);
                    cmd.draw(depthTestedCount, 1, 0, 0);
                }
                if (vertexCount > depthTestedCount)
                {
                    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, renderer.pipelines.overlay->pipeline);
                    cmd.draw(vertexCount - depthTestedCount, 1, depthTestedCount, 0);
                }
            };
            addPass(graph, std::move(overlay));
        }
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
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, renderer.pipelines.tonemap->layout, 0,
                                   renderer.descriptors.tonemapSet, {});
            const f32 exposure = std::exp2(renderer.exposureEv);
            cmd.pushConstants(renderer.pipelines.tonemap->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(f32),
                              &exposure);
            const vk::Extent2D extent = renderer.targets.offscreen.extent;
            cmd.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        };
        addPass(graph, std::move(pass));
    }

    void setPresentViewportOnly(Renderer& renderer, bool enabled)
    {
        renderer.presentViewportOnly = enabled;
    }

    auto makeMappedVertexBuffer(Renderer& renderer, vk::DeviceSize bytes) -> Result<Ref<Buffer>>
    {
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO;
        alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer raw = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        VmaAllocationInfo mapped{};
        if (vmaCreateBuffer(renderer.context.allocator, &info, &alloc, &raw, &allocation, &mapped) != VK_SUCCESS)
        {
            return Err(std::string{ "makeMappedVertexBuffer: vmaCreateBuffer failed" });
        }
        Buffer buffer;
        buffer.allocator = renderer.context.allocator;
        buffer.buffer = vk::Buffer{ raw };
        buffer.alloc = allocation;
        buffer.mapped = mapped.pMappedData;
        buffer.size = bytes;
        return std::make_shared<Buffer>(std::move(buffer));
    }

    void submitOverlay(Renderer& renderer, std::vector<OverlayVertex> depthTested, std::vector<OverlayVertex> onTop)
    {
        renderer.overlay.depthTestedCount = static_cast<u32>(depthTested.size());
        renderer.overlay.vertices = std::move(depthTested);
        renderer.overlay.vertices.insert(renderer.overlay.vertices.end(), onTop.begin(), onTop.end());
    }

    // Native-viewport host: blit the post-processed offscreen color straight to the
    // swapchain (Nearest, full extent) and transition it to present, in place of the ui
    // pass when the scene is embedded in an external window.
    void presentViewportToSwapchain(Renderer& renderer, vk::CommandBuffer cmd)
    {
        Image& src = renderer.targets.offscreen;
        const vk::Image swap = renderer.swapchain.images[renderer.frame.imageIndex];

        transitionImage(cmd, src.image, src.layout, vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                        vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eColorAttachmentWrite,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        src.layout = vk::ImageLayout::eTransferSrcOptimal;

        transitionImage(cmd, swap, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                        vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);

        vk::ImageBlit blit{};
        blit.srcSubresource = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
        blit.srcOffsets[0] = vk::Offset3D{ 0, 0, 0 };
        blit.srcOffsets[1] = vk::Offset3D{ static_cast<i32>(src.extent.width), static_cast<i32>(src.extent.height), 1 };
        blit.dstSubresource = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
        blit.dstOffsets[0] = vk::Offset3D{ 0, 0, 0 };
        blit.dstOffsets[1] = vk::Offset3D{ static_cast<i32>(renderer.swapchain.extent.width),
                                           static_cast<i32>(renderer.swapchain.extent.height), 1 };
        cmd.blitImage(src.image, vk::ImageLayout::eTransferSrcOptimal, swap, vk::ImageLayout::eTransferDstOptimal, blit,
                      vk::Filter::eNearest);

        transitionImage(cmd, swap, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::ePresentSrcKHR,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                        vk::PipelineStageFlagBits2::eBottomOfPipe, vk::AccessFlagBits2::eNone);
    }

    // Records the offscreen→BGRA8→staging readback into the frame's own command buffer —
    // the GPU does the RGBA16F→BGRA8 conversion via blit, and the only CPU sync is the
    // frame fence beginFrame already waits on. No extra submits, no waitIdle.
    void recordShmPublishCopy(Renderer& renderer, vk::CommandBuffer cmd)
    {
        Image& src = renderer.targets.offscreen;
        const u32 width = src.extent.width;
        const u32 height = src.extent.height;
        if (width == 0 || height == 0)
        {
            return;
        }
        ShmPublishSlot& slot = renderer.shmPublish.slots[renderer.frame.index];
        if (!ensureShmPublishSlot(renderer, slot, width, height))
        {
            return;
        }

        transitionImage(cmd, src.image, src.layout, vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                        vk::AccessFlagBits2::eShaderStorageWrite | vk::AccessFlagBits2::eColorAttachmentWrite,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
        src.layout = vk::ImageLayout::eTransferSrcOptimal;

        transitionImage(cmd, slot.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                        vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite);

        vk::ImageBlit blit{};
        blit.srcSubresource = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
        blit.srcOffsets[0] = vk::Offset3D{ 0, 0, 0 };
        blit.srcOffsets[1] = vk::Offset3D{ static_cast<i32>(width), static_cast<i32>(height), 1 };
        blit.dstSubresource = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
        blit.dstOffsets[0] = vk::Offset3D{ 0, 0, 0 };
        blit.dstOffsets[1] = vk::Offset3D{ static_cast<i32>(width), static_cast<i32>(height), 1 };
        cmd.blitImage(src.image, vk::ImageLayout::eTransferSrcOptimal, slot.image, vk::ImageLayout::eTransferDstOptimal,
                      blit, vk::Filter::eNearest);

        transitionImage(cmd, slot.image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                        vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);

        vk::BufferImageCopy region{};
        region.imageSubresource = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
        region.imageExtent = vk::Extent3D{ width, height, 1 };
        cmd.copyImageToBuffer(slot.image, vk::ImageLayout::eTransferSrcOptimal, vk::Buffer{ slot.buffer }, region);

        // Make the staging write visible to host reads once the frame fence signals.
        vk::BufferMemoryBarrier2 hostBarrier{};
        hostBarrier.srcStageMask = vk::PipelineStageFlagBits2::eCopy;
        hostBarrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        hostBarrier.dstStageMask = vk::PipelineStageFlagBits2::eHost;
        hostBarrier.dstAccessMask = vk::AccessFlagBits2::eHostRead;
        hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hostBarrier.buffer = vk::Buffer{ slot.buffer };
        hostBarrier.offset = 0;
        hostBarrier.size = VK_WHOLE_SIZE;
        vk::DependencyInfo dependency{};
        dependency.setBufferMemoryBarriers(hostBarrier);
        cmd.pipelineBarrier2(dependency);

        slot.valid = true;
    }

    void endFrame(Renderer& renderer)
    {
        FrameData& frame = renderer.frame.frames[renderer.frame.index];
        RenderGraph& graph = renderer.graph.current;

        // In native-viewport (present-only) mode no graph pass writes the swapchain;
        // presentViewportToSwapchain does the offscreen->swapchain blit below instead.
        if (!renderer.presentViewportOnly)
        {
            // The ui pass samples the (now post-processed) offscreen color and composites any
            // submitUi overlay closures into the swapchain. Added last so app-authored passes land
            // before it. (Unused by the present-only host, which blits via presentViewportToSwapchain.)
            RgPass ui;
            ui.name = "ui";
            ui.kind = RgPassKind::Graphics;
            ui.accesses = { RgAccess{ renderer.graph.sceneColor, RgUsage::SampledRead } };
            ui.colors.push_back(RgAttachment{ renderer.graph.swapImage, vk::AttachmentLoadOp::eClear,
                                              vk::AttachmentStoreOp::eStore,
                                              vk::ClearValue{ vk::ClearColorValue{ renderer.frame.clearColor } } });
            ui.renderArea = renderer.swapchain.extent;
            ui.execute = [&renderer](vk::CommandBuffer cmd)
            {
                for (RenderFn& fn : renderer.frame.uiSubmissions)
                {
                    fn(cmd);
                }
            };
            addPass(graph, std::move(ui));
        }

        // Debug-utils pass markers are always-on (independent of the profiler); GPU
        // timestamps ride along only when a profiler mode armed the per-frame pool.
        const RgDebugLabels* labels =
            renderer.context.debugLabels.begin != nullptr ? &renderer.context.debugLabels : nullptr;
        // Arm the GPU scope recorder for this frame (or clear it, so any pass-body sub-scope
        // is a no-op when not recording). It lives on the profiler — not a local — so pass
        // bodies reach the same recorder via gpuScopeRecorder to open child scopes.
        RgTimestamps* tsPtr = nullptr;
        if (renderer.profiler.mode != ProfilerMode::Off && renderer.profiler.poolsReady)
        {
            RgTimestamps& sr = renderer.profiler.scopeRecorder;
            sr.pool = renderer.profiler.timestampPools[renderer.frame.index];
            sr.capacity = 2 * MaxProfiledScopes;
            sr.records = &renderer.profiler.recordedScopes[renderer.frame.index];
            sr.nextSlot = 0;
            sr.openScope = -1;
            sr.depth = 0;
            // Arm pipeline-statistics queries only in PipelineStats mode (and when the pools
            // exist); otherwise leave the stats pool null so executeRenderGraph skips them.
            const bool statsOn = renderer.profiler.mode == ProfilerMode::PipelineStats &&
                                 renderer.profiler.statsPools[renderer.frame.index];
            sr.statsPool = statsOn ? renderer.profiler.statsPools[renderer.frame.index] : vk::QueryPool{};
            sr.statsCapacity = statsOn ? MaxProfiledScopes : 0;
            sr.nextStatsSlot = 0;
            tsPtr = &sr;
        }
        else
        {
            renderer.profiler.scopeRecorder = RgTimestamps{};
        }
        const CpuRecorder rec = cpuRecorder(renderer);
        {
            const CpuScope scope(&rec, "execute-render-graph");
            executeRenderGraph(graph, frame.commandBuffer, tsPtr, labels, &rec);
        }

        // Store this frame's camera viewProj as next frame's "previous" for TAA motion
        // vectors. Only valid once a scene draw list was submitted this frame.
        if (renderer.frame.sceneDrawList.valid)
        {
            renderer.prevViewProj = renderer.frame.sceneDrawList.viewProj;
            renderer.prevViewProjValid = true;
        }

        // The swapchain image is only safely owned in-frame, so a pending capture
        // is copied here, between the ui pass and present; its COLOR->PRESENT
        // transition is folded into captureImageToBuffer's toLayout.
        VkBuffer captureBuffer = VK_NULL_HANDLE;
        VmaAllocation captureAlloc = nullptr;
        VmaAllocationInfo captureInfo{};
        vk::Extent2D captureExtent{};
        bool doCapture = renderer.captureNextSwapchainPath.has_value();
        // Present-only never leaves the swapchain in eColorAttachmentOptimal, so the
        // window-capture path is invalid here (use screenshot target=viewport instead).
        if (renderer.presentViewportOnly)
        {
            doCapture = false;
        }
        if (doCapture)
        {
            captureExtent = renderer.swapchain.extent;
            const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(captureExtent.width) * captureExtent.height * 4;
            Result<void> created = newHostCaptureBuffer(renderer, bytes, captureBuffer, captureAlloc, captureInfo);
            if (!created)
            {
                logError(created.error());
                doCapture = false;
                renderer.captureNextSwapchainPath.reset();
            }
        }
        if (renderer.shmPublish.enabled)
        {
            recordShmPublishCopy(renderer, frame.commandBuffer);
        }
        else if (renderer.presentViewportOnly)
        {
            presentViewportToSwapchain(renderer, frame.commandBuffer);
        }
        else if (doCapture)
        {
            captureImageToBuffer(
                frame.commandBuffer, renderer.swapchain.images[renderer.frame.imageIndex], captureExtent,
                vk::ImageLayout::eColorAttachmentOptimal, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                vk::AccessFlagBits2::eColorAttachmentWrite, vk::ImageLayout::ePresentSrcKHR,
                vk::PipelineStageFlagBits2::eBottomOfPipe, vk::AccessFlagBits2::eNone, vk::Buffer{ captureBuffer });
        }
        else
        {
            transitionImage(frame.commandBuffer, renderer.swapchain.images[renderer.frame.imageIndex],
                            vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR,
                            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                            vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eBottomOfPipe,
                            vk::AccessFlagBits2::eNone);
        }

        static_cast<void>(frame.commandBuffer.end());

        if (renderer.shmPublish.enabled)
        {
            const CpuScope presentScope(&rec, "submit-present");
            // No swapchain image was acquired and nothing presents: submit with the frame
            // fence only, so the loop is paced purely by GPU completion (frames in flight).
            vk::CommandBufferSubmitInfo cmdInfo{};
            cmdInfo.commandBuffer = frame.commandBuffer;
            vk::SubmitInfo2 submitInfo{};
            submitInfo.setCommandBufferInfos(cmdInfo);
            static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, frame.inFlight));
        }
        else
        {
            const CpuScope presentScope(&rec, "submit-present");
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
        }

        // The recorded copy is now submitted; idle so it completed, then write the PNG.
        if (doCapture && captureBuffer != VK_NULL_HANDLE)
        {
            static_cast<void>(renderer.context.device.waitIdle());
            vmaInvalidateAllocation(renderer.context.allocator, captureAlloc, 0, VK_WHOLE_SIZE);
            auto wrote =
                writeBufferToPng(static_cast<const unsigned char*>(captureInfo.pMappedData), captureExtent.width,
                                 captureExtent.height, renderer.swapchain.format, *renderer.captureNextSwapchainPath);
            if (!wrote)
            {
                logError(wrote.error());
            }
            else
            {
                logInfo(std::format("captured window ({}x{}) to {}", captureExtent.width, captureExtent.height,
                                    *renderer.captureNextSwapchainPath));
            }
            vmaDestroyBuffer(renderer.context.allocator, captureBuffer, captureAlloc);
            renderer.captureNextSwapchainPath.reset();
        }

        const u64 nowNs = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count());
        f32 dtSec = 0.0f;
        if (renderer.lastFrameNs != 0)
        {
            const f32 deltaMs = static_cast<f32>(nowNs - renderer.lastFrameNs) / 1.0e6f;
            dtSec = deltaMs / 1000.0f;
            renderer.frameMs = renderer.frameMs == 0.0f ? deltaMs : renderer.frameMs * 0.9f + deltaMs * 0.1f;
        }
        renderer.lastFrameNs = nowNs;

        // CPU split: busy = the beginFrame→endFrame window minus the blocking waits; the
        // waits themselves are the GPU-bound signal. Both EMA-smoothed for the headline.
        const u64 frameSpanNs = nowNs > renderer.frameCpuStartNs ? nowNs - renderer.frameCpuStartNs : 0;
        const u64 busyNs = frameSpanNs > renderer.frameWaitNs ? frameSpanNs - renderer.frameWaitNs : 0;
        const f32 busyMs = static_cast<f32>(busyNs) / 1.0e6f;
        const f32 waitMs = static_cast<f32>(renderer.frameWaitNs) / 1.0e6f;
        renderer.cpuFrameMs = renderer.cpuFrameMs == 0.0f ? busyMs : renderer.cpuFrameMs * 0.9f + busyMs * 0.1f;
        renderer.cpuWaitMs = renderer.cpuWaitMs == 0.0f ? waitMs : renderer.cpuWaitMs * 0.9f + waitMs * 0.1f;

        // Record the raw frame into the history ring (always on; the distribution stays
        // honest only if it sees every frame, un-smoothed). A frame is a stutter when its
        // time exceeds both 2× the previous-3 average and an absolute floor of 2× budget —
        // the relative rule catches hitches at any frame rate, the floor rejects noise.
        {
            const f32 frameTime = busyMs + waitMs;
            if (renderer.frameRingCount >= 3)
            {
                f32 sum3 = 0.0f;
                for (u32 k = 1; k <= 3; k = k + 1)
                {
                    const u32 idx = (renderer.frameRingHead + FrameHistoryCapacity - k) % FrameHistoryCapacity;
                    sum3 = sum3 + renderer.frameRing[idx].cpuMs + renderer.frameRing[idx].cpuWaitMs;
                }
                const f32 avg3 = sum3 / 3.0f;
                const f32 budget = perfBudgetMs(renderer.perfConfig);
                if (frameTime > 2.0f * avg3 && frameTime > 2.0f * budget)
                {
                    renderer.stutterCount = renderer.stutterCount + 1;
                    renderer.lastStutterNs = nowNs;
                }
            }
            renderer.frameRing[renderer.frameRingHead] =
                FrameSample{ renderer.frameSerial, busyMs, renderer.profiler.lastGpuTotalMs, waitMs };
            renderer.frameSerial = renderer.frameSerial + 1;
            renderer.frameRingHead = (renderer.frameRingHead + 1) % FrameHistoryCapacity;
            if (renderer.frameRingCount < FrameHistoryCapacity)
            {
                renderer.frameRingCount = renderer.frameRingCount + 1;
            }
            // Run the alarm detectors on this frame (after the ring push, so MAD/burn-rate
            // windows include it). Pure CPU bookkeeping — appends to the event ring, never blocks.
            tickAlarms(renderer, frameTime, dtSec, nowNs);
        }

        // One primary command buffer, one submit2 per frame (both present and shm paths).
        renderer.stats.commandBuffers = 1;
        renderer.stats.queueSubmits = 1;
        if (renderer.profiler.mode != ProfilerMode::Off)
        {
            readVramBudget(renderer);
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
        RenderStats stats = renderer.stats;
        stats.frameMs = renderer.frameMs;
        stats.fps = renderer.frameMs > 0.0f ? 1000.0f / renderer.frameMs : 0.0f;
        stats.gpuMs = renderer.gpuFrameMs;
        stats.cpuFrameMs = renderer.cpuFrameMs;
        stats.cpuWaitMs = renderer.cpuWaitMs;
        stats.softwareGpu = renderer.softwareGpu;
        stats.profilerMode = renderer.profiler.mode;
        return stats;
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
        return renderer.ssao.ready && (renderer.ssao.useSsao || renderer.ssao.useContact || renderer.ssao.useSsgi);
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
        // Capture this frame's static instances; the TLAS-build graph pass consumes them (plus
        // the draw list's skinned instances) when RT shadows are on. Skinned instances are added
        // by submitDrawList, which runs after this — so the pass is also armed when RT is on and
        // the static list is empty (the gate re-checks the skinned list in beginFrameGraph).
        renderer.rt.frameModels = std::move(models);
        renderer.rt.frameMeshes = std::move(meshes);
        renderer.rt.buildPending = renderer.context.rtSupported && renderer.rt.useRtShadows;
    }

    void buildTlas(Renderer& renderer, vk::CommandBuffer cmd, const std::vector<glm::mat4>& models,
                   const std::vector<Ref<GpuMesh>>& meshes)
    {
        renderer.rt.tlasReady = false;
        renderer.rt.skinnedBlasCount = 0;
        const std::vector<SkinnedRtInstance>& skinned = renderer.frame.sceneDrawList.skinnedRtInstances;
        if (!renderer.context.rtSupported || (models.empty() && skinned.empty()))
        {
            return;
        }
        const u32 f = renderer.frame.index;
        // Refit each skinned instance's per-frame BLAS first (records into the same cmd; emits the
        // AS-build -> AS-build barrier so this TLAS build waits on them).
        recordSkinnedBlasBuilds(renderer, cmd, f, skinned);
        // Pack one VkAccelerationStructureInstanceKHR per mesh instance that has a BLAS.
        std::vector<VkAccelerationStructureInstanceKHR> instances;
        instances.reserve(models.size() + skinned.size());
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
        // Skinned instances reference their refit BLAS with an IDENTITY transform: the deformed
        // vertices are already in world space (the skin kernel bakes worldBone * inverseBind in,
        // without the model matrix), so any extra transform would double-apply the placement.
        for (const SkinnedRtInstance& s : skinned)
        {
            auto found = renderer.rt.skinnedBlas[f].find(s.entity);
            if (found == renderer.rt.skinnedBlas[f].end() || !found->second.as)
            {
                continue;
            }
            VkAccelerationStructureInstanceKHR inst{};
            inst.transform.matrix[0][0] = 1.0f;
            inst.transform.matrix[1][1] = 1.0f;
            inst.transform.matrix[2][2] = 1.0f;
            inst.instanceCustomIndex = static_cast<u32>(instances.size());
            inst.mask = 0xFF;
            inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            inst.accelerationStructureReference = static_cast<u64>(found->second.as->address);
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

    void setDdgiScene(Renderer& renderer, const std::vector<glm::vec4>& boxMins, const std::vector<glm::vec4>& boxMaxs,
                      const std::vector<glm::vec4>& boxAlbedos, glm::vec3 volumeMin, glm::vec3 volumeExtent,
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

    void setSsaoCamera(Renderer& renderer, const glm::mat4& view, const glm::mat4& proj, glm::vec3 sunDirectionWorld)
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

    void setSkinning(Renderer& renderer, bool enabled)
    {
        renderer.useSkinning = enabled;
    }

    auto skinningEnabled(const Renderer& renderer) -> bool
    {
        return renderer.useSkinning;
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
