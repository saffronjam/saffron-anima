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

#include <algorithm>
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

namespace sa
{
    // The image read back after rendering: the resolve target under MSAA, else the color image.
    static auto resolveTarget(Image& resolve, Image& color, bool msaa) -> Image&
    {
        if (msaa)
        {
            return resolve;
        }
        return color;
    }

    // The highest MSAA count (≤8) valid for the thumbnail targets: the renderer's supported
    // counts (framebuffer limits ∩ depth format) further intersected with the swapchain color
    // format's own counts, which the thumbnail renders into. Thumbnails are tiny and rendered
    // once, so always taking the maximum is cheap and hides geometry aliasing.
    static auto thumbnailSampleCount(const Renderer& renderer) -> vk::SampleCountFlagBits
    {
        vk::SampleCountFlags supported = renderer.targets.supportedSampleCounts;
        auto colorFmt = renderer.context.physicalDevice.getImageFormatProperties(
            renderer.swapchain.format, vk::ImageType::e2D, vk::ImageTiling::eOptimal,
            vk::ImageUsageFlagBits::eColorAttachment, {});
        if (colorFmt.result == vk::Result::eSuccess)
        {
            supported = supported & colorFmt.value.sampleCounts;
        }
        for (vk::SampleCountFlagBits candidate :
             { vk::SampleCountFlagBits::e8, vk::SampleCountFlagBits::e4, vk::SampleCountFlagBits::e2 })
        {
            if (supported & candidate)
            {
                return candidate;
            }
        }
        return vk::SampleCountFlagBits::e1;
    }

