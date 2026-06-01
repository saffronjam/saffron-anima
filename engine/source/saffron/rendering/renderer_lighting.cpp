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
    // Ensures the current frame's punctual-light buffer holds at least `count` lights,
    // growing to the next power of two (never shrinking) and rewriting its set.
    auto ensureLightCapacity(Renderer& renderer, u32 frame, u32 count) -> Result<void>
    {
        if (renderer.lighting.lightListBuffers[frame] && renderer.lighting.lightListCapacity[frame] >= count)
        {
            return {};
        }
        u32 capacity = renderer.lighting.lightListCapacity[frame];
        if (capacity == 0)
        {
            capacity = LightListInitial;
        }
        while (capacity < count)
        {
            capacity = capacity * 2;
        }
        Result<Ref<Buffer>> buffer =
            makeMappedStorageBuffer(renderer, static_cast<vk::DeviceSize>(capacity) * sizeof(GpuLight));
        if (!buffer)
        {
            return Err(buffer.error());
        }
        renderer.lighting.lightListBuffers[frame] = *buffer;
        renderer.lighting.lightListCapacity[frame] = capacity;

        // Both the fragment lighting set (binding 1) and the compute cluster set
        // (binding 1) read this buffer — rewrite both to the grown allocation.
        vk::DescriptorBufferInfo bufferInfo{ (*buffer)->buffer, 0, (*buffer)->size };
        std::array<vk::WriteDescriptorSet, 2> writes{};
        writes[0].dstSet = renderer.lighting.lightSets[frame];
        writes[0].dstBinding = 1;
        writes[0].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[0].setBufferInfo(bufferInfo);
        writes[1].dstSet = renderer.lighting.clusterSets[frame];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[1].setBufferInfo(bufferInfo);
        renderer.context.device.updateDescriptorSets(writes, {});
        return {};
    }
    void setDirectionalLight(Renderer& renderer, glm::vec3 direction, glm::vec3 color, f32 intensity, f32 ambient)
    {
        setSceneLighting(renderer, direction, color, intensity, glm::vec3(ambient), glm::vec3(0.0f), {});
    }

    void setSceneLighting(Renderer& renderer, glm::vec3 direction, glm::vec3 color, f32 intensity,
                          glm::vec3 ambient, glm::vec3 eyePosition, const std::vector<GpuLight>& lights)
    {
        // Write the current frame's copies; beginFrame already waited on its fence, so
        // no in-flight frame is reading them.
        const u32 frame = renderer.frame.index;
        if (renderer.lighting.lightMapped[frame] == nullptr)
        {
            return;
        }
        const u32 count = static_cast<u32>(lights.size());
        if (count > 0)
        {
            if (Result<void> ok = ensureLightCapacity(renderer, frame, count); !ok)
            {
                logError(ok.error());
                return;
            }
            const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(count) * sizeof(GpuLight);
            std::memcpy(renderer.lighting.lightListBuffers[frame]->mapped, lights.data(), bytes);
            vmaFlushAllocation(renderer.context.allocator, renderer.lighting.lightListBuffers[frame]->alloc, 0, bytes);
        }

        LightUbo ubo;
        // directionAmbient.w keeps a scalar ambient luminance for legacy reads; the shader's
        // non-IBL fallback uses the RGB ambientColor below.
        ubo.directionAmbient = glm::vec4(glm::normalize(direction), (ambient.r + ambient.g + ambient.b) / 3.0f);
        ubo.colorIntensity = glm::vec4(color, intensity);
        ubo.ambientColor = glm::vec4(ambient, 1.0f);
        // counts: x = punctual count, y = directional-shadow flag, z = IBL-ambient flag,
        // w = SSAO flag (the mesh multiplies the AO map into the ambient term). screenFlags
        // x = contact-shadow flag, y = SSGI flag. Driven off the same enable state
        // beginFrameGraph uses to schedule the passes (set this frame, before this call), so
        // a flagged map is always produced + valid.
        const u32 iblFlag = (renderer.ibl.useIbl && renderer.ibl.ready) ? 1u : 0u;
        const u32 ssaoFlag = (renderer.ssao.useSsao && renderer.ssao.ready) ? 1u : 0u;
        const u32 contactFlag = (renderer.ssao.useContact && renderer.ssao.ready) ? 1u : 0u;
        const u32 ssgiFlag = (renderer.ssao.useSsgi && renderer.ssao.ready) ? 1u : 0u;
        const u32 ddgiFlag = (renderer.ddgi.useDdgi && renderer.ddgi.ready) ? 1u : 0u;
        ubo.counts = glm::uvec4(count, renderer.lighting.shadowPending ? 1u : 0u, iblFlag, ssaoFlag);
        // screenFlags.w = ReSTIR direct-lighting flag: the mesh replaces its punctual loop
        // with the resolved ReSTIR radiance buffer.
        const u32 restirFlag = (renderer.restir.useRestir && renderer.restir.ready && renderer.context.rtSupported) ? 1u : 0u;
        ubo.screenFlags = glm::uvec4(contactFlag, ssgiFlag, ddgiFlag, restirFlag);
        ubo.ddgiVolumeMin = glm::vec4(renderer.ddgi.volumeMin, 0.0f);
        ubo.ddgiVolumeExtent = glm::vec4(renderer.ddgi.volumeExtent, 0.0f);
        ubo.ddgiProbeCount = glm::uvec4(DdgiProbesX, DdgiProbesY, DdgiProbesZ, DdgiIrrInterior);
        ubo.eyePosition = glm::vec4(eyePosition, 0.0f);
        ubo.shadowViewProj = renderer.lighting.shadowViewProj;
        ubo.spotShadowViewProj = renderer.lighting.spotShadowViewProj;
        ubo.spotShadow = glm::uvec4(renderer.lighting.spotShadowLightIndex,
                                    renderer.lighting.spotShadowPending ? 1u : 0u, 0, 0);
        ubo.pointShadow = glm::vec4(renderer.lighting.pointShadowPos, renderer.lighting.pointShadowFar);
        // pointShadowMeta.z = RT-shadow flag (the mesh traces a ray-query shadow per light
        // instead of / in addition to the shadow maps). Requires rtSupported + a built TLAS.
        const u32 rtFlag = (renderer.rt.useRtShadows && renderer.context.rtSupported) ? 1u : 0u;
        ubo.pointShadowMeta = glm::uvec4(renderer.lighting.pointShadowLightIndex,
                                         renderer.lighting.pointShadowPending ? 1u : 0u, rtFlag, 0);
        std::memcpy(renderer.lighting.lightMapped[frame], &ubo, sizeof(ubo));
        vmaFlushAllocation(renderer.context.allocator, renderer.lighting.lightAllocs[frame], 0, sizeof(ubo));
        renderer.lighting.frameLightCount = count;
    }

    void setClusterCamera(Renderer& renderer, const glm::mat4& view, const glm::mat4& proj,
                          f32 nearPlane, f32 farPlane)
    {
        const u32 frame = renderer.frame.index;
        if (renderer.lighting.clusterParamMapped[frame] == nullptr)
        {
            return;
        }
        ClusterParams params;
        params.view = view;
        params.inverseProjection = glm::inverse(proj);
        params.gridSize = glm::uvec4(ClusterGridX, ClusterGridY, ClusterGridZ, renderer.lighting.frameLightCount);
        params.screenSize = glm::uvec4(viewportWidth(renderer), viewportHeight(renderer),
                                       renderer.lighting.useClustered ? 1u : 0u, 0u);
        params.zPlanes = glm::vec4(nearPlane, farPlane, 0.0f, 0.0f);
        std::memcpy(renderer.lighting.clusterParamMapped[frame], &params, sizeof(params));
        vmaFlushAllocation(renderer.context.allocator, renderer.lighting.clusterParamAllocs[frame], 0, sizeof(params));
        renderer.lighting.clusterDispatchPending = renderer.lighting.useClustered && renderer.lighting.frameLightCount > 0;
    }

    void setClustered(Renderer& renderer, bool enabled)
    {
        renderer.lighting.useClustered = enabled;
    }

    auto clusteredEnabled(const Renderer& renderer) -> bool
    {
        return renderer.lighting.useClustered;
    }

    void submitSky(Renderer& renderer, const SkyRenderSettings& settings)
    {
        renderer.sky.mode = settings.mode;
        renderer.sky.clearColor = settings.clearColor;
        renderer.sky.intensity = settings.intensity;
        renderer.sky.rotation = settings.rotation;
        renderer.sky.visible = settings.visible;
        renderer.sky.textureIndex = settings.textureIndex;
    }
}
