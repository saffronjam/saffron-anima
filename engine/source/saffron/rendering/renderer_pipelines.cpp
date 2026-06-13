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
    auto newMeshPipeline(Renderer& renderer, std::string_view shaderName, bool unlit, bool skinned, bool wireframe)
        -> Result<Ref<Pipeline>>
    {
        std::string path = assetPath(shaderName);
        auto moduleResult = loadShaderModule(renderer.context.device, path);
        if (!moduleResult)
        {
            return Err(moduleResult.error());
        }
        vk::ShaderModule shaderModule = *moduleResult;

        // The übershader's unlit branch is a specialization constant (id 0) baked into the
        // fragment stage, so this PSO is the lit or the unlit variant.
        const vk::Bool32 unlitValue = static_cast<vk::Bool32>(unlit);
        vk::SpecializationMapEntry specEntry{};
        specEntry.constantID = 0;
        specEntry.offset = 0;
        specEntry.size = sizeof(vk::Bool32);
        vk::SpecializationInfo specInfo{};
        specInfo.setMapEntries(specEntry);
        specInfo.dataSize = sizeof(vk::Bool32);
        specInfo.pData = &unlitValue;

        std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
        stages[0].stage = vk::ShaderStageFlagBits::eVertex;
        stages[0].module = shaderModule;
        stages[0].pName = "vertexMain";
        if (skinned)
        {
            stages[0].pName = "vertexMainSkinned";
        }
        stages[1].stage = vk::ShaderStageFlagBits::eFragment;
        stages[1].module = shaderModule;
        stages[1].pName = "fragmentMain";
        stages[1].pSpecializationInfo = &specInfo;

        // The skinned variant adds a second vertex stream (VertexSkin: joints + weights)
        // on binding 1; the base layout stays untouched so unskinned PSOs are unchanged.
        std::array<vk::VertexInputBindingDescription, 2> bindings{};
        bindings[0].binding = 0;
        bindings[0].stride = sizeof(Vertex);
        bindings[0].inputRate = vk::VertexInputRate::eVertex;
        bindings[1].binding = 1;
        bindings[1].stride = sizeof(VertexSkin);
        bindings[1].inputRate = vk::VertexInputRate::eVertex;

        std::array<vk::VertexInputAttributeDescription, 5> attributes{
            vk::VertexInputAttributeDescription{ 0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position) },
            vk::VertexInputAttributeDescription{ 1, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, normal) },
            vk::VertexInputAttributeDescription{ 2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv0) },
            vk::VertexInputAttributeDescription{ 3, 1, vk::Format::eR16G16B16A16Uint, offsetof(VertexSkin, joints) },
            vk::VertexInputAttributeDescription{ 4, 1, vk::Format::eR32G32B32A32Sfloat, offsetof(VertexSkin, weights) }
        };

        vk::PipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.vertexAttributeDescriptionCount = 3;
        if (skinned)
        {
            vertexInput.vertexBindingDescriptionCount = 2;
            vertexInput.vertexAttributeDescriptionCount = 5;
        }
        vertexInput.pVertexBindingDescriptions = bindings.data();
        vertexInput.pVertexAttributeDescriptions = attributes.data();

        vk::PipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.topology = vk::PrimitiveTopology::eTriangleList;

        vk::PipelineViewportStateCreateInfo viewportState{};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        vk::PipelineRasterizationStateCreateInfo raster{};
        raster.polygonMode = vk::PolygonMode::eFill;
        if (wireframe)
        {
            raster.polygonMode = vk::PolygonMode::eLine;
        }
        raster.cullMode = vk::CullModeFlagBits::eNone;  // enable Back once winding is verified
        raster.frontFace = vk::FrontFace::eCounterClockwise;
        raster.lineWidth = 1.0f;

        vk::PipelineMultisampleStateCreateInfo multisample{};
        multisample.rasterizationSamples = renderer.targets.sampleCount;  // match the MSAA target

        vk::PipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = vk::CompareOp::eLessOrEqual;  // passes fragments at a depth pre-pass's value

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
        renderingInfo.setColorAttachmentFormats(OffscreenColorFormat);
        renderingInfo.depthAttachmentFormat = DepthFormat;

        vk::PushConstantRange pushConstant{};
        pushConstant.stageFlags = vk::ShaderStageFlagBits::eVertex;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(glm::mat4);  // viewProj

        // Sets 0-5 are always present; the IBL set (set 3) also carries the reflection probes
        // (bindings 3-5), so no separate probe set is bound. Sets 6/7 (RT TLAS + ReSTIR radiance)
        // are appended only when RT is supported (their layouts need the AS extension). The mesh
        // shader's RT bindings are compiled in but unused unless the ray-query path is taken
        // (gated by a UBO flag).
        std::vector<vk::DescriptorSetLayout> setLayouts{ renderer.descriptors.bindlessSetLayout,
                                                         renderer.descriptors.lightSetLayout,
                                                         renderer.descriptors.instanceSetLayout,
                                                         renderer.ibl.setLayout,
                                                         renderer.ssao.meshSetLayout,
                                                         renderer.ddgi.meshLayout };
        if (renderer.context.rtSupported && renderer.rt.meshLayout)
        {
            setLayouts.push_back(renderer.rt.meshLayout);      // set 6: TLAS
            setLayouts.push_back(renderer.restir.meshLayout);  // set 7: ReSTIR radiance
        }
        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setSetLayouts(setLayouts);
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult =
            checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (mesh)");
        if (!layoutResult)
        {
            renderer.context.device.destroyShaderModule(shaderModule);
            return Err(layoutResult.error());
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

        vk::ResultValue<vk::Pipeline> created = renderer.context.device.createGraphicsPipeline(nullptr, pipelineInfo);
        renderer.context.device.destroyShaderModule(shaderModule);
        if (created.result != vk::Result::eSuccess)
        {
            renderer.context.device.destroyPipelineLayout(*layoutResult);
            return Err(std::format("createGraphicsPipeline (mesh): {}", vk::to_string(created.result)));
        }

        Pipeline pipeline;
        pipeline.device = renderer.context.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;
        return std::make_shared<Pipeline>(std::move(pipeline));
    }

    auto requestMeshPipeline(Renderer& renderer, const Material& material, bool skinned, bool wireframe)
        -> Ref<Pipeline>
    {
        std::string key = material.shader;
        if (material.unlit)
        {
            key = key + "|unlit";
        }
        if (skinned)
        {
            key = key + "|skinned";
        }
        if (wireframe)
        {
            key = key + "|wireframe";
        }
        auto found = renderer.pipelines.cache.find(key);
        if (found != renderer.pipelines.cache.end())
        {
            return found->second;
        }
        auto built = newMeshPipeline(renderer, material.shader, material.unlit, skinned, wireframe);
        if (!built)
        {
            logError(built.error());
            return nullptr;
        }
        renderer.pipelines.cache.emplace(key, *built);
        // A non-zero count on a steady-state frame is the signature of a PSO-compile hitch.
        renderer.stats.pipelinesCreated = renderer.stats.pipelinesCreated + 1;
        return *built;
    }

    auto requestMeshPipeline(Renderer& renderer, const Material& material) -> Ref<Pipeline>
    {
        return requestMeshPipeline(renderer, material, false, false);
    }

    auto pipelineCount(const Renderer& renderer) -> u32
    {
        return static_cast<u32>(renderer.pipelines.cache.size());
    }

    auto bindlessTextureCount(const Renderer& renderer) -> u32
    {
        return renderer.descriptors.nextBindlessIndex;
    }

    auto bindlessFreeCount(const Renderer& renderer) -> u32
    {
        return static_cast<u32>(renderer.descriptors.bindlessFreeList->size());
    }

    auto newOverlayPipeline(Renderer& renderer, bool depthTest) -> Result<Ref<Pipeline>>
    {
        auto moduleResult = loadShaderModule(renderer.context.device, assetPath("shaders/gizmo_overlay.spv"));
        if (!moduleResult)
        {
            return Err(moduleResult.error());
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
        binding.stride = sizeof(OverlayVertex);
        binding.inputRate = vk::VertexInputRate::eVertex;
        std::array<vk::VertexInputAttributeDescription, 4> attributes{
            vk::VertexInputAttributeDescription{ 0, 0, vk::Format::eR32G32Sfloat, offsetof(OverlayVertex, position) },
            vk::VertexInputAttributeDescription{ 1, 0, vk::Format::eR32G32B32A32Sfloat,
                                                 offsetof(OverlayVertex, color) },
            vk::VertexInputAttributeDescription{ 2, 0, vk::Format::eR32G32B32A32Sfloat, offsetof(OverlayVertex, edge) },
            vk::VertexInputAttributeDescription{ 3, 0, vk::Format::eR32Sfloat, offsetof(OverlayVertex, depth) }
        };
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
        raster.cullMode = vk::CullModeFlagBits::eNone;
        raster.frontFace = vk::FrontFace::eCounterClockwise;
        raster.lineWidth = 1.0f;
        vk::PipelineMultisampleStateCreateInfo multisample{};
        multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
        // The depth-tested variant occludes against the scene depth without touching it
        // (eLessOrEqual matches the scene pass's compare; depth is Vulkan [0,1]).
        vk::PipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.depthTestEnable = VK_FALSE;
        if (depthTest)
        {
            depthStencil.depthTestEnable = VK_TRUE;
        }
        depthStencil.depthWriteEnable = VK_FALSE;
        depthStencil.depthCompareOp = vk::CompareOp::eLessOrEqual;
        vk::PipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
        blendAttachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
        blendAttachment.colorBlendOp = vk::BlendOp::eAdd;
        blendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
        blendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
        blendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
        blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.setAttachments(blendAttachment);
        std::array<vk::DynamicState, 2> dynamicStates{ vk::DynamicState::eViewport, vk::DynamicState::eScissor };
        vk::PipelineDynamicStateCreateInfo dynamic{};
        dynamic.setDynamicStates(dynamicStates);
        // Both variants run in the overlay pass, which binds a depth attachment; declare its
        // format so the PSO is render-pass compatible even when depth testing is off.
        vk::PipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.setColorAttachmentFormats(OffscreenColorFormat);
        renderingInfo.setDepthAttachmentFormat(DepthFormat);
        vk::PipelineLayoutCreateInfo layoutInfo{};
        auto layoutResult =
            checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (overlay)");
        if (!layoutResult)
        {
            renderer.context.device.destroyShaderModule(shaderModule);
            return Err(layoutResult.error());
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
        vk::ResultValue<vk::Pipeline> created = renderer.context.device.createGraphicsPipeline(nullptr, pipelineInfo);
        renderer.context.device.destroyShaderModule(shaderModule);
        if (created.result != vk::Result::eSuccess)
        {
            renderer.context.device.destroyPipelineLayout(*layoutResult);
            return Err(std::format("createGraphicsPipeline (overlay): {}", vk::to_string(created.result)));
        }
        Pipeline pipeline;
        pipeline.device = renderer.context.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;
        return std::make_shared<Pipeline>(std::move(pipeline));
    }

    auto newGridPipeline(Renderer& renderer) -> Result<Ref<Pipeline>>
    {
        auto moduleResult = loadShaderModule(renderer.context.device, assetPath("shaders/grid.spv"));
        if (!moduleResult)
        {
            return Err(moduleResult.error());
        }
        vk::ShaderModule shaderModule = *moduleResult;

        std::array<vk::PipelineShaderStageCreateInfo, 2> stages{};
        stages[0].stage = vk::ShaderStageFlagBits::eVertex;
        stages[0].module = shaderModule;
        stages[0].pName = "vertexMain";
        stages[1].stage = vk::ShaderStageFlagBits::eFragment;
        stages[1].module = shaderModule;
        stages[1].pName = "fragmentMain";

        vk::PipelineVertexInputStateCreateInfo vertexInput{};  // fullscreen triangle — no vertex buffer
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
        multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;  // 1x post-resolve, like the overlay
        // Test against the persisted 1x scene depth without writing it; the fragment emits SV_Depth
        // (the reprojected ground point) so geometry in front of the plane occludes the grid.
        vk::PipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_FALSE;
        depthStencil.depthCompareOp = vk::CompareOp::eLessOrEqual;
        vk::PipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
        blendAttachment.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
        blendAttachment.colorBlendOp = vk::BlendOp::eAdd;
        blendAttachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
        blendAttachment.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
        blendAttachment.alphaBlendOp = vk::BlendOp::eAdd;
        blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.setAttachments(blendAttachment);
        std::array<vk::DynamicState, 2> dynamicStates{ vk::DynamicState::eViewport, vk::DynamicState::eScissor };
        vk::PipelineDynamicStateCreateInfo dynamic{};
        dynamic.setDynamicStates(dynamicStates);
        vk::PipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.setColorAttachmentFormats(OffscreenColorFormat);
        renderingInfo.setDepthAttachmentFormat(DepthFormat);
        vk::PushConstantRange pushConstant{};
        pushConstant.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
        pushConstant.offset = 0;
        pushConstant.size = 2 * sizeof(glm::mat4);  // viewProj + invViewProj
        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult =
            checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (grid)");
        if (!layoutResult)
        {
            renderer.context.device.destroyShaderModule(shaderModule);
            return Err(layoutResult.error());
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
        vk::ResultValue<vk::Pipeline> created = renderer.context.device.createGraphicsPipeline(nullptr, pipelineInfo);
        renderer.context.device.destroyShaderModule(shaderModule);
        if (created.result != vk::Result::eSuccess)
        {
            renderer.context.device.destroyPipelineLayout(*layoutResult);
            return Err(std::format("createGraphicsPipeline (grid): {}", vk::to_string(created.result)));
        }
        Pipeline pipeline;
        pipeline.device = renderer.context.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;
        return std::make_shared<Pipeline>(std::move(pipeline));
    }
}
