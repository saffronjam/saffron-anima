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
    void setPostProcess(Renderer& renderer, bool enabled)
    {
        renderer.usePostProcess = enabled;
    }

    auto postProcessEnabled(const Renderer& renderer) -> bool
    {
        return renderer.usePostProcess;
    }

    void setDepthPrepass(Renderer& renderer, bool enabled)
    {
        renderer.useDepthPrepass = enabled;
    }

    auto depthPrepassEnabled(const Renderer& renderer) -> bool
    {
        return renderer.useDepthPrepass;
    }

    void setAa(Renderer& renderer, u32 msaaSamples, bool fxaa)
    {
        vk::SampleCountFlagBits count = vk::SampleCountFlagBits::e1;
        if (msaaSamples >= 8) { count = vk::SampleCountFlagBits::e8; }
        else if (msaaSamples >= 4) { count = vk::SampleCountFlagBits::e4; }
        else if (msaaSamples >= 2) { count = vk::SampleCountFlagBits::e2; }
        if (static_cast<u32>(count) > static_cast<u32>(renderer.targets.maxSampleCount))
        {
            count = renderer.targets.maxSampleCount;
        }

        waitGpuIdle(renderer);
        renderer.targets.sampleCount = count;
        renderer.targets.fxaaEnabled = fxaa;
        recreateMsaaTargets(renderer);
        recreateFxaaTarget(renderer);

        // The mesh + depth-prepass PSOs bake the sample count — rebuild them.
        renderer.pipelines.cache.clear();
        Result<Ref<Pipeline>> depthPrepass =
            makeDepthPrepassPipeline(renderer, "shaders/mesh.spv");
        if (depthPrepass)
        {
            renderer.pipelines.depthPrepass = *depthPrepass;
        }
        else
        {
            logError(depthPrepass.error());
        }
    }

    auto aaMode(const Renderer& renderer) -> std::string
    {
        if (renderer.targets.fxaaEnabled)
        {
            return "fxaa";
        }
        const u32 n = static_cast<u32>(renderer.targets.sampleCount);
        if (n <= 1)
        {
            return "off";
        }
        return std::format("msaa{}", n);
    }
}
