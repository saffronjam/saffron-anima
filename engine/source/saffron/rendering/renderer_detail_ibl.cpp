module;

#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_NO_SMART_HANDLE
#include <vulkan/vulkan.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module Saffron.Rendering;

import Saffron.Core;
import Saffron.Window;
import Saffron.Geometry;
import :Detail;

namespace sa
{
    // Writes one probe slot's prefiltered + irradiance cube into the IBL set (binding 3 =
    // prefiltered array, binding 4 = irradiance array). Falls back to the global IBL cubes when
    // the slot has no captured probe, so every array element is always valid.
    void writeReflectionProbeSlot(Renderer& renderer, u32 slot)
    {
        ReflectionProbes& refl = renderer.reflection;
        const bool valid = refl.probes[slot].valid && refl.probes[slot].allocated;
        vk::ImageView pre = valid ? refl.probes[slot].prefilteredCube.view : renderer.ibl.prefilteredCube.view;
        vk::ImageView irr = valid ? refl.probes[slot].irradianceCube.view : renderer.ibl.irradianceCube.view;
        std::array<vk::DescriptorImageInfo, 2> infos{
            vk::DescriptorImageInfo{ refl.sampler, pre, vk::ImageLayout::eShaderReadOnlyOptimal },
            vk::DescriptorImageInfo{ refl.sampler, irr, vk::ImageLayout::eShaderReadOnlyOptimal }
        };
        std::array<vk::WriteDescriptorSet, 2> writes{};
        for (u32 b = 0; b < 2; b = b + 1)
        {
            writes[b].dstSet = refl.meshSet;
            writes[b].dstBinding = b + 3;
            writes[b].dstArrayElement = slot;
            writes[b].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            writes[b].setImageInfo(infos[b]);
        }
        renderer.context.device.updateDescriptorSets(writes, {});
    }

    // Seeds every probe array slot (IBL set bindings 3/4) with the global IBL cubes (valid
    // placeholders) and writes the metadata-SSBO binding (binding 5). Called once after the first
    // IBL bake so the mesh bind is valid even before any probe captures.
    void seedReflectionProbeSet(Renderer& renderer)
    {
        for (u32 slot = 0; slot < MaxReflectionProbes; slot = slot + 1)
        {
            writeReflectionProbeSlot(renderer, slot);
        }
        vk::DescriptorBufferInfo bi{};
        bi.buffer = renderer.reflection.metaBuffer->buffer;
        bi.offset = 0;
        bi.range = sizeof(ProbeMetaGpu) * MaxReflectionProbes;
        vk::WriteDescriptorSet w{};
        w.dstSet = renderer.reflection.meshSet;
        w.dstBinding = 5;
        w.descriptorType = vk::DescriptorType::eStorageBuffer;
        w.setBufferInfo(bi);
        renderer.context.device.updateDescriptorSets(w, {});
    }

