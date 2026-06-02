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
    // The largest sample count not exceeding `requested` that the MSAA color+depth formats
    // accept (e1 if none) — a count valid as a framebuffer limit can still be unsupported for
    // a specific format, and creating an image with it is invalid (VUID-VkImageCreateInfo-samples).
    static auto clampSampleCount(vk::SampleCountFlags supported, vk::SampleCountFlagBits requested)
        -> vk::SampleCountFlagBits
    {
        for (vk::SampleCountFlagBits candidate :
             { vk::SampleCountFlagBits::e8, vk::SampleCountFlagBits::e4, vk::SampleCountFlagBits::e2 })
        {
            if (static_cast<u32>(candidate) <= static_cast<u32>(requested) && (supported & candidate))
            {
                return candidate;
            }
        }
        return vk::SampleCountFlagBits::e1;
    }

    void setDepthPrepass(Renderer& renderer, bool enabled)
    {
        renderer.useDepthPrepass = enabled;
    }

    auto depthPrepassEnabled(const Renderer& renderer) -> bool
    {
        return renderer.useDepthPrepass;
    }

    void setAa(Renderer& renderer, u32 msaaSamples, bool fxaa, bool taa)
    {
        // The three AA modes are mutually exclusive; MSAA wins if samples > 1 is requested.
        vk::SampleCountFlagBits count = vk::SampleCountFlagBits::e1;
        if (msaaSamples >= 8) { count = vk::SampleCountFlagBits::e8; }
        else if (msaaSamples >= 4) { count = vk::SampleCountFlagBits::e4; }
        else if (msaaSamples >= 2) { count = vk::SampleCountFlagBits::e2; }
        count = clampSampleCount(renderer.targets.supportedSampleCounts, count);

        waitGpuIdle(renderer);
        renderer.targets.sampleCount = count;
        renderer.targets.fxaaEnabled = fxaa;
        renderer.targets.taaEnabled = taa;
        recreateMsaaTargets(renderer);
        recreateFxaaTarget(renderer);   // owns the 1x scratch FXAA + TAA both render into
        recreateTaaTargets(renderer);   // binds scratch into the TAA set, so run it after

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

        // The sky PSO bakes the sample count too — rebuild it for the new scene color target.
        Result<Ref<Pipeline>> skyPipe = makeSkyPipeline(renderer);
        if (skyPipe)
        {
            renderer.sky.pipeline = *skyPipe;
        }
        else
        {
            logError(skyPipe.error());
        }
    }

    auto aaMode(const Renderer& renderer) -> std::string
    {
        if (renderer.targets.fxaaEnabled)
        {
            return "fxaa";
        }
        if (renderer.targets.taaEnabled)
        {
            return "taa";
        }
        const u32 n = static_cast<u32>(renderer.targets.sampleCount);
        if (n <= 1)
        {
            return "off";
        }
        return std::format("msaa{}", n);
    }
}
