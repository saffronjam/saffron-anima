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
    auto uploadMesh(Renderer& renderer, const Mesh& mesh) -> Result<Ref<GpuMesh>>
    {
        if (mesh.vertices.empty() || mesh.indices.empty())
        {
            return Err(std::string{ "uploadMesh: empty mesh" });
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
        if (vmaCreateBuffer(renderer.context.allocator, &stagingInfo, &stagingAlloc, &staging, &stagingAllocation, &stagingMapped) != VK_SUCCESS)
        {
            return Err(std::string{ "uploadMesh: staging vmaCreateBuffer failed" });
        }
        std::memcpy(stagingMapped.pMappedData, mesh.vertices.data(), vertexBytes);
        std::memcpy(static_cast<char*>(stagingMapped.pMappedData) + vertexBytes, mesh.indices.data(), indexBytes);
        vmaFlushAllocation(renderer.context.allocator, stagingAllocation, 0, VK_WHOLE_SIZE);

        auto makeDeviceBuffer = [&](vk::DeviceSize size, VkBufferUsageFlags usage, VkBuffer& outBuffer, VmaAllocation& outAlloc) -> bool
        {
            VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size = size;
            info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo alloc{};
            alloc.usage = VMA_MEMORY_USAGE_AUTO;
            return vmaCreateBuffer(renderer.context.allocator, &info, &alloc, &outBuffer, &outAlloc, nullptr) == VK_SUCCESS;
        };

        GpuMesh gpu;
        gpu.allocator = renderer.context.allocator;
        gpu.indexCount = static_cast<u32>(mesh.indices.size());
        gpu.submeshes = mesh.submeshes;
        gpu.boundsMin = glm::vec3(std::numeric_limits<f32>::max());
        gpu.boundsMax = glm::vec3(std::numeric_limits<f32>::lowest());
        for (const Vertex& vertex : mesh.vertices)
        {
            gpu.boundsMin = glm::min(gpu.boundsMin, vertex.position);
            gpu.boundsMax = glm::max(gpu.boundsMax, vertex.position);
        }

        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VmaAllocation vertexAlloc = nullptr;
        VmaAllocation indexAlloc = nullptr;
        if (!makeDeviceBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertexBuffer, vertexAlloc) ||
            !makeDeviceBuffer(indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexBuffer, indexAlloc))
        {
            if (vertexBuffer != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(renderer.context.allocator, vertexBuffer, vertexAlloc);
            }
            vmaDestroyBuffer(renderer.context.allocator, staging, stagingAllocation);
            return Err(std::string{ "uploadMesh: device vmaCreateBuffer failed" });
        }
        gpu.vertexBuffer = vk::Buffer{ vertexBuffer };
        gpu.vertexAlloc = vertexAlloc;
        gpu.indexBuffer = vk::Buffer{ indexBuffer };
        gpu.indexAlloc = indexAlloc;

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = renderer.frame.frames[0].commandPool;
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(renderer.context.device.allocateCommandBuffers(cmdAlloc), "uploadMesh: allocateCommandBuffers");
        if (!cmds)
        {
            vmaDestroyBuffer(renderer.context.allocator, staging, stagingAllocation);
            return Err(cmds.error());
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
        static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, nullptr));
        static_cast<void>(renderer.context.device.waitIdle());
        renderer.context.device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);
        vmaDestroyBuffer(renderer.context.allocator, staging, stagingAllocation);

        logInfo(std::format("uploaded mesh: {} vertices, {} indices, {} submeshes",
                            mesh.vertices.size(), mesh.indices.size(), mesh.submeshes.size()));
        return std::make_shared<GpuMesh>(std::move(gpu));
    }

    // Ensures the current frame's instance buffer holds at least `count` elements,
    // growing to the next power of two (never shrinking) and rewriting its set.
    auto ensureInstanceCapacity(Renderer& renderer, u32 frame, u32 count) -> Result<void>
    {
        if (renderer.instancing.buffers[frame] && renderer.instancing.capacity[frame] >= count)
        {
            return {};
        }
        u32 capacity = renderer.instancing.capacity[frame];
        if (capacity == 0)
        {
            capacity = 256;
        }
        while (capacity < count)
        {
            capacity = capacity * 2;
        }

        // beginFrame already waited on this frame's fence, so the old buffer (used by
        // the same frame) is no longer in flight — safe to drop and recreate.
        Result<Ref<Buffer>> buffer =
            makeMappedStorageBuffer(renderer, static_cast<vk::DeviceSize>(capacity) * sizeof(InstanceData));
        if (!buffer)
        {
            return Err(buffer.error());
        }
        renderer.instancing.buffers[frame] = *buffer;
        renderer.instancing.capacity[frame] = capacity;

        vk::DescriptorBufferInfo bufferInfo{ (*buffer)->buffer, 0, (*buffer)->size };
        vk::WriteDescriptorSet write{};
        write.dstSet = renderer.instancing.sets[frame];
        write.dstBinding = 0;
        write.descriptorType = vk::DescriptorType::eStorageBuffer;
        write.setBufferInfo(bufferInfo);
        renderer.context.device.updateDescriptorSets(write, {});
        return {};
    }
    void submitDrawList(Renderer& renderer, const glm::mat4& viewProj, const std::vector<DrawItem>& items)
    {
        renderer.stats = RenderStats{};
        renderer.frame.sceneDrawList = SceneDrawList{};
        if (items.empty())
        {
            return;
        }

        // Bucket items by (pipeline, mesh); each bucket becomes one instanced draw. The
        // albedo is bindless — a per-instance index into the global texture array — so
        // items differing only by texture batch together. First-seen order preserved.
        struct Bucket
        {
            Ref<Pipeline> pipeline;
            Ref<GpuMesh> mesh;
            std::vector<InstanceData> instances;
        };
        std::vector<Bucket> buckets;
        std::vector<Ref<GpuTexture>> liveTextures;
        for (const DrawItem& item : items)
        {
            if (!item.mesh)
            {
                continue;
            }
            auto pipeline = requestMeshPipeline(renderer, item.material);
            if (!pipeline)
            {
                continue;
            }
            // Resolve the albedo to a bindless slot (default white when the item has none).
            u32 textureIndex = 0;
            if (renderer.defaultWhiteTexture)
            {
                textureIndex = renderer.defaultWhiteTexture->bindlessIndex;
            }
            if (item.texture && item.texture->image)
            {
                textureIndex = item.texture->bindlessIndex;
                liveTextures.push_back(item.texture);
            }
            Bucket* bucket = nullptr;
            for (Bucket& candidate : buckets)
            {
                if (candidate.pipeline.get() == pipeline.get() && candidate.mesh.get() == item.mesh.get())
                {
                    bucket = &candidate;
                    break;
                }
            }
            if (bucket == nullptr)
            {
                buckets.push_back(Bucket{ pipeline, item.mesh, {} });
                bucket = &buckets.back();
            }
            InstanceData instance;
            instance.model = item.model;
            instance.normalMatrix = item.normalMatrix;
            instance.baseColor = item.baseColor;
            instance.texture = glm::uvec4{ textureIndex, 0, 0, 0 };
            instance.pbr = glm::vec4{ item.metallic, item.roughness, 0.0f, 0.0f };
            instance.emissive = glm::vec4{ item.emissive * item.emissiveStrength, 0.0f };
            bucket->instances.push_back(instance);
        }

        // Flatten buckets into one contiguous instance array + per-batch ranges.
        std::vector<InstanceData> instances;
        instances.reserve(items.size());
        std::vector<DrawBatch> batches;
        for (Bucket& bucket : buckets)
        {
            DrawBatch batch;
            batch.pipeline = bucket.pipeline;
            batch.mesh = bucket.mesh;
            batch.baseInstance = static_cast<u32>(instances.size());
            batch.instanceCount = static_cast<u32>(bucket.instances.size());
            instances.insert(instances.end(), bucket.instances.begin(), bucket.instances.end());
            batches.push_back(std::move(batch));
        }

        if (instances.empty())
        {
            return;
        }
        const u32 frame = renderer.frame.index;
        if (Result<void> ok = ensureInstanceCapacity(renderer, frame, static_cast<u32>(instances.size())); !ok)
        {
            logError(ok.error());
            return;
        }
        const vk::DeviceSize bytes = instances.size() * sizeof(InstanceData);
        std::memcpy(renderer.instancing.buffers[frame]->mapped, instances.data(), bytes);
        vmaFlushAllocation(renderer.context.allocator, renderer.instancing.buffers[frame]->alloc, 0, bytes);

        u32 drawCalls = 0;
        u32 drawnInstances = 0;
        for (const DrawBatch& batch : batches)
        {
            drawCalls = drawCalls + static_cast<u32>(batch.mesh->submeshes.size());
            drawnInstances = drawnInstances + batch.instanceCount;
        }
        renderer.stats.drawCalls = drawCalls;
        renderer.stats.batches = static_cast<u32>(batches.size());
        renderer.stats.instances = drawnInstances;

        SceneDrawList list;
        list.viewProj = viewProj;
        list.batches = std::move(batches);
        list.liveTextures = std::move(liveTextures);
        list.lightSet = renderer.lighting.lightSets[frame];
        list.instanceSet = renderer.instancing.sets[frame];
        list.valid = true;
        renderer.frame.sceneDrawList = std::move(list);
    }

    // Record the scene's shaded geometry. All mesh PSOs share the layout, so the light +
    // instance sets and the viewProj push bind once; per batch then binds its pipeline +
    // material set and issues one instanced drawIndexed.
    void recordSceneDrawList(Renderer& renderer, vk::CommandBuffer cmd)
    {
        SceneDrawList& list = renderer.frame.sceneDrawList;
        if (!list.valid || list.batches.empty())
        {
            return;
        }
        vk::PipelineLayout layout = list.batches[0].pipeline->layout;
        // All sets bind once: the bindless albedo array (0) + light (1) + instance (2).
        // Per-instance texture indices live in the instance buffer, so no per-batch set.
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, renderer.descriptors.bindlessSet, {});
        std::array<vk::DescriptorSet, 2> frameSets{ list.lightSet, list.instanceSet };
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 1, frameSets, {});
        // Set 3 = IBL (irradiance + prefiltered + BRDF LUT); baked once, always valid.
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 3, renderer.ibl.set, {});
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(glm::mat4), &list.viewProj);
        for (const DrawBatch& batch : list.batches)
        {
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, batch.pipeline->pipeline);
            vk::DeviceSize offset = 0;
            cmd.bindVertexBuffers(0, batch.mesh->vertexBuffer, offset);
            cmd.bindIndexBuffer(batch.mesh->indexBuffer, 0, vk::IndexType::eUint32);
            for (const Submesh& submesh : batch.mesh->submeshes)
            {
                cmd.drawIndexed(submesh.indexCount, batch.instanceCount, submesh.firstIndex,
                                submesh.vertexOffset, batch.baseInstance);
            }
        }
    }

    void recordDepthPrepass(Renderer& renderer, vk::CommandBuffer cmd)
    {
        SceneDrawList& list = renderer.frame.sceneDrawList;
        if (!list.valid || !renderer.pipelines.depthPrepass)
        {
            return;
        }
        // The vertex-only pipeline needs only the instance set (set 2) + viewProj push.
        vk::PipelineLayout layout = renderer.pipelines.depthPrepass->layout;
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, renderer.pipelines.depthPrepass->pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 2, list.instanceSet, {});
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(glm::mat4), &list.viewProj);
        for (const DrawBatch& batch : list.batches)
        {
            vk::DeviceSize offset = 0;
            cmd.bindVertexBuffers(0, batch.mesh->vertexBuffer, offset);
            cmd.bindIndexBuffer(batch.mesh->indexBuffer, 0, vk::IndexType::eUint32);
            for (const Submesh& submesh : batch.mesh->submeshes)
            {
                cmd.drawIndexed(submesh.indexCount, batch.instanceCount, submesh.firstIndex,
                                submesh.vertexOffset, batch.baseInstance);
            }
        }
    }

    void recordShadowDepth(Renderer& renderer, vk::CommandBuffer cmd, const glm::mat4& lightViewProj)
    {
        SceneDrawList& list = renderer.frame.sceneDrawList;
        if (!list.valid || !renderer.pipelines.shadowDepth)
        {
            return;
        }
        // Same vertex-only path as the depth pre-pass, but the push constant is the LIGHT's
        // viewProj and the rasterizer applies a depth bias (set here per pass).
        vk::PipelineLayout layout = renderer.pipelines.shadowDepth->layout;
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, renderer.pipelines.shadowDepth->pipeline);
        cmd.setDepthBias(ShadowDepthBiasConstant, 0.0f, ShadowDepthBiasSlope);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 2, list.instanceSet, {});
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(glm::mat4), &lightViewProj);
        for (const DrawBatch& batch : list.batches)
        {
            vk::DeviceSize offset = 0;
            cmd.bindVertexBuffers(0, batch.mesh->vertexBuffer, offset);
            cmd.bindIndexBuffer(batch.mesh->indexBuffer, 0, vk::IndexType::eUint32);
            for (const Submesh& submesh : batch.mesh->submeshes)
            {
                cmd.drawIndexed(submesh.indexCount, batch.instanceCount, submesh.firstIndex,
                                submesh.vertexOffset, batch.baseInstance);
            }
        }
    }
}