    // Captures a local reflection probe: renders the scene into the probe's 6 cube faces
    // (pointShadowFaceMatrices), then convolves the captured cube into the probe's irradiance +
    // prefiltered cubes via the shared ibl_irradiance/ibl_prefilter shaders (same dispatch as
    // bakeEnvironment), and writes the result into the probe slot. Synchronous one-shot work +
    // waitIdle, run only on a dirty probe at the GPU-idle top of beginFrameGraph.
    auto captureReflectionProbe(Renderer& renderer, ReflectionProbe& probe, u32 slot) -> Result<void>
    {
        vk::Device device = renderer.context.device;
        const u32 preMips = IblPrefilterMips;

        // A single-sampled scene render is required (the cube faces are 1x). Skip when MSAA is
        // on rather than fight a PSO sample-count mismatch — capture is editor-time, MSAA off.
        if (renderer.targets.sampleCount != vk::SampleCountFlagBits::e1)
        {
            return Err(std::string{ "reflection probe capture needs MSAA off" });
        }

        if (!probe.allocated)
        {
            if (Result<void> r =
                    newColorCubeImage(renderer, IblEnvSize, IblColorFormat, probe.envCube, probe.faceViews);
                !r)
            {
                return Err(r.error());
            }
            auto depth = newDepthImage(renderer, IblEnvSize, IblEnvSize);
            if (!depth)
            {
                return Err(depth.error());
            }
            probe.envDepth = std::move(*depth);
            auto irr = newCubeImage(renderer, IblIrradianceSize, 1, IblColorFormat);
            if (!irr)
            {
                return Err(irr.error());
            }
            probe.irradianceCube = std::move(*irr);
            auto pre = newCubeImage(renderer, IblPrefilterSize, preMips, IblColorFormat);
            if (!pre)
            {
                return Err(pre.error());
            }
            probe.prefilteredCube = std::move(*pre);
            probe.allocated = true;
        }

        static_cast<void>(device.waitIdle());

        // The 6-face scene render reuses the cached scene draw list (built this frame) but pushes
        // each face's view-proj in place of the camera's. recordSceneDrawList binds the probe set, but the
        // probe being captured is not yet `valid`, so its slot still resolves to the global env —
        // no self-feedback (its envCube is the attachment, never sampled here).
        const std::array<glm::mat4, 6> faces =
            pointShadowFaceMatrices(probe.origin, glm::max(probe.influenceRadius * 4.0f, 50.0f));
        const glm::mat4 savedViewProj = renderer.frame.sceneDrawList.viewProj;

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = renderer.frame.frames[0].commandPool;
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(device.allocateCommandBuffers(cmdAlloc), "probe capture cmd");
        if (!cmds)
        {
            return Err(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];
        vk::CommandBufferBeginInfo begin{};
        begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(begin));

        auto barrier = [&](vk::Image image, vk::ImageLayout oldL, vk::ImageLayout newL, vk::PipelineStageFlags2 srcS,
                           vk::AccessFlags2 srcA, vk::PipelineStageFlags2 dstS, vk::AccessFlags2 dstA,
                           vk::ImageAspectFlags aspect, u32 baseMip, u32 mipCount, u32 baseLayer, u32 layerCount)
        {
            vk::ImageMemoryBarrier2 b{};
            b.srcStageMask = srcS;
            b.srcAccessMask = srcA;
            b.dstStageMask = dstS;
            b.dstAccessMask = dstA;
            b.oldLayout = oldL;
            b.newLayout = newL;
            b.image = image;
            b.subresourceRange = vk::ImageSubresourceRange{ aspect, baseMip, mipCount, baseLayer, layerCount };
            vk::DependencyInfo d{};
            d.setImageMemoryBarriers(b);
            cmd.pipelineBarrier2(d);
        };

        const vk::Extent2D faceExtent{ IblEnvSize, IblEnvSize };
        for (u32 f = 0; f < 6; f = f + 1)
        {
            barrier(probe.envCube.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
                    vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                    vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                    vk::ImageAspectFlagBits::eColor, 0, 1, f, 1);

            vk::RenderingAttachmentInfo colorAtt{};
            colorAtt.imageView = probe.faceViews[f];
            colorAtt.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
            colorAtt.loadOp = vk::AttachmentLoadOp::eClear;
            colorAtt.storeOp = vk::AttachmentStoreOp::eStore;
            colorAtt.clearValue = vk::ClearValue{ vk::ClearColorValue{ std::array<f32, 4>{ 0.0f, 0.0f, 0.0f, 1.0f } } };
            vk::RenderingAttachmentInfo depthAtt{};
            depthAtt.imageView = probe.envDepth.view;
            depthAtt.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
            depthAtt.loadOp = vk::AttachmentLoadOp::eClear;
            depthAtt.storeOp = vk::AttachmentStoreOp::eDontCare;
            depthAtt.clearValue = vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } };
            barrier(probe.envDepth.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthAttachmentOptimal,
                    vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                    vk::PipelineStageFlagBits2::eEarlyFragmentTests, vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
                    vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1);

            vk::RenderingInfo ri{};
            ri.renderArea = vk::Rect2D{ vk::Offset2D{ 0, 0 }, faceExtent };
            ri.layerCount = 1;
            ri.setColorAttachments(colorAtt);
            ri.pDepthAttachment = &depthAtt;
            cmd.beginRendering(ri);
            vk::Viewport vp{ 0.0f, 0.0f, static_cast<f32>(IblEnvSize), static_cast<f32>(IblEnvSize), 0.0f, 1.0f };
            cmd.setViewport(0, vp);
            cmd.setScissor(0, vk::Rect2D{ vk::Offset2D{ 0, 0 }, faceExtent });
            renderer.frame.sceneDrawList.viewProj = faces[f];
            recordSceneDrawList(renderer, cmd);
            cmd.endRendering();