    // The minimal mesh-thumbnail pipeline (vertex input + a 2x mat4 push constant, no
    // descriptor sets). Color format matches the offscreen thumbnail image.
    auto newThumbnailPipeline(Renderer& renderer) -> Result<Ref<Pipeline>>
    {
        auto moduleResult = loadShaderModule(renderer.context.device, assetPath("shaders/thumbnail.spv"));
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
        binding.stride = sizeof(Vertex);
        binding.inputRate = vk::VertexInputRate::eVertex;
        std::array<vk::VertexInputAttributeDescription, 3> attributes{
            vk::VertexInputAttributeDescription{ 0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position) },
            vk::VertexInputAttributeDescription{ 1, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, normal) },
            vk::VertexInputAttributeDescription{ 2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv0) }
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
        multisample.rasterizationSamples = thumbnailSampleCount(renderer);
        vk::PipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = vk::CompareOp::eLess;
        vk::PipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.setAttachments(blendAttachment);
        std::array<vk::DynamicState, 2> dynamicStates{ vk::DynamicState::eViewport, vk::DynamicState::eScissor };
        vk::PipelineDynamicStateCreateInfo dynamic{};
        dynamic.setDynamicStates(dynamicStates);

        vk::PipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.setColorAttachmentFormats(renderer.swapchain.format);
        renderingInfo.depthAttachmentFormat = DepthFormat;

        vk::PushConstantRange pushConstant{};
        pushConstant.stageFlags = vk::ShaderStageFlagBits::eVertex;
        pushConstant.offset = 0;
        pushConstant.size = 2 * sizeof(glm::mat4);  // mvp + normalMatrix

        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult =
            checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (thumbnail)");
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
            return Err(std::format("createGraphicsPipeline (thumbnail): {}", vk::to_string(created.result)));
        }
        Pipeline pipeline;
        pipeline.device = renderer.context.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;
        return std::make_shared<Pipeline>(std::move(pipeline));
    }

    auto renderMeshThumbnail(Renderer& renderer, const Ref<GpuMesh>& mesh, u32 size) -> Result<Ref<GpuTexture>>
    {
        if (!mesh)
        {
            return Err(std::string{ "renderMeshThumbnail: null mesh" });
        }
        if (!renderer.pipelines.thumbnail)
        {
            auto pipeline = newThumbnailPipeline(renderer);
            if (!pipeline)
            {
                return Err(pipeline.error());
            }
            renderer.pipelines.thumbnail = *pipeline;
        }

        // Render multisampled and resolve into a 1x image; the resolve is what gets read back.
        const vk::SampleCountFlagBits samples = thumbnailSampleCount(renderer);
        const bool msaa = samples != vk::SampleCountFlagBits::e1;
        auto colorImage = newColorImage(renderer, size, size, renderer.swapchain.format, false, samples);
        if (!colorImage)
        {
            return Err(colorImage.error());
        }
        Image color = std::move(*colorImage);
        Image resolve;
        if (msaa)
        {
            auto resolveImage = newColorImage(renderer, size, size, renderer.swapchain.format);
            if (!resolveImage)
            {
                return Err(resolveImage.error());
            }
            resolve = std::move(*resolveImage);
        }
        auto depthImage = newDepthImage(renderer, size, size, samples);
        if (!depthImage)
        {
            return Err(depthImage.error());
        }
        Image depth = std::move(*depthImage);

        // Frame the mesh: a 3/4 view at a distance that fits its bounding sphere.
        const glm::vec3 center = (mesh->boundsMin + mesh->boundsMax) * 0.5f;
        f32 radius = glm::length(mesh->boundsMax - mesh->boundsMin) * 0.5f;
        if (radius <= 0.0001f)
        {
            radius = 1.0f;
        }
        const f32 fovy = glm::radians(45.0f);
        const f32 distance = radius / glm::tan(fovy * 0.5f) * 1.3f;
        const glm::vec3 eye = center + glm::normalize(glm::vec3(1.0f, 0.7f, 1.0f)) * distance;
        const glm::mat4 view = glm::lookAt(eye, center, glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 proj =
            glm::perspective(fovy, 1.0f, glm::max(0.01f, distance - radius * 2.0f), distance + radius * 2.0f);
        proj[1][1] *= -1.0f;  // Vulkan clip; matches the viewport so the thumbnail is upright
        struct ThumbnailPush
        {
            glm::mat4 mvp;
            glm::mat4 normalMatrix;
        } push{ proj * view, glm::mat4(1.0f) };

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = oneOffCommandPool(renderer);
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(renderer.context.device.allocateCommandBuffers(cmdAlloc),
                            "renderMeshThumbnail: allocateCommandBuffers");
        if (!cmds)
        {
            return Err(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];
        vk::CommandBufferBeginInfo begin{};
        begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(begin));

        transitionImage(cmd, color.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
                        vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                        vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite);
        if (msaa)
        {
            transitionImage(cmd, resolve.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
                            vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                            vk::AccessFlagBits2::eColorAttachmentWrite);
        }
        transitionImage(cmd, depth.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthAttachmentOptimal,
                        vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                        vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                            vk::PipelineStageFlagBits2::eLateFragmentTests,
                        vk::AccessFlagBits2::eDepthStencilAttachmentWrite, vk::ImageAspectFlagBits::eDepth);

        vk::RenderingAttachmentInfo colorAttach{};
        colorAttach.imageView = color.view;
        colorAttach.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        colorAttach.loadOp = vk::AttachmentLoadOp::eClear;
        colorAttach.storeOp = vk::AttachmentStoreOp::eStore;
        if (msaa)
        {
            colorAttach.storeOp = vk::AttachmentStoreOp::eDontCare;
        }
        colorAttach.clearValue =
            vk::ClearValue{ vk::ClearColorValue{ std::array<f32, 4>{ 0.12f, 0.12f, 0.14f, 1.0f } } };
        if (msaa)
        {
            colorAttach.resolveMode = vk::ResolveModeFlagBits::eAverage;
            colorAttach.resolveImageView = resolve.view;
            colorAttach.resolveImageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        }
        vk::RenderingAttachmentInfo depthAttach{};
        depthAttach.imageView = depth.view;
        depthAttach.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
        depthAttach.loadOp = vk::AttachmentLoadOp::eClear;
        depthAttach.storeOp = vk::AttachmentStoreOp::eDontCare;
        depthAttach.clearValue = vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } };
        vk::RenderingInfo rendering{};
        rendering.renderArea = vk::Rect2D{ vk::Offset2D{ 0, 0 }, vk::Extent2D{ size, size } };
        rendering.layerCount = 1;
        rendering.setColorAttachments(colorAttach);
        rendering.setPDepthAttachment(&depthAttach);
        cmd.beginRendering(rendering);

        vk::Viewport viewport{ 0.0f, 0.0f, static_cast<f32>(size), static_cast<f32>(size), 0.0f, 1.0f };
        cmd.setViewport(0, viewport);
        cmd.setScissor(0, vk::Rect2D{ vk::Offset2D{ 0, 0 }, vk::Extent2D{ size, size } });
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, renderer.pipelines.thumbnail->pipeline);
        cmd.pushConstants(renderer.pipelines.thumbnail->layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(push),
                          &push);
        vk::DeviceSize offset = 0;
        cmd.bindVertexBuffers(0, mesh->vertexBuffer, offset);
        cmd.bindIndexBuffer(mesh->indexBuffer, 0, vk::IndexType::eUint32);
        for (const Submesh& submesh : mesh->submeshes)
        {
            cmd.drawIndexed(submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
        }
        cmd.endRendering();

        Image& result = resolveTarget(resolve, color, msaa);
        transitionImage(cmd, result.image, vk::ImageLayout::eColorAttachmentOptimal,
                        vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                        vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderSampledRead);
        static_cast<void>(cmd.end());

        auto submitted = submitAndWait(renderer, cmd);
        renderer.context.device.freeCommandBuffers(oneOffCommandPool(renderer), cmd);
        if (!submitted)
        {
            return Err(submitted.error());
        }

        // Take ownership of the resolved image as a sampled GpuTexture (no material set; the
        // editor reads thumbnails over the control plane). Null the Image's handles so it
        // does not free them on scope exit; the multisampled color image frees normally.
        GpuTexture texture;
        texture.device = renderer.context.device;
        texture.allocator = renderer.context.allocator;
        texture.image = result.image;
        texture.view = result.view;
        texture.alloc = result.alloc;
        texture.extent = result.extent;
        texture.format = result.format;
        result.image = nullptr;
        result.view = nullptr;
        result.alloc = nullptr;
        return std::make_shared<GpuTexture>(std::move(texture));
    }

    // Push constant for the material preview (matches preview.slang's PreviewPush). 112 bytes.
    struct PreviewPush
    {
        glm::mat4 viewProj;
        glm::vec4 baseColor;
        glm::uvec4 tex;  // x albedo, y metallic-roughness, z normal, w feature bits
        glm::vec4 pbr;   // x metallic, y roughness, z normalStrength
    };

    auto newPreviewPipeline(Renderer& renderer, const std::string& spvPath) -> Result<Ref<Pipeline>>
    {
        auto moduleResult = loadShaderModule(renderer.context.device, spvPath);
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
        binding.stride = sizeof(Vertex);
        binding.inputRate = vk::VertexInputRate::eVertex;
        std::array<vk::VertexInputAttributeDescription, 3> attributes{
            vk::VertexInputAttributeDescription{ 0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position) },
            vk::VertexInputAttributeDescription{ 1, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, normal) },
            vk::VertexInputAttributeDescription{ 2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv0) }
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
        multisample.rasterizationSamples = thumbnailSampleCount(renderer);
        vk::PipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = vk::CompareOp::eLess;
        vk::PipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        vk::PipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.setAttachments(blendAttachment);
        std::array<vk::DynamicState, 2> dynamicStates{ vk::DynamicState::eViewport, vk::DynamicState::eScissor };
        vk::PipelineDynamicStateCreateInfo dynamic{};
        dynamic.setDynamicStates(dynamicStates);

        vk::PipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.setColorAttachmentFormats(renderer.swapchain.format);
        renderingInfo.depthAttachmentFormat = DepthFormat;

        vk::PushConstantRange pushConstant{};
        pushConstant.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(PreviewPush);

        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setSetLayouts(renderer.descriptors.bindlessSetLayout);
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult =
            checked(renderer.context.device.createPipelineLayout(layoutInfo), "createPipelineLayout (preview)");
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
            return Err(std::format("createGraphicsPipeline (preview): {}", vk::to_string(created.result)));
        }
        Pipeline pipeline;
        pipeline.device = renderer.context.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;
        return std::make_shared<Pipeline>(std::move(pipeline));
    }

    // A unit UV sphere (origin-centered, radius 1) for material previews; normals == positions.
    auto makePreviewSphere(Renderer& renderer) -> Result<Ref<GpuMesh>>
    {
        const u32 rings = 32;
        const u32 sectors = 48;
        Mesh mesh;
        for (u32 r = 0; r <= rings; r = r + 1)
        {
            const f32 phi = glm::pi<f32>() * static_cast<f32>(r) / static_cast<f32>(rings);
            for (u32 s = 0; s <= sectors; s = s + 1)
            {
                const f32 theta = 2.0f * glm::pi<f32>() * static_cast<f32>(s) / static_cast<f32>(sectors);
                Vertex v;
                v.position = glm::vec3(glm::sin(phi) * glm::cos(theta), glm::cos(phi), glm::sin(phi) * glm::sin(theta));
                v.normal = v.position;
                v.uv0 = glm::vec2(static_cast<f32>(s) / static_cast<f32>(sectors),
                                  static_cast<f32>(r) / static_cast<f32>(rings));
                mesh.vertices.push_back(v);
            }
        }
        for (u32 r = 0; r < rings; r = r + 1)
        {
            for (u32 s = 0; s < sectors; s = s + 1)
            {
                const u32 a = r * (sectors + 1) + s;
                const u32 b = a + sectors + 1;
                mesh.indices.push_back(a);
                mesh.indices.push_back(b);
                mesh.indices.push_back(a + 1);
                mesh.indices.push_back(a + 1);
                mesh.indices.push_back(b);
                mesh.indices.push_back(b + 1);
            }
        }
        mesh.submeshes.push_back(Submesh{ 0, static_cast<u32>(mesh.indices.size()), 0, 0 });
        return uploadMesh(renderer, mesh);
    }

    auto renderMaterialPreview(Renderer& renderer, const SubmeshMaterial& material, u32 size,
                               const std::string& shaderSpv) -> Result<Ref<GpuTexture>>
    {
        // Empty shaderSpv => the default studio preview pipeline (cached). A codegen material passes
        // its compiled shader path and gets a fresh per-call pipeline (caching is a follow-on).
        Ref<Pipeline> pipeline;
        if (shaderSpv.empty())
        {
            if (!renderer.pipelines.preview)
            {
                auto created = newPreviewPipeline(renderer, assetPath("shaders/preview.spv"));
                if (!created)
                {
                    return Err(created.error());
                }
                renderer.pipelines.preview = *created;
            }
            pipeline = renderer.pipelines.preview;
        }
        else
        {
            auto created = newPreviewPipeline(renderer, shaderSpv);
            if (!created)
            {
                return Err(created.error());
            }
            pipeline = *created;
        }
        if (!renderer.previewSphere)
        {
            auto sphere = makePreviewSphere(renderer);
            if (!sphere)
            {
                return Err(sphere.error());
            }
            renderer.previewSphere = *sphere;
        }

        const vk::SampleCountFlagBits samples = thumbnailSampleCount(renderer);
        const bool msaa = samples != vk::SampleCountFlagBits::e1;
        auto colorImage = newColorImage(renderer, size, size, renderer.swapchain.format, false, samples);
        if (!colorImage)
        {
            return Err(colorImage.error());
        }
        Image color = std::move(*colorImage);
        Image resolve;
        if (msaa)
        {
            auto resolveImage = newColorImage(renderer, size, size, renderer.swapchain.format);
            if (!resolveImage)
            {
                return Err(resolveImage.error());
            }
            resolve = std::move(*resolveImage);
        }
        auto depthImage = newDepthImage(renderer, size, size, samples);
        if (!depthImage)
        {
            return Err(depthImage.error());
        }
        Image depth = std::move(*depthImage);

        // Frame the unit sphere from a fixed 3/4-front direction (matching preview.slang's view dir).
        const f32 fovy = glm::radians(45.0f);
        const f32 distance = 1.0f / glm::tan(fovy * 0.5f) * 1.35f;
        const glm::vec3 dir = glm::normalize(glm::vec3(0.3f, 0.4f, 1.0f));
        const glm::vec3 eye = dir * distance;
        const glm::mat4 view = glm::lookAt(eye, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 proj = glm::perspective(fovy, 1.0f, glm::max(0.01f, distance - 2.0f), distance + 2.0f);
        proj[1][1] *= -1.0f;

        u32 white = 0u;
        if (renderer.defaultWhiteTexture)
        {
            white = renderer.defaultWhiteTexture->bindlessIndex;
        }
        const auto idx = [&](const Ref<GpuTexture>& t) -> u32
        {
            if (t && t->image)
            {
                return t->bindlessIndex;
            }
            return white;
        };
        u32 features = 0u;
        if (material.normalTexture && material.normalTexture->image)
        {
            features |= 1u;  // FEATURE_NORMAL
        }
        PreviewPush push{};
        push.viewProj = proj * view;
        push.baseColor = material.baseColor;
        push.tex = glm::uvec4{ idx(material.albedoTexture), idx(material.metallicRoughnessTexture),
                               idx(material.normalTexture), features };
        push.pbr = glm::vec4{ material.metallic, material.roughness, material.normalStrength, 0.0f };

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = oneOffCommandPool(renderer);
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(renderer.context.device.allocateCommandBuffers(cmdAlloc),
                            "renderMaterialPreview: allocateCommandBuffers");
        if (!cmds)
        {
            return Err(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];
        vk::CommandBufferBeginInfo begin{};
        begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(begin));

        transitionImage(cmd, color.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
                        vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                        vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite);
        if (msaa)
        {
            transitionImage(cmd, resolve.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
                            vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                            vk::AccessFlagBits2::eColorAttachmentWrite);
        }
        transitionImage(cmd, depth.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthAttachmentOptimal,
                        vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                        vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                            vk::PipelineStageFlagBits2::eLateFragmentTests,
                        vk::AccessFlagBits2::eDepthStencilAttachmentWrite, vk::ImageAspectFlagBits::eDepth);

        vk::RenderingAttachmentInfo colorAttach{};
        colorAttach.imageView = color.view;
        colorAttach.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        colorAttach.loadOp = vk::AttachmentLoadOp::eClear;
        colorAttach.storeOp = vk::AttachmentStoreOp::eStore;
        if (msaa)
        {
            colorAttach.storeOp = vk::AttachmentStoreOp::eDontCare;
        }
        colorAttach.clearValue =
            vk::ClearValue{ vk::ClearColorValue{ std::array<f32, 4>{ 0.10f, 0.10f, 0.12f, 1.0f } } };
        if (msaa)
        {
            colorAttach.resolveMode = vk::ResolveModeFlagBits::eAverage;
            colorAttach.resolveImageView = resolve.view;
            colorAttach.resolveImageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        }
        vk::RenderingAttachmentInfo depthAttach{};
        depthAttach.imageView = depth.view;
        depthAttach.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
        depthAttach.loadOp = vk::AttachmentLoadOp::eClear;
        depthAttach.storeOp = vk::AttachmentStoreOp::eDontCare;
        depthAttach.clearValue = vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } };
        vk::RenderingInfo rendering{};
        rendering.renderArea = vk::Rect2D{ vk::Offset2D{ 0, 0 }, vk::Extent2D{ size, size } };
        rendering.layerCount = 1;
        rendering.setColorAttachments(colorAttach);
        rendering.setPDepthAttachment(&depthAttach);
        cmd.beginRendering(rendering);

        vk::Viewport viewport{ 0.0f, 0.0f, static_cast<f32>(size), static_cast<f32>(size), 0.0f, 1.0f };
        cmd.setViewport(0, viewport);
        cmd.setScissor(0, vk::Rect2D{ vk::Offset2D{ 0, 0 }, vk::Extent2D{ size, size } });
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline->layout, 0, renderer.descriptors.bindlessSet,
                               {});
        cmd.pushConstants(pipeline->layout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
                          sizeof(push), &push);
        vk::DeviceSize offset = 0;
        cmd.bindVertexBuffers(0, renderer.previewSphere->vertexBuffer, offset);
        cmd.bindIndexBuffer(renderer.previewSphere->indexBuffer, 0, vk::IndexType::eUint32);
        for (const Submesh& submesh : renderer.previewSphere->submeshes)
        {
            cmd.drawIndexed(submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
        }
        cmd.endRendering();

        Image& result = resolveTarget(resolve, color, msaa);
        transitionImage(cmd, result.image, vk::ImageLayout::eColorAttachmentOptimal,
                        vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                        vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderSampledRead);
        static_cast<void>(cmd.end());

        auto submitted = submitAndWait(renderer, cmd);
        renderer.context.device.freeCommandBuffers(oneOffCommandPool(renderer), cmd);
        if (!submitted)
        {
            return Err(submitted.error());
        }

        GpuTexture texture;
        texture.device = renderer.context.device;
        texture.allocator = renderer.context.allocator;
        texture.image = result.image;
        texture.view = result.view;
        texture.alloc = result.alloc;
        texture.extent = result.extent;
        texture.format = result.format;
        result.image = nullptr;
        result.view = nullptr;
        result.alloc = nullptr;
        return std::make_shared<GpuTexture>(std::move(texture));
    }

    // Renders a model's mesh under the studio preview lighting, each submesh shaded with its own
    // material from the table (indexed by Submesh.materialSlot), framed by the mesh bounds. The
    // textured counterpart to renderMeshThumbnail — the asset tile shows the model as it looks.
    auto renderModelThumbnail(Renderer& renderer, const Ref<GpuMesh>& mesh,
                              const std::vector<SubmeshMaterial>& submeshMaterials, u32 size) -> Result<Ref<GpuTexture>>
    {
        if (!mesh)
        {
            return Err(std::string{ "renderModelThumbnail: null mesh" });
        }
        if (!renderer.pipelines.preview)
        {
            auto created = newPreviewPipeline(renderer, assetPath("shaders/preview.spv"));
            if (!created)
            {
                return Err(created.error());
            }
            renderer.pipelines.preview = *created;
        }
        const Ref<Pipeline> pipeline = renderer.pipelines.preview;

        const vk::SampleCountFlagBits samples = thumbnailSampleCount(renderer);
        const bool msaa = samples != vk::SampleCountFlagBits::e1;
        auto colorImage = newColorImage(renderer, size, size, renderer.swapchain.format, false, samples);
        if (!colorImage)
        {
            return Err(colorImage.error());
        }
        Image color = std::move(*colorImage);
        Image resolve;
        if (msaa)
        {
            auto resolveImage = newColorImage(renderer, size, size, renderer.swapchain.format);
            if (!resolveImage)
            {
                return Err(resolveImage.error());
            }
            resolve = std::move(*resolveImage);
        }
        auto depthImage = newDepthImage(renderer, size, size, samples);
        if (!depthImage)
        {
            return Err(depthImage.error());
        }
        Image depth = std::move(*depthImage);

        // Frame the mesh from the 3/4 direction preview.slang assumes for its fixed view vector, so the
        // specular reads correctly; distance fits the bounding sphere.
        const glm::vec3 center = (mesh->boundsMin + mesh->boundsMax) * 0.5f;
        f32 radius = glm::length(mesh->boundsMax - mesh->boundsMin) * 0.5f;
        if (radius <= 0.0001f)
        {
            radius = 1.0f;
        }
        const f32 fovy = glm::radians(45.0f);
        const f32 distance = radius / glm::tan(fovy * 0.5f) * 1.3f;
        const glm::vec3 eye = center + glm::normalize(glm::vec3(0.3f, 0.4f, 1.0f)) * distance;
        const glm::mat4 view = glm::lookAt(eye, center, glm::vec3(0.0f, 1.0f, 0.0f));
        glm::mat4 proj =
            glm::perspective(fovy, 1.0f, glm::max(0.01f, distance - radius * 2.0f), distance + radius * 2.0f);
        proj[1][1] *= -1.0f;
        const glm::mat4 viewProj = proj * view;

        u32 white = 0u;
        if (renderer.defaultWhiteTexture)
        {
            white = renderer.defaultWhiteTexture->bindlessIndex;
        }
        const auto idx = [&](const Ref<GpuTexture>& t) -> u32
        {
            if (t && t->image)
            {
                return t->bindlessIndex;
            }
            return white;
        };

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = oneOffCommandPool(renderer);
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(renderer.context.device.allocateCommandBuffers(cmdAlloc),
                            "renderModelThumbnail: allocateCommandBuffers");
        if (!cmds)
        {
            return Err(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];
        vk::CommandBufferBeginInfo begin{};
        begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(begin));

        transitionImage(cmd, color.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
                        vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                        vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite);
        if (msaa)
        {
            transitionImage(cmd, resolve.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
                            vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                            vk::AccessFlagBits2::eColorAttachmentWrite);
        }
        transitionImage(cmd, depth.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthAttachmentOptimal,
                        vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                        vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                            vk::PipelineStageFlagBits2::eLateFragmentTests,
                        vk::AccessFlagBits2::eDepthStencilAttachmentWrite, vk::ImageAspectFlagBits::eDepth);

        vk::RenderingAttachmentInfo colorAttach{};
        colorAttach.imageView = color.view;
        colorAttach.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        colorAttach.loadOp = vk::AttachmentLoadOp::eClear;
        colorAttach.storeOp = vk::AttachmentStoreOp::eStore;
        if (msaa)
        {
            colorAttach.storeOp = vk::AttachmentStoreOp::eDontCare;
        }
        colorAttach.clearValue =
            vk::ClearValue{ vk::ClearColorValue{ std::array<f32, 4>{ 0.10f, 0.10f, 0.12f, 1.0f } } };
        if (msaa)
        {
            colorAttach.resolveMode = vk::ResolveModeFlagBits::eAverage;
            colorAttach.resolveImageView = resolve.view;
            colorAttach.resolveImageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        }
        vk::RenderingAttachmentInfo depthAttach{};
        depthAttach.imageView = depth.view;
        depthAttach.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
        depthAttach.loadOp = vk::AttachmentLoadOp::eClear;
        depthAttach.storeOp = vk::AttachmentStoreOp::eDontCare;
        depthAttach.clearValue = vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } };
        vk::RenderingInfo rendering{};
        rendering.renderArea = vk::Rect2D{ vk::Offset2D{ 0, 0 }, vk::Extent2D{ size, size } };
        rendering.layerCount = 1;
        rendering.setColorAttachments(colorAttach);
        rendering.setPDepthAttachment(&depthAttach);
        cmd.beginRendering(rendering);

        vk::Viewport viewport{ 0.0f, 0.0f, static_cast<f32>(size), static_cast<f32>(size), 0.0f, 1.0f };
        cmd.setViewport(0, viewport);
        cmd.setScissor(0, vk::Rect2D{ vk::Offset2D{ 0, 0 }, vk::Extent2D{ size, size } });
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline->layout, 0, renderer.descriptors.bindlessSet,
                               {});
        vk::DeviceSize offset = 0;
        cmd.bindVertexBuffers(0, mesh->vertexBuffer, offset);
        cmd.bindIndexBuffer(mesh->indexBuffer, 0, vk::IndexType::eUint32);
        const SubmeshMaterial fallback{};
        for (const Submesh& submesh : mesh->submeshes)
        {
            const SubmeshMaterial* mPtr = &fallback;
            if (!submeshMaterials.empty())
            {
                mPtr = &submeshMaterials[std::min<std::size_t>(submesh.materialSlot, submeshMaterials.size() - 1)];
            }
            const SubmeshMaterial& m = *mPtr;
            u32 features = 0u;
            if (m.normalTexture && m.normalTexture->image)
            {
                features |= 1u;  // FEATURE_NORMAL
            }
            PreviewPush push{};
            push.viewProj = viewProj;
            push.baseColor = m.baseColor;
            push.tex =
                glm::uvec4{ idx(m.albedoTexture), idx(m.metallicRoughnessTexture), idx(m.normalTexture), features };
            push.pbr = glm::vec4{ m.metallic, m.roughness, m.normalStrength, 0.0f };
            cmd.pushConstants(pipeline->layout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
                              0, sizeof(push), &push);
            cmd.drawIndexed(submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
        }
        cmd.endRendering();

        Image& result = resolveTarget(resolve, color, msaa);
        transitionImage(cmd, result.image, vk::ImageLayout::eColorAttachmentOptimal,
                        vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                        vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderSampledRead);
        static_cast<void>(cmd.end());

        auto submitted = submitAndWait(renderer, cmd);
        renderer.context.device.freeCommandBuffers(oneOffCommandPool(renderer), cmd);
        if (!submitted)
        {
            return Err(submitted.error());
        }

        GpuTexture texture;
        texture.device = renderer.context.device;
        texture.allocator = renderer.context.allocator;
        texture.image = result.image;
        texture.view = result.view;
        texture.alloc = result.alloc;
        texture.extent = result.extent;
        texture.format = result.format;
        result.image = nullptr;
        result.view = nullptr;
        result.alloc = nullptr;
        return std::make_shared<GpuTexture>(std::move(texture));
    }

    // A transient 1x image for the thumbnail downscale chain: TRANSFER_DST | TRANSFER_SRC only
    // (blit target then blit source; never sampled or stored). No view — blit/copy take images.
    static auto newBlitImage(Renderer& renderer, u32 width, u32 height, vk::Format format) -> Result<Image>
    {
        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = static_cast<VkFormat>(format);
        imageInfo.extent = VkExtent3D{ width, height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

        VkImage rawImage = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        if (vmaCreateImage(renderer.context.allocator, &imageInfo, &allocInfo, &rawImage, &allocation, nullptr) !=
            VK_SUCCESS)
        {
            return Err(std::string{ "encodeTextureThumbnailPng: vmaCreateImage (blit target) failed" });
        }
        Image result;
        result.device = renderer.context.device;
        result.allocator = renderer.context.allocator;
        result.image = vk::Image{ rawImage };
        result.alloc = allocation;
        result.extent = vk::Extent2D{ width, height };
        result.format = format;
        result.layout = vk::ImageLayout::eUndefined;
        return result;
    }

    // Whether `format` supports linear-filtered blits (src + dst) in optimal tiling. RGBA8/RGBA16F
    // do on every target we run; a format that does not falls back to a native-extent readback.
    static auto formatSupportsLinearBlit(Renderer& renderer, vk::Format format) -> bool
    {
        const vk::FormatProperties props = renderer.context.physicalDevice.getFormatProperties(format);
        const vk::FormatFeatureFlags needed = vk::FormatFeatureFlagBits::eBlitSrc |
                                              vk::FormatFeatureFlagBits::eBlitDst |
                                              vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
        return (props.optimalTilingFeatures & needed) == needed;
    }

    auto encodeTextureThumbnailPng(Renderer& renderer, const Ref<GpuTexture>& texture, u32 size, PngTransfer transfer)
        -> Result<ThumbnailPng>
    {
        if (!texture)
        {
            return Err(std::string{ "encodeTextureThumbnailPng: null texture" });
        }
        const u32 srcW = texture->extent.width;
        const u32 srcH = texture->extent.height;
        const u32 maxDim = std::max(srcW, srcH);

        // Fit within size×size preserving aspect (max dimension == size), so the editor's
        // object-contain display is unchanged. Downscale only shrinks; never upscale a small source.
        u32 dstW = srcW;
        u32 dstH = srcH;
        const bool downscale = maxDim > size && size > 0 && formatSupportsLinearBlit(renderer, texture->format);
        if (downscale)
        {
            dstW = std::max<u32>(1, (srcW * size + maxDim / 2) / maxDim);
            dstH = std::max<u32>(1, (srcH * size + maxDim / 2) / maxDim);
        }

        // A chained 2× reduction (mip-style) down to the target — a single 4096→128 linear blit
        // would undersample (a 2×2 tap per texel). Allocate one transient per halving step.
        std::vector<Image> transients;
        if (downscale)
        {
            std::vector<vk::Extent2D> steps;
            vk::Extent2D cur = texture->extent;
            while (cur.width > dstW * 2 || cur.height > dstH * 2)
            {
                cur.width = std::max(dstW, cur.width / 2);
                cur.height = std::max(dstH, cur.height / 2);
                steps.push_back(cur);
            }
            if (steps.empty() || steps.back().width != dstW || steps.back().height != dstH)
            {
                steps.push_back(vk::Extent2D{ dstW, dstH });
            }
            for (const vk::Extent2D& e : steps)
            {
                auto img = newBlitImage(renderer, e.width, e.height, texture->format);
                if (!img)
                {
                    return Err(img.error());
                }
                transients.push_back(std::move(*img));
            }
        }

        const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(dstW) * dstH * formatPixelBytes(texture->format);

        // No pre-readback wait: the capture/blit command buffer's own barriers transition the source
        // out of eShaderReadOnlyOptimal with srcStage=eFragmentShader, and a pipeline barrier's first
        // scope covers every command submitted earlier on this queue — so the readback is already
        // ordered after any in-flight frame's sampling of the texture. The post-submit fence below
        // (submitAndWait) is the only wait needed, and the texture's producer already fenced its upload.

        VkBuffer rawBuffer = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        VmaAllocationInfo info{};
        if (auto created = newHostCaptureBuffer(renderer, bytes, rawBuffer, alloc, info); !created)
        {
            return Err(created.error());
        }

        vk::CommandBufferAllocateInfo allocInfo{};
        allocInfo.commandPool = oneOffCommandPool(renderer);
        allocInfo.level = vk::CommandBufferLevel::ePrimary;
        allocInfo.commandBufferCount = 1;
        auto cmds = checked(renderer.context.device.allocateCommandBuffers(allocInfo),
                            "encodeTextureThumbnailPng: allocateCommandBuffers");
        if (!cmds)
        {
            vmaDestroyBuffer(renderer.context.allocator, rawBuffer, alloc);
            return Err(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];
        vk::CommandBufferBeginInfo begin{};
        begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(begin));

        if (downscale)
        {
            // Bindless + thumbnail textures live in eShaderReadOnlyOptimal; blit-read the source and
            // restore that layout so the bindless array stays valid. Each transient is blitted into,
            // then flipped to TransferSrc to feed the next step; the last stays TransferSrc for readback.
            transitionImage(cmd, texture->image, vk::ImageLayout::eShaderReadOnlyOptimal,
                            vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eFragmentShader,
                            vk::AccessFlagBits2::eShaderSampledRead, vk::PipelineStageFlagBits2::eBlit,
                            vk::AccessFlagBits2::eTransferRead);
            vk::Image srcImage = texture->image;
            vk::Extent2D srcExtent = texture->extent;
            for (const Image& dst : transients)
            {
                transitionImage(cmd, dst.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                                vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                                vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferWrite);
                vk::ImageBlit region{};
                region.srcSubresource = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
                region.srcOffsets[0] = vk::Offset3D{ 0, 0, 0 };
                region.srcOffsets[1] =
                    vk::Offset3D{ static_cast<i32>(srcExtent.width), static_cast<i32>(srcExtent.height), 1 };
                region.dstSubresource = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
                region.dstOffsets[0] = vk::Offset3D{ 0, 0, 0 };
                region.dstOffsets[1] =
                    vk::Offset3D{ static_cast<i32>(dst.extent.width), static_cast<i32>(dst.extent.height), 1 };
                cmd.blitImage(srcImage, vk::ImageLayout::eTransferSrcOptimal, dst.image,
                              vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eLinear);
                transitionImage(cmd, dst.image, vk::ImageLayout::eTransferDstOptimal,
                                vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eBlit,
                                vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eBlit,
                                vk::AccessFlagBits2::eTransferRead);
                srcImage = dst.image;
                srcExtent = dst.extent;
            }
            transitionImage(cmd, texture->image, vk::ImageLayout::eTransferSrcOptimal,
                            vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eBlit,
                            vk::AccessFlagBits2::eTransferRead, vk::PipelineStageFlagBits2::eFragmentShader,
                            vk::AccessFlagBits2::eShaderSampledRead);
            captureImageToBuffer(cmd, srcImage, vk::Extent2D{ dstW, dstH }, vk::ImageLayout::eTransferSrcOptimal,
                                 vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferRead,
                                 vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eBlit,
                                 vk::AccessFlagBits2::eTransferRead, vk::Buffer{ rawBuffer });
        }
        else
        {
            captureImageToBuffer(cmd, texture->image, texture->extent, vk::ImageLayout::eShaderReadOnlyOptimal,
                                 vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
                                 vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eFragmentShader,
                                 vk::AccessFlagBits2::eShaderSampledRead, vk::Buffer{ rawBuffer });
        }
        static_cast<void>(cmd.end());

        auto submitted = submitAndWait(renderer, cmd);
        renderer.context.device.freeCommandBuffers(oneOffCommandPool(renderer), cmd);
        if (!submitted)
        {
            vmaDestroyBuffer(renderer.context.allocator, rawBuffer, alloc);
            return Err(submitted.error());
        }
        vmaInvalidateAllocation(renderer.context.allocator, alloc, 0, VK_WHOLE_SIZE);

        auto png = encodeBufferToPng(static_cast<const unsigned char*>(info.pMappedData), dstW, dstH, texture->format,
                                     transfer);
        vmaDestroyBuffer(renderer.context.allocator, rawBuffer, alloc);
        if (!png)
        {
            return Err(png.error());
        }
        return ThumbnailPng{ std::move(*png), dstW, dstH };
    }

    auto encodeAssetThumbnailPng(Renderer& renderer, const Ref<GpuMesh>& mesh, u32 size) -> Result<ThumbnailPng>
    {
        // Render the framed mesh to a size×size texture, then read that texture back.
        auto tex = renderMeshThumbnail(renderer, mesh, size);
        if (!tex)
        {
            return Err(tex.error());
        }
        return encodeTextureThumbnailPng(renderer, *tex, size);
    }

    auto encodeModelThumbnailPng(Renderer& renderer, const Ref<GpuMesh>& mesh,
                                 const std::vector<SubmeshMaterial>& submeshMaterials, u32 size) -> Result<ThumbnailPng>
    {
        // Render the framed, textured model to a size×size texture, then read that texture back.
        auto tex = renderModelThumbnail(renderer, mesh, submeshMaterials, size);
        if (!tex)
        {
            return Err(tex.error());
        }
        return encodeTextureThumbnailPng(renderer, *tex, size);
    }

    void bindThumbnailWorkerThread(Renderer& renderer)
    {
        tlsThumbnailPool = renderer.workerCommandPool;
    }

    auto prewarmThumbnailResources(Renderer& renderer) -> Result<void>
    {
        // renderMeshThumbnail / renderMaterialPreview lazily create these the first time they run.
        // The worker must never be the one to write them (that would race a main-thread read), so the
        // main thread creates them up front before the worker starts.
        if (!renderer.pipelines.thumbnail)
        {
            auto pipeline = newThumbnailPipeline(renderer);
            if (!pipeline)
            {
                return Err(pipeline.error());
            }
            renderer.pipelines.thumbnail = *pipeline;
        }
        if (!renderer.pipelines.preview)
        {
            auto pipeline = newPreviewPipeline(renderer, assetPath("shaders/preview.spv"));
            if (!pipeline)
            {
                return Err(pipeline.error());
            }
            renderer.pipelines.preview = *pipeline;
        }
        if (!renderer.previewSphere)
        {
            auto sphere = makePreviewSphere(renderer);
            if (!sphere)
            {
                return Err(sphere.error());
            }
            renderer.previewSphere = *sphere;
        }
        return {};
    }
}