            barrier(probe.envCube.image, vk::ImageLayout::eColorAttachmentOptimal,
                    vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                    vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderSampledRead, vk::ImageAspectFlagBits::eColor, 0, 1, f, 1);
        }
        renderer.frame.sceneDrawList.viewProj = savedViewProj;
        probe.envCube.layout = vk::ImageLayout::eShaderReadOnlyOptimal;

        // Convolve the captured cube into the probe's irradiance + prefiltered cubes. Transient
        // pool/layouts/sets/pipelines + 2D-array storage views, exactly like bakeEnvironment.
        std::array<vk::DescriptorPoolSize, 2> poolSizes{
            vk::DescriptorPoolSize{ vk::DescriptorType::eStorageImage, 16 },
            vk::DescriptorPoolSize{ vk::DescriptorType::eCombinedImageSampler, 16 }
        };
        vk::DescriptorPoolCreateInfo poolInfo{};
        poolInfo.maxSets = 32;
        poolInfo.setPoolSizes(poolSizes);
        auto poolR = checked(device.createDescriptorPool(poolInfo), "probe convolve pool");
        if (!poolR)
        {
            device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);
            return Err(poolR.error());
        }
        vk::DescriptorPool pool = *poolR;

        std::array<vk::DescriptorSetLayoutBinding, 2> bindB{};
        bindB[0].binding = 0;
        bindB[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        bindB[0].descriptorCount = 1;
        bindB[0].stageFlags = vk::ShaderStageFlagBits::eCompute;
        bindB[1].binding = 1;
        bindB[1].descriptorType = vk::DescriptorType::eStorageImage;
        bindB[1].descriptorCount = 1;
        bindB[1].stageFlags = vk::ShaderStageFlagBits::eCompute;
        vk::DescriptorSetLayoutCreateInfo layoutBInfo{};
        layoutBInfo.setBindings(bindB);
        auto layoutBR = checked(device.createDescriptorSetLayout(layoutBInfo), "probe layoutB");
        if (!layoutBR)
        {
            device.destroyDescriptorPool(pool);
            device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);
            return Err(layoutBR.error());
        }
        vk::DescriptorSetLayout layoutB = *layoutBR;
        auto cleanup = [&]()
        {
            device.destroyDescriptorSetLayout(layoutB);
            device.destroyDescriptorPool(pool);
        };

        auto irrP = newComputePipeline(renderer, "shaders/ibl_irradiance.spv", layoutB);
        if (!irrP)
        {
            cleanup();
            device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);
            return Err(irrP.error());
        }
        auto preP = newComputePipeline(renderer, "shaders/ibl_prefilter.spv", layoutB, static_cast<u32>(sizeof(f32)));
        if (!preP)
        {
            cleanup();
            device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);
            return Err(preP.error());
        }

        std::vector<vk::ImageView> transientViews;
        auto makeStorageView = [&](vk::Image image, u32 mip) -> vk::ImageView
        {
            vk::ImageViewCreateInfo v{};
            v.image = image;
            v.viewType = vk::ImageViewType::e2DArray;
            v.format = IblColorFormat;
            v.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, mip, 1, 0, 6 };
            vk::ImageView view = device.createImageView(v).value;
            transientViews.push_back(view);
            return view;
        };
        vk::ImageView irrStore = makeStorageView(probe.irradianceCube.image, 0);
        std::vector<vk::ImageView> preStore;
        for (u32 m = 0; m < preMips; m = m + 1)
        {
            preStore.push_back(makeStorageView(probe.prefilteredCube.image, m));
        }

        auto allocSet = [&](vk::DescriptorSetLayout layout) -> vk::DescriptorSet
        {
            vk::DescriptorSetAllocateInfo ai{};
            ai.descriptorPool = pool;
            ai.setSetLayouts(layout);
            return device.allocateDescriptorSets(ai).value[0];
        };
        auto writeSampler = [&](vk::DescriptorSet set, u32 binding, vk::ImageView view)
        {
            vk::DescriptorImageInfo ii{ renderer.ibl.sampler, view, vk::ImageLayout::eShaderReadOnlyOptimal };
            vk::WriteDescriptorSet w{};
            w.dstSet = set;
            w.dstBinding = binding;
            w.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            w.setImageInfo(ii);
            device.updateDescriptorSets(w, {});
        };
        auto writeStorage = [&](vk::DescriptorSet set, u32 binding, vk::ImageView view)
        {
            vk::DescriptorImageInfo ii{};
            ii.imageView = view;
            ii.imageLayout = vk::ImageLayout::eGeneral;
            vk::WriteDescriptorSet w{};
            w.dstSet = set;
            w.dstBinding = binding;
            w.descriptorType = vk::DescriptorType::eStorageImage;
            w.setImageInfo(ii);
            device.updateDescriptorSets(w, {});
        };
        vk::DescriptorSet irrSet = allocSet(layoutB);
        writeSampler(irrSet, 0, probe.envCube.view);
        writeStorage(irrSet, 1, irrStore);
        std::vector<vk::DescriptorSet> preSets;
        for (u32 m = 0; m < preMips; m = m + 1)
        {
            preSets.push_back(allocSet(layoutB));
            writeSampler(preSets[m], 0, probe.envCube.view);
            writeStorage(preSets[m], 1, preStore[m]);
        }

        const auto group = [](u32 n) -> u32 { return (n + 7) / 8; };

        barrier(probe.irradianceCube.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                vk::ImageAspectFlagBits::eColor, 0, 1, 0, 6);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, irrP.value()->pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, irrP.value()->layout, 0, irrSet, {});
        cmd.dispatch(group(IblIrradianceSize), group(IblIrradianceSize), 6);
        barrier(probe.irradianceCube.image, vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
                vk::ImageAspectFlagBits::eColor, 0, 1, 0, 6);

        barrier(probe.prefilteredCube.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                vk::ImageAspectFlagBits::eColor, 0, preMips, 0, 6);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, preP.value()->pipeline);
        for (u32 m = 0; m < preMips; m = m + 1)
        {
            u32 mipSize = IblPrefilterSize >> m;
            if (mipSize == 0)
            {
                mipSize = 1;
            }
            f32 roughness = preMips > 1 ? static_cast<f32>(m) / static_cast<f32>(preMips - 1) : 0.0f;
            cmd.pushConstants(preP.value()->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(f32), &roughness);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, preP.value()->layout, 0, preSets[m], {});
            cmd.dispatch(group(mipSize), group(mipSize), 6);
        }
        barrier(probe.prefilteredCube.image, vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
                vk::ImageAspectFlagBits::eColor, 0, preMips, 0, 6);

        static_cast<void>(cmd.end());
        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = cmd;
        vk::SubmitInfo2 submitInfo{};
        submitInfo.setCommandBufferInfos(cmdInfo);
        static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, nullptr));
        static_cast<void>(device.waitIdle());
        device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);

        probe.irradianceCube.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
        probe.prefilteredCube.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
        for (vk::ImageView v : transientViews)
        {
            device.destroyImageView(v);
        }
        cleanup();

        probe.valid = true;
        writeReflectionProbeSlot(renderer, slot);
        return {};
    }

    // Bakes the IBL environment: generates the procedural sky cube, convolves it into a
    // diffuse irradiance cube + a roughness-mipped prefiltered specular cube, integrates
    // the split-sum BRDF LUT, and writes the persistent set 3. Synchronous one-time work
    // (own command buffer + waitIdle), like uploadTexture/renderMeshThumbnail. Run once at
    // startup after initDescriptorResources.
    auto bakeEnvironment(Renderer& renderer, const SkygenParams& sky, bool firstBake) -> Result<void>
    {
        const u32 preMips = IblPrefilterMips;
        vk::Device device = renderer.context.device;

        if (firstBake)
        {
            auto env = newCubeImage(renderer, IblEnvSize, 1, IblColorFormat);
            if (!env)
            {
                return Err(env.error());
            }
            renderer.ibl.envCube = std::move(*env);
            auto irr = newCubeImage(renderer, IblIrradianceSize, 1, IblColorFormat);
            if (!irr)
            {
                return Err(irr.error());
            }
            renderer.ibl.irradianceCube = std::move(*irr);
            auto pre = newCubeImage(renderer, IblPrefilterSize, preMips, IblColorFormat);
            if (!pre)
            {
                return Err(pre.error());
            }
            renderer.ibl.prefilteredCube = std::move(*pre);
            auto lut = newColorImage(renderer, IblLutSize, IblLutSize, IblColorFormat, true);
            if (!lut)
            {
                return Err(lut.error());
            }
            renderer.ibl.brdfLut = std::move(*lut);
            renderer.ibl.prefilterMips = preMips;

            // Atmosphere LUTs (storage + sampled, persistent like the cubes above).
            auto trans = newColorImage(renderer, AtmosTransmittanceW, AtmosTransmittanceH, IblColorFormat, true);
            if (!trans)
            {
                return Err(trans.error());
            }
            renderer.ibl.transmittanceLut = std::move(*trans);
            auto ms = newColorImage(renderer, AtmosMultiScatterSize, AtmosMultiScatterSize, IblColorFormat, true);
            if (!ms)
            {
                return Err(ms.error());
            }
            renderer.ibl.multiScatterLut = std::move(*ms);
            auto sv = newColorImage(renderer, AtmosSkyViewW, AtmosSkyViewH, IblColorFormat, true);
            if (!sv)
            {
                return Err(sv.error());
            }
            renderer.ibl.skyViewLut = std::move(*sv);
        }
        else
        {
            // A re-bake overwrites the existing images in place (the Undefined->General barriers
            // below discard prior contents); drain any in-flight frame still sampling them first.
            static_cast<void>(device.waitIdle());
        }

        // A transient pool + layouts + sets used only for this bake (freed at the end).
        std::array<vk::DescriptorPoolSize, 2> poolSizes{
            vk::DescriptorPoolSize{ vk::DescriptorType::eStorageImage, 16 },
            vk::DescriptorPoolSize{ vk::DescriptorType::eCombinedImageSampler, 16 }
        };
        vk::DescriptorPoolCreateInfo poolInfo{};
        poolInfo.maxSets = 32;
        poolInfo.setPoolSizes(poolSizes);
        auto poolR = checked(device.createDescriptorPool(poolInfo), "ibl bake pool");
        if (!poolR)
        {
            return Err(poolR.error());
        }
        vk::DescriptorPool pool = *poolR;

        vk::DescriptorSetLayoutBinding bindA{};
        bindA.binding = 0;
        bindA.descriptorType = vk::DescriptorType::eStorageImage;
        bindA.descriptorCount = 1;
        bindA.stageFlags = vk::ShaderStageFlagBits::eCompute;
        vk::DescriptorSetLayoutCreateInfo layoutAInfo{};
        layoutAInfo.setBindings(bindA);
        auto layoutAR = checked(device.createDescriptorSetLayout(layoutAInfo), "ibl layoutA");
        if (!layoutAR)
        {
            device.destroyDescriptorPool(pool);
            return Err(layoutAR.error());
        }
        vk::DescriptorSetLayout layoutA = *layoutAR;

        std::array<vk::DescriptorSetLayoutBinding, 2> bindB{};
        bindB[0].binding = 0;
        bindB[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        bindB[0].descriptorCount = 1;
        bindB[0].stageFlags = vk::ShaderStageFlagBits::eCompute;
        bindB[1].binding = 1;
        bindB[1].descriptorType = vk::DescriptorType::eStorageImage;
        bindB[1].descriptorCount = 1;
        bindB[1].stageFlags = vk::ShaderStageFlagBits::eCompute;
        vk::DescriptorSetLayoutCreateInfo layoutBInfo{};
        layoutBInfo.setBindings(bindB);
        auto layoutBR = checked(device.createDescriptorSetLayout(layoutBInfo), "ibl layoutB");
        if (!layoutBR)
        {
            device.destroyDescriptorSetLayout(layoutA);
            device.destroyDescriptorPool(pool);
            return Err(layoutBR.error());
        }
        vk::DescriptorSetLayout layoutB = *layoutBR;

        auto cleanupLayouts = [&]()
        {
            device.destroyDescriptorSetLayout(layoutA);
            device.destroyDescriptorSetLayout(layoutB);
            device.destroyDescriptorPool(pool);
        };

        auto skygenP =
            newComputePipeline(renderer, "shaders/ibl_skygen.spv", layoutA, static_cast<u32>(2 * sizeof(glm::vec4)));
        if (!skygenP)
        {
            cleanupLayouts();
            return Err(skygenP.error());
        }
        auto equirectP =
            newComputePipeline(renderer, "shaders/ibl_equirect.spv", layoutB, static_cast<u32>(sizeof(glm::vec4)));
        if (!equirectP)
        {
            cleanupLayouts();
            return Err(equirectP.error());
        }
        auto irrP = newComputePipeline(renderer, "shaders/ibl_irradiance.spv", layoutB);
        if (!irrP)
        {
            cleanupLayouts();
            return Err(irrP.error());
        }
        auto preP = newComputePipeline(renderer, "shaders/ibl_prefilter.spv", layoutB, static_cast<u32>(sizeof(f32)));
        if (!preP)
        {
            cleanupLayouts();
            return Err(preP.error());
        }
        auto lutP = newComputePipeline(renderer, "shaders/ibl_brdf.spv", layoutA);
        if (!lutP)
        {
            cleanupLayouts();
            return Err(lutP.error());
        }

        // The atmosphere chain. transmittance writes one storage image (layoutA); multiscatter
        // and skyview read prior LUTs (layoutC = two samplers + one storage out), and skygen
        // reads the sky-view LUT into the cube (layoutB-shaped: sampler + storage cube). Built
        // only when this bake selects the Atmosphere source.
        const bool useAtmosphere = renderer.ibl.source == EnvSource::Atmosphere && sky.atmosphere.enabled;
        std::array<vk::DescriptorSetLayoutBinding, 3> bindC{};
        for (u32 i = 0; i < 2; i = i + 1)
        {
            bindC[i].binding = i;
            bindC[i].descriptorType = vk::DescriptorType::eCombinedImageSampler;
            bindC[i].descriptorCount = 1;
            bindC[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
        }
        bindC[2].binding = 2;
        bindC[2].descriptorType = vk::DescriptorType::eStorageImage;
        bindC[2].descriptorCount = 1;
        bindC[2].stageFlags = vk::ShaderStageFlagBits::eCompute;
        vk::DescriptorSetLayoutCreateInfo layoutCInfo{};
        layoutCInfo.setBindings(bindC);
        vk::DescriptorSetLayout layoutC = nullptr;
        if (useAtmosphere)
        {
            auto layoutCR = checked(device.createDescriptorSetLayout(layoutCInfo), "atmos layoutC");
            if (!layoutCR)
            {
                cleanupLayouts();
                return Err(layoutCR.error());
            }
            layoutC = *layoutCR;
        }
        auto cleanupAtmos = [&]()
        {
            if (layoutC)
            {
                device.destroyDescriptorSetLayout(layoutC);
            }
        };

        const u32 atmosPush = static_cast<u32>(5 * sizeof(glm::vec4));
        std::optional<Ref<Pipeline>> transP;
        std::optional<Ref<Pipeline>> multiP;
        std::optional<Ref<Pipeline>> skyViewP;
        std::optional<Ref<Pipeline>> atmosSkygenP;
        if (useAtmosphere)
        {
            auto t = newComputePipeline(renderer, "shaders/atmos_transmittance.spv", layoutA, atmosPush);
            if (!t)
            {
                cleanupAtmos();
                cleanupLayouts();
                return Err(t.error());
            }
            transP = *t;
            auto m = newComputePipeline(renderer, "shaders/atmos_multiscatter.spv", layoutC, atmosPush);
            if (!m)
            {
                cleanupAtmos();
                cleanupLayouts();
                return Err(m.error());
            }
            multiP = *m;
            auto s = newComputePipeline(renderer, "shaders/atmos_skyview.spv", layoutC, atmosPush);
            if (!s)
            {
                cleanupAtmos();
                cleanupLayouts();
                return Err(s.error());
            }
            skyViewP = *s;
            auto g = newComputePipeline(renderer, "shaders/atmos_skygen.spv", layoutB, atmosPush);
            if (!g)
            {
                cleanupAtmos();
                cleanupLayouts();
                return Err(g.error());
            }
            atmosSkygenP = *g;
        }

        // Transient 2D-array storage views (one per cube mip we write) + the per-set allocs.
        std::vector<vk::ImageView> transientViews;
        auto makeStorageView = [&](vk::Image image, u32 mip) -> vk::ImageView
        {
            vk::ImageViewCreateInfo v{};
            v.image = image;
            v.viewType = vk::ImageViewType::e2DArray;
            v.format = IblColorFormat;
            v.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, mip, 1, 0, 6 };
            vk::ImageView view = device.createImageView(v).value;
            transientViews.push_back(view);
            return view;
        };
        vk::ImageView envStore = makeStorageView(renderer.ibl.envCube.image, 0);
        vk::ImageView irrStore = makeStorageView(renderer.ibl.irradianceCube.image, 0);
        std::vector<vk::ImageView> preStore;
        for (u32 m = 0; m < preMips; m = m + 1)
        {
            preStore.push_back(makeStorageView(renderer.ibl.prefilteredCube.image, m));
        }

        auto allocSet = [&](vk::DescriptorSetLayout layout) -> vk::DescriptorSet
        {
            vk::DescriptorSetAllocateInfo ai{};
            ai.descriptorPool = pool;
            ai.setSetLayouts(layout);
            return device.allocateDescriptorSets(ai).value[0];
        };
        vk::DescriptorSet skygenSet = allocSet(layoutA);
        vk::DescriptorSet equirectSet = allocSet(layoutB);
        vk::DescriptorSet brdfSet = allocSet(layoutA);
        vk::DescriptorSet irrSet = allocSet(layoutB);
        vk::DescriptorSet transSet = nullptr;
        vk::DescriptorSet multiSet = nullptr;
        vk::DescriptorSet skyViewSet = nullptr;
        vk::DescriptorSet atmosSkygenSet = nullptr;
        if (useAtmosphere)
        {
            transSet = allocSet(layoutA);
            multiSet = allocSet(layoutC);
            skyViewSet = allocSet(layoutC);
            atmosSkygenSet = allocSet(layoutB);
        }
        std::vector<vk::DescriptorSet> preSets;
        for (u32 m = 0; m < preMips; m = m + 1)
        {
            preSets.push_back(allocSet(layoutB));
        }

        auto writeStorage = [&](vk::DescriptorSet set, u32 binding, vk::ImageView view)
        {
            vk::DescriptorImageInfo ii{};
            ii.imageView = view;
            ii.imageLayout = vk::ImageLayout::eGeneral;
            vk::WriteDescriptorSet w{};
            w.dstSet = set;
            w.dstBinding = binding;
            w.descriptorType = vk::DescriptorType::eStorageImage;
            w.setImageInfo(ii);
            device.updateDescriptorSets(w, {});
        };
        auto writeSampler = [&](vk::DescriptorSet set, u32 binding, vk::ImageView view)
        {
            vk::DescriptorImageInfo ii{ renderer.ibl.sampler, view, vk::ImageLayout::eShaderReadOnlyOptimal };
            vk::WriteDescriptorSet w{};
            w.dstSet = set;
            w.dstBinding = binding;
            w.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            w.setImageInfo(ii);
            device.updateDescriptorSets(w, {});
        };
        writeStorage(skygenSet, 0, envStore);
        writeStorage(brdfSet, 0, renderer.ibl.brdfLut.view);
        writeSampler(irrSet, 0, renderer.ibl.envCube.view);
        writeStorage(irrSet, 1, irrStore);
        for (u32 m = 0; m < preMips; m = m + 1)
        {
            writeSampler(preSets[m], 0, renderer.ibl.envCube.view);
            writeStorage(preSets[m], 1, preStore[m]);
        }
        if (useAtmosphere)
        {
            // transmittance: storage out only. multiscatter: transmittance sampler -> storage out.
            // skyview: transmittance + multiscatter samplers -> storage out. skygen: skyview sampler
            // (binding 0) -> envCube storage (binding 1). The LUT 2D .view doubles as storage view.
            writeStorage(transSet, 0, renderer.ibl.transmittanceLut.view);
            writeSampler(multiSet, 0, renderer.ibl.transmittanceLut.view);
            writeStorage(multiSet, 2, renderer.ibl.multiScatterLut.view);
            writeSampler(skyViewSet, 0, renderer.ibl.transmittanceLut.view);
            writeSampler(skyViewSet, 1, renderer.ibl.multiScatterLut.view);
            writeStorage(skyViewSet, 2, renderer.ibl.skyViewLut.view);
            writeSampler(atmosSkygenSet, 0, renderer.ibl.skyViewLut.view);
            writeStorage(atmosSkygenSet, 1, envStore);
        }

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = renderer.frame.frames[0].commandPool;
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(device.allocateCommandBuffers(cmdAlloc), "ibl bake cmd");
        if (!cmds)
        {
            for (vk::ImageView v : transientViews)
            {
                device.destroyImageView(v);
            }
            cleanupAtmos();
            cleanupLayouts();
            return Err(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];
        vk::CommandBufferBeginInfo begin{};
        begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(begin));

        auto barrier = [&](vk::Image image, vk::ImageLayout oldL, vk::ImageLayout newL, vk::PipelineStageFlags2 srcS,
                           vk::AccessFlags2 srcA, vk::PipelineStageFlags2 dstS, vk::AccessFlags2 dstA, u32 baseMip,
                           u32 mipCount, u32 layerCount)
        {
            vk::ImageMemoryBarrier2 b{};
            b.srcStageMask = srcS;
            b.srcAccessMask = srcA;
            b.dstStageMask = dstS;
            b.dstAccessMask = dstA;
            b.oldLayout = oldL;
            b.newLayout = newL;
            b.image = image;
            b.subresourceRange =
                vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, baseMip, mipCount, 0, layerCount };
            vk::DependencyInfo d{};
            d.setImageMemoryBarriers(b);
            cmd.pipelineBarrier2(d);
        };
        const auto group = [](u32 n) -> u32 { return (n + 7) / 8; };

        // Environment cube -> general, fill it (procedural skygen or an equirect panorama),
        // -> shader-read for the convolutions. A missing panorama degrades to Procedural.
        const bool useEquirect = renderer.ibl.source == EnvSource::Equirect && renderer.ibl.envPanorama != nullptr;
        if (renderer.ibl.source == EnvSource::Equirect && renderer.ibl.envPanorama == nullptr)
        {
            logWarn("ibl bake: Equirect source has no panorama; falling back to procedural sky");
        }
        // The atmosphere LUT chain runs before the cube fill: each LUT goes Undefined->General,
        // dispatch, General->ShaderReadOnly so the next stage samples it. The push packs the
        // AtmosphereParams + sun into 5 float4s (layout shared with the atmos_*.slang shaders).
        if (useAtmosphere)
        {
            const AtmosphereParams& a = sky.atmosphere;
            struct AtmosPush
            {
                glm::vec4 sunDir;    // xyz = dir to sun, w = sun intensity
                glm::vec4 rayleigh;  // xyz = rayleigh scattering, w = rayleigh scale height
                glm::vec4 ozone;     // xyz = ozone absorption, w = mie scattering
                glm::vec4 params0;   // planetRadius, atmosphereHeight, mieScaleHeight, mieAnisotropy
                glm::vec4 params1;   // sunDiskAngularRadius, sunDiskIntensity, cameraAltitude, 0
            } atmos{ glm::vec4(glm::normalize(sky.sunDir), sky.sunIntensity),
                     glm::vec4(a.rayleighScattering, a.rayleighScaleHeight),
                     glm::vec4(a.ozoneAbsorption, a.mieScattering),
                     glm::vec4(a.planetRadius, a.atmosphereHeight, a.mieScaleHeight, a.mieAnisotropy),
                     glm::vec4(a.sunDiskAngularRadius, a.sunDiskIntensity, 0.0f, 0.0f) };

            barrier(renderer.ibl.transmittanceLut.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite, 0, 1, 1);
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, transP.value()->pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, transP.value()->layout, 0, transSet, {});
            cmd.pushConstants(transP.value()->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(atmos), &atmos);
            cmd.dispatch(group(AtmosTransmittanceW), group(AtmosTransmittanceH), 1);
            barrier(renderer.ibl.transmittanceLut.image, vk::ImageLayout::eGeneral,
                    vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderStorageWrite, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderSampledRead, 0, 1, 1);

            barrier(renderer.ibl.multiScatterLut.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite, 0, 1, 1);
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, multiP.value()->pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, multiP.value()->layout, 0, multiSet, {});
            cmd.pushConstants(multiP.value()->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(atmos), &atmos);
            cmd.dispatch(group(AtmosMultiScatterSize), group(AtmosMultiScatterSize), 1);
            barrier(renderer.ibl.multiScatterLut.image, vk::ImageLayout::eGeneral,
                    vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderStorageWrite, vk::PipelineStageFlagBits2::eComputeShader,
                    vk::AccessFlagBits2::eShaderSampledRead, 0, 1, 1);

            barrier(renderer.ibl.skyViewLut.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                    vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite, 0, 1, 1);
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, skyViewP.value()->pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, skyViewP.value()->layout, 0, skyViewSet, {});
            cmd.pushConstants(skyViewP.value()->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(atmos), &atmos);
            cmd.dispatch(group(AtmosSkyViewW), group(AtmosSkyViewH), 1);
            barrier(renderer.ibl.skyViewLut.image, vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                    vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderSampledRead, 0, 1, 1);
        }

        barrier(renderer.ibl.envCube.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite, 0, 1, 6);
        if (useAtmosphere)
        {
            const AtmosphereParams& a = sky.atmosphere;
            struct AtmosPush
            {
                glm::vec4 sunDir;
                glm::vec4 rayleigh;
                glm::vec4 ozone;
                glm::vec4 params0;
                glm::vec4 params1;
            } atmos{ glm::vec4(glm::normalize(sky.sunDir), sky.sunIntensity),
                     glm::vec4(a.rayleighScattering, a.rayleighScaleHeight),
                     glm::vec4(a.ozoneAbsorption, a.mieScattering),
                     glm::vec4(a.planetRadius, a.atmosphereHeight, a.mieScaleHeight, a.mieAnisotropy),
                     glm::vec4(a.sunDiskAngularRadius, a.sunDiskIntensity, 0.0f, 0.0f) };
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, atmosSkygenP.value()->pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, atmosSkygenP.value()->layout, 0, atmosSkygenSet,
                                   {});
            cmd.pushConstants(atmosSkygenP.value()->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(atmos),
                              &atmos);
            cmd.dispatch(group(IblEnvSize), group(IblEnvSize), 6);
        }
        else if (useEquirect)
        {
            // The panorama wraps in longitude, so it reads through the eRepeat linearSampler
            // (ibl.sampler is eClampToEdge and would seam the meridian).
            vk::DescriptorImageInfo panoInfo{ renderer.descriptors.linearSampler, renderer.ibl.envPanorama->view,
                                              vk::ImageLayout::eShaderReadOnlyOptimal };
            vk::WriteDescriptorSet panoWrite{};
            panoWrite.dstSet = equirectSet;
            panoWrite.dstBinding = 0;
            panoWrite.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            panoWrite.setImageInfo(panoInfo);
            device.updateDescriptorSets(panoWrite, {});
            writeStorage(equirectSet, 1, envStore);
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, equirectP.value()->pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, equirectP.value()->layout, 0, equirectSet, {});
            // x = rotation, y = intensity. The IBL bakes the raw panorama (no rotation, unit
            // intensity); the visible-sky pass applies the scene's rotation/intensity itself.
            const glm::vec4 equirectPush{ 0.0f, 1.0f, 0.0f, 0.0f };
            cmd.pushConstants(equirectP.value()->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(equirectPush),
                              &equirectPush);
            cmd.dispatch(group(IblEnvSize), group(IblEnvSize), 6);
        }
        else
        {
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, skygenP.value()->pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, skygenP.value()->layout, 0, skygenSet, {});
            struct SkyPush
            {
                glm::vec4 sunDir;
                glm::vec4 sunColor;
            } skyPush{ glm::vec4(sky.sunDir, sky.sunIntensity), glm::vec4(sky.sunColor, 1.0f) };
            cmd.pushConstants(skygenP.value()->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(skyPush), &skyPush);
            cmd.dispatch(group(IblEnvSize), group(IblEnvSize), 6);
        }
        barrier(renderer.ibl.envCube.image, vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderSampledRead, 0, 1, 6);

        // Diffuse irradiance.
        barrier(renderer.ibl.irradianceCube.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite, 0, 1, 6);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, irrP.value()->pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, irrP.value()->layout, 0, irrSet, {});
        cmd.dispatch(group(IblIrradianceSize), group(IblIrradianceSize), 6);
        barrier(renderer.ibl.irradianceCube.image, vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead, 0, 1, 6);

        // Prefiltered specular: one dispatch per mip (roughness = mip / (mips-1)).
        barrier(renderer.ibl.prefilteredCube.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite, 0, preMips, 6);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, preP.value()->pipeline);
        for (u32 m = 0; m < preMips; m = m + 1)
        {
            u32 mipSize = IblPrefilterSize >> m;
            if (mipSize == 0)
            {
                mipSize = 1;
            }
            f32 roughness = preMips > 1 ? static_cast<f32>(m) / static_cast<f32>(preMips - 1) : 0.0f;
            cmd.pushConstants(preP.value()->layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(f32), &roughness);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, preP.value()->layout, 0, preSets[m], {});
            cmd.dispatch(group(mipSize), group(mipSize), 6);
        }
        barrier(renderer.ibl.prefilteredCube.image, vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead, 0, preMips, 6);

        // Split-sum BRDF LUT (2D, single layer).
        barrier(renderer.ibl.brdfLut.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite, 0, 1, 1);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, lutP.value()->pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, lutP.value()->layout, 0, brdfSet, {});
        cmd.dispatch(group(IblLutSize), group(IblLutSize), 1);
        barrier(renderer.ibl.brdfLut.image, vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
                vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead, 0, 1, 1);

        static_cast<void>(cmd.end());
        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = cmd;
        vk::SubmitInfo2 submitInfo{};
        submitInfo.setCommandBufferInfos(cmdInfo);
        static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, nullptr));
        static_cast<void>(device.waitIdle());
        device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);

        renderer.ibl.envCube.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
        renderer.ibl.irradianceCube.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
        renderer.ibl.prefilteredCube.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
        renderer.ibl.brdfLut.layout = vk::ImageLayout::eShaderReadOnlyOptimal;

        // Write the persistent descriptor sets only on the first bake — a re-bake reuses the
        // same images/views (only their contents change), so the sets stay valid.
        if (firstBake)
        {
            // Write the persistent set 3 the mesh fragment samples.
            std::array<vk::DescriptorImageInfo, 3> setImages{
                vk::DescriptorImageInfo{ renderer.ibl.sampler, renderer.ibl.irradianceCube.view,
                                         vk::ImageLayout::eShaderReadOnlyOptimal },
                vk::DescriptorImageInfo{ renderer.ibl.sampler, renderer.ibl.prefilteredCube.view,
                                         vk::ImageLayout::eShaderReadOnlyOptimal },
                vk::DescriptorImageInfo{ renderer.ibl.sampler, renderer.ibl.brdfLut.view,
                                         vk::ImageLayout::eShaderReadOnlyOptimal }
            };
            std::array<vk::WriteDescriptorSet, 3> setWrites{};
            for (u32 b = 0; b < 3; b = b + 1)
            {
                setWrites[b].dstSet = renderer.ibl.set;
                setWrites[b].dstBinding = b;
                setWrites[b].descriptorType = vk::DescriptorType::eCombinedImageSampler;
                setWrites[b].setImageInfo(setImages[b]);
            }
            device.updateDescriptorSets(setWrites, {});
            renderer.ibl.ready = true;

            // The visible-sky pass samples the same procedural environment cube (set 1, binding 0)
            // so the background matches the IBL lighting.
            vk::DescriptorImageInfo skyImage{ renderer.ibl.sampler, renderer.ibl.envCube.view,
                                              vk::ImageLayout::eShaderReadOnlyOptimal };
            vk::WriteDescriptorSet skyWrite{};
            skyWrite.dstSet = renderer.sky.set;
            skyWrite.dstBinding = 0;
            skyWrite.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            skyWrite.setImageInfo(skyImage);
            device.updateDescriptorSets(skyWrite, {});
            renderer.sky.ready = true;
        }

        if (useAtmosphere)
        {
            renderer.ibl.transmittanceLut.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
            renderer.ibl.multiScatterLut.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
            renderer.ibl.skyViewLut.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
        }

        for (vk::ImageView v : transientViews)
        {
            device.destroyImageView(v);
        }
        cleanupAtmos();
        cleanupLayouts();
        logInfo(std::format("ibl baked — env {}^2, irradiance {}^2, prefiltered {}^2 x{} mips, lut {}^2{}", IblEnvSize,
                            IblIrradianceSize, IblPrefilterSize, preMips, IblLutSize,
                            useAtmosphere ? " (atmosphere)" : ""));
        return {};
    }
}
