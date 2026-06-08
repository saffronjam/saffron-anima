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

namespace se
{
    // Issues one instanced drawIndexed per submesh of a batch. The frame's instance
    // buffer is laid out submesh-major (see submitDrawList), so submesh s reads its rows
    // by offsetting baseInstance by s * instanceCount.
    void recordBatchSubmeshes(vk::CommandBuffer cmd, const DrawBatch& batch)
    {
        const auto& submeshes = batch.mesh->submeshes;
        for (u32 s = 0; s < submeshes.size(); s = s + 1)
        {
            const Submesh& submesh = submeshes[s];
            cmd.drawIndexed(submesh.indexCount, batch.instanceCount, submesh.firstIndex, submesh.vertexOffset,
                            batch.baseInstance + s * batch.instanceCount);
        }
    }

    auto uploadMesh(Renderer& renderer, const Mesh& mesh) -> Result<Ref<GpuMesh>>
    {
        return uploadMesh(renderer, mesh, std::vector<VertexSkin>{});
    }

    auto uploadMesh(Renderer& renderer, const Mesh& mesh, const std::vector<VertexSkin>& skin) -> Result<Ref<GpuMesh>>
    {
        if (mesh.vertices.empty() || mesh.indices.empty())
        {
            return Err(std::string{ "uploadMesh: empty mesh" });
        }
        if (!skin.empty() && skin.size() != mesh.vertices.size())
        {
            return Err(std::string{ "uploadMesh: skin stream does not parallel the vertices" });
        }
        const vk::DeviceSize vertexBytes = mesh.vertices.size() * sizeof(Vertex);
        const vk::DeviceSize indexBytes = mesh.indices.size() * sizeof(u32);
        const vk::DeviceSize skinBytes = skin.size() * sizeof(VertexSkin);

        // One staging buffer holds [vertices | indices | skin]; copies fan it out to
        // device-local vertex + index (+ skin) buffers.
        VkBufferCreateInfo stagingInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        stagingInfo.size = vertexBytes + indexBytes + skinBytes;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo stagingAlloc{};
        stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = nullptr;
        VmaAllocationInfo stagingMapped{};
        if (vmaCreateBuffer(renderer.context.allocator, &stagingInfo, &stagingAlloc, &staging, &stagingAllocation,
                            &stagingMapped) != VK_SUCCESS)
        {
            return Err(std::string{ "uploadMesh: staging vmaCreateBuffer failed" });
        }
        std::memcpy(stagingMapped.pMappedData, mesh.vertices.data(), vertexBytes);
        std::memcpy(static_cast<char*>(stagingMapped.pMappedData) + vertexBytes, mesh.indices.data(), indexBytes);
        if (!skin.empty())
        {
            std::memcpy(static_cast<char*>(stagingMapped.pMappedData) + vertexBytes + indexBytes, skin.data(),
                        skinBytes);
        }
        vmaFlushAllocation(renderer.context.allocator, stagingAllocation, 0, VK_WHOLE_SIZE);

        // When RT is on, the vertex/index buffers also feed BLAS builds: they need shader
        // device address + AS-build-input usage.
        VkBufferUsageFlags rtUsage = 0;
        if (renderer.context.rtSupported)
        {
            rtUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        }
        auto makeDeviceBuffer = [&](vk::DeviceSize size, VkBufferUsageFlags usage, VkBuffer& outBuffer,
                                    VmaAllocation& outAlloc) -> bool
        {
            VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size = size;
            info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rtUsage;
            VmaAllocationCreateInfo alloc{};
            alloc.usage = VMA_MEMORY_USAGE_AUTO;
            return vmaCreateBuffer(renderer.context.allocator, &info, &alloc, &outBuffer, &outAlloc, nullptr) ==
                   VK_SUCCESS;
        };

        GpuMesh gpu;
        gpu.allocator = renderer.context.allocator;
        gpu.indexCount = static_cast<u32>(mesh.indices.size());
        gpu.vertexCount = static_cast<u32>(mesh.vertices.size());
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
        VkBuffer skinBuffer = VK_NULL_HANDLE;
        VmaAllocation vertexAlloc = nullptr;
        VmaAllocation indexAlloc = nullptr;
        VmaAllocation skinAlloc = nullptr;
        if (!makeDeviceBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertexBuffer, vertexAlloc) ||
            !makeDeviceBuffer(indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexBuffer, indexAlloc) ||
            (!skin.empty() && !makeDeviceBuffer(skinBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, skinBuffer, skinAlloc)))
        {
            if (vertexBuffer != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(renderer.context.allocator, vertexBuffer, vertexAlloc);
            }
            if (indexBuffer != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(renderer.context.allocator, indexBuffer, indexAlloc);
            }
            vmaDestroyBuffer(renderer.context.allocator, staging, stagingAllocation);
            return Err(std::string{ "uploadMesh: device vmaCreateBuffer failed" });
        }
        gpu.vertexBuffer = vk::Buffer{ vertexBuffer };
        gpu.vertexAlloc = vertexAlloc;
        gpu.indexBuffer = vk::Buffer{ indexBuffer };
        gpu.indexAlloc = indexAlloc;
        gpu.skinBuffer = vk::Buffer{ skinBuffer };
        gpu.skinAlloc = skinAlloc;

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = renderer.frame.frames[0].commandPool;
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds =
            checked(renderer.context.device.allocateCommandBuffers(cmdAlloc), "uploadMesh: allocateCommandBuffers");
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
        if (gpu.skinBuffer)
        {
            cmd.copyBuffer(vk::Buffer{ staging }, gpu.skinBuffer,
                           vk::BufferCopy{ vertexBytes + indexBytes, 0, skinBytes });
        }
        static_cast<void>(cmd.end());

        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = cmd;
        vk::SubmitInfo2 submitInfo{};
        submitInfo.setCommandBufferInfos(cmdInfo);
        static_cast<void>(renderer.context.graphicsQueue.submit2(submitInfo, nullptr));
        static_cast<void>(renderer.context.device.waitIdle());
        renderer.context.device.freeCommandBuffers(renderer.frame.frames[0].commandPool, cmd);
        vmaDestroyBuffer(renderer.context.allocator, staging, stagingAllocation);

        auto meshRef = std::make_shared<GpuMesh>(std::move(gpu));
        // Build this mesh's BLAS once (RT geometry occlusion oracle) when RT is available.
        if (renderer.context.rtSupported)
        {
            if (Result<Ref<AccelerationStructure>> blas = buildBlas(renderer, *meshRef); blas)
            {
                meshRef->blas = *blas;
                renderer.rt.blasCount = renderer.rt.blasCount + 1;
            }
            else
            {
                logWarn(std::format("BLAS build failed: {}", blas.error()));
            }
        }
        logInfo(std::format("uploaded mesh: {} vertices, {} indices, {} submeshes", mesh.vertices.size(),
                            mesh.indices.size(), mesh.submeshes.size()));
        return meshRef;
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
    // Ensures the current frame's joint palette holds at least `count` matrices and is
    // written to set 2 binding 1; same grow-only policy as the instance buffer.
    auto ensureJointCapacity(Renderer& renderer, u32 frame, u32 count) -> Result<void>
    {
        if (renderer.instancing.jointBuffers[frame] && renderer.instancing.jointCapacity[frame] >= count)
        {
            return {};
        }
        u32 capacity = renderer.instancing.jointCapacity[frame];
        if (capacity == 0)
        {
            capacity = 128;
        }
        while (capacity < count)
        {
            capacity = capacity * 2;
        }
        Result<Ref<Buffer>> buffer =
            makeMappedStorageBuffer(renderer, static_cast<vk::DeviceSize>(capacity) * sizeof(glm::mat4));
        if (!buffer)
        {
            return Err(buffer.error());
        }
        renderer.instancing.jointBuffers[frame] = *buffer;
        renderer.instancing.jointCapacity[frame] = capacity;

        vk::DescriptorBufferInfo bufferInfo{ (*buffer)->buffer, 0, (*buffer)->size };
        vk::WriteDescriptorSet write{};
        write.dstSet = renderer.instancing.sets[frame];
        write.dstBinding = 1;
        write.descriptorType = vk::DescriptorType::eStorageBuffer;
        write.setBufferInfo(bufferInfo);
        renderer.context.device.updateDescriptorSets(write, {});
        return {};
    }

    void submitDrawList(Renderer& renderer, const glm::mat4& viewProj, const std::vector<DrawItem>& items)
    {
        submitDrawList(renderer, viewProj, items, std::vector<glm::mat4>{});
    }

    void submitDrawList(Renderer& renderer, const glm::mat4& viewProj, const std::vector<DrawItem>& items,
                        const std::vector<glm::mat4>& joints)
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
            bool skinned = false;
            // Per logical instance: one InstanceData row per mesh submesh, in submesh order.
            std::vector<std::vector<InstanceData>> instances;
        };
        std::vector<Bucket> buckets;
        std::vector<Ref<GpuTexture>> liveTextures;
        const u32 defaultTextureIndex = renderer.defaultWhiteTexture ? renderer.defaultWhiteTexture->bindlessIndex : 0u;
        for (const DrawItem& item : items)
        {
            if (!item.mesh)
            {
                continue;
            }
            if (item.skinned && !item.mesh->skinBuffer)
            {
                continue;  // a skinned draw needs the mesh's VertexSkin stream
            }
            auto pipeline = requestMeshPipeline(renderer, item.material, item.skinned);
            if (!pipeline)
            {
                continue;
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
                buckets.push_back(Bucket{ pipeline, item.mesh, item.skinned, {} });
                bucket = &buckets.back();
            }
            // One row per submesh; a single material entry covers every submesh (clamped).
            const std::size_t submeshCount = std::max<std::size_t>(item.mesh->submeshes.size(), 1);
            std::vector<InstanceData> rows;
            rows.reserve(submeshCount);
            for (std::size_t s = 0; s < submeshCount; s = s + 1)
            {
                u32 albedoIndex = defaultTextureIndex;
                u32 mrIndex = defaultTextureIndex;  // white default → metallic*1, roughness*1
                glm::vec4 baseColor{ 1.0f };
                glm::vec2 metallicRoughness{ 0.0f, 1.0f };
                glm::vec3 emissive{ 0.0f };
                if (!item.submeshMaterials.empty())
                {
                    const SubmeshMaterial& mat = item.submeshMaterials[std::min(s, item.submeshMaterials.size() - 1)];
                    baseColor = mat.baseColor;
                    metallicRoughness = glm::vec2{ mat.metallic, mat.roughness };
                    emissive = mat.emissive * mat.emissiveStrength;
                    if (mat.albedoTexture && mat.albedoTexture->image)
                    {
                        albedoIndex = mat.albedoTexture->bindlessIndex;
                        liveTextures.push_back(mat.albedoTexture);
                    }
                    if (mat.metallicRoughnessTexture && mat.metallicRoughnessTexture->image)
                    {
                        mrIndex = mat.metallicRoughnessTexture->bindlessIndex;
                        liveTextures.push_back(mat.metallicRoughnessTexture);
                    }
                }
                InstanceData instance;
                instance.model = item.model;
                instance.normalMatrix = item.normalMatrix;
                instance.baseColor = baseColor;
                // .x = albedo bindless index, .y = joint-palette offset (skinned), .z = metallic-roughness.
                instance.texture = glm::uvec4{ albedoIndex, item.jointOffset, mrIndex, 0 };
                instance.pbr = glm::vec4{ metallicRoughness.x, metallicRoughness.y, 0.0f, 0.0f };
                instance.emissive = glm::vec4{ emissive, 0.0f };
                rows.push_back(instance);
            }
            bucket->instances.push_back(std::move(rows));
        }

        // Flatten submesh-major: lay every instance's submesh-s row contiguously, so a
        // submesh draws all instances at once by offsetting baseInstance by s * instanceCount.
        std::vector<InstanceData> instances;
        std::vector<DrawBatch> batches;
        for (Bucket& bucket : buckets)
        {
            if (bucket.instances.empty())
            {
                continue;
            }
            const u32 submeshCount = static_cast<u32>(bucket.instances.front().size());
            DrawBatch batch;
            batch.pipeline = bucket.pipeline;
            batch.mesh = bucket.mesh;
            batch.skinned = bucket.skinned;
            batch.baseInstance = static_cast<u32>(instances.size());
            batch.instanceCount = static_cast<u32>(bucket.instances.size());
            for (u32 s = 0; s < submeshCount; s = s + 1)
            {
                for (const std::vector<InstanceData>& rows : bucket.instances)
                {
                    instances.push_back(rows[s]);
                }
            }
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
        if (!joints.empty())
        {
            if (Result<void> ok = ensureJointCapacity(renderer, frame, static_cast<u32>(joints.size())); !ok)
            {
                logError(ok.error());
                return;
            }
            const vk::DeviceSize jointBytes = joints.size() * sizeof(glm::mat4);
            std::memcpy(renderer.instancing.jointBuffers[frame]->mapped, joints.data(), jointBytes);
            vmaFlushAllocation(renderer.context.allocator, renderer.instancing.jointBuffers[frame]->alloc, 0,
                               jointBytes);
        }

        u32 drawCalls = 0;
        u32 drawnInstances = 0;
        u32 triangles = 0;
        for (const DrawBatch& batch : batches)
        {
            drawCalls = drawCalls + static_cast<u32>(batch.mesh->submeshes.size());
            drawnInstances = drawnInstances + batch.instanceCount;
            u32 meshIndices = 0;
            for (const Submesh& submesh : batch.mesh->submeshes)
            {
                meshIndices = meshIndices + submesh.indexCount;
            }
            triangles = triangles + (meshIndices / 3) * batch.instanceCount;
        }
        renderer.stats.drawCalls = drawCalls;
        renderer.stats.batches = static_cast<u32>(batches.size());
        renderer.stats.instances = drawnInstances;
        renderer.stats.triangles = triangles;

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
        // Set 3 = IBL (irradiance + prefiltered + BRDF LUT) plus the reflection probes at
        // bindings 3-5 (prefiltered + irradiance cube arrays + meta SSBO). Baked once, always
        // valid; every probe slot is seeded with the global IBL cubes. Probes are gated in-shader
        // by the probe count (ambientColor.w == 0 -> pure global IBL fallback).
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 3, renderer.ibl.set, {});
        // Set 4 = screen-space maps (AO + contact + SSGI); each gated by its flag in the
        // shader, so the bind is always valid even when an effect is off.
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 4, renderer.ssao.meshSet, {});
        // Set 5 = DDGI probe atlases (irradiance + distance); gated by screenFlags.z.
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 5, renderer.ddgi.meshSet, {});
        // Set 6 = the RT TLAS, set 7 = the ReSTIR radiance (only when RT is supported, so the
        // PSO layout has them). Both gated in-shader by their runtime flags.
        u32 binds = 5;  // sets 0, 1 (light+instance), 3, 4, 5 bind once regardless of batch count
        if (renderer.context.rtSupported && renderer.rt.meshSets[renderer.frame.index])
        {
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 6,
                                   renderer.rt.meshSets[renderer.frame.index], {});
            binds = binds + 1;
            if (renderer.restir.meshSet)
            {
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 7, renderer.restir.meshSet, {});
                binds = binds + 1;
            }
        }
        // Binds stay constant in the batch count (bindless textures + per-instance indices) —
        // surfacing the count confirms the path is not O(draws).
        renderer.stats.descriptorBinds = renderer.stats.descriptorBinds + binds;
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(glm::mat4), &list.viewProj);
        for (const DrawBatch& batch : list.batches)
        {
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, batch.pipeline->pipeline);
            vk::DeviceSize offset = 0;
            cmd.bindVertexBuffers(0, batch.mesh->vertexBuffer, offset);
            if (batch.skinned)
            {
                cmd.bindVertexBuffers(1, batch.mesh->skinBuffer, offset);
            }
            cmd.bindIndexBuffer(batch.mesh->indexBuffer, 0, vk::IndexType::eUint32);
            recordBatchSubmeshes(cmd, batch);
        }
    }

    // Record the fullscreen sky: bind the bindless array (set 0, for a Texture-mode panorama)
    // + the envCube set (set 1), push this frame's inverse view-projection + sky params, and
    // draw a single fullscreen triangle. The graph sets the dynamic viewport/scissor.
    void recordSky(Renderer& renderer, vk::CommandBuffer cmd)
    {
        if (!renderer.sky.pipeline)
        {
            return;
        }
        vk::PipelineLayout layout = renderer.sky.pipeline->layout;
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, renderer.sky.pipeline->pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, renderer.descriptors.bindlessSet, {});
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 1, renderer.sky.set, {});
        struct SkyPush
        {
            glm::mat4 invViewProj;
            glm::vec4 params;  // intensity, rotation, mode, textureIndex
            glm::vec4 clearColor;
        } push;
        push.invViewProj = glm::inverse(renderer.frame.sceneDrawList.viewProj);
        push.params = glm::vec4(renderer.sky.intensity, renderer.sky.rotation, static_cast<f32>(renderer.sky.mode),
                                static_cast<f32>(renderer.sky.textureIndex));
        push.clearColor = glm::vec4(renderer.sky.clearColor, 1.0f);
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(push), &push);
        cmd.draw(3, 1, 0, 0);
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
            if (batch.skinned)
            {
                continue;  // skinned draws render in the scene pass only (v1)
            }
            vk::DeviceSize offset = 0;
            cmd.bindVertexBuffers(0, batch.mesh->vertexBuffer, offset);
            cmd.bindIndexBuffer(batch.mesh->indexBuffer, 0, vk::IndexType::eUint32);
            recordBatchSubmeshes(cmd, batch);
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
            if (batch.skinned)
            {
                continue;  // skinned draws render in the scene pass only (v1)
            }
            vk::DeviceSize offset = 0;
            cmd.bindVertexBuffers(0, batch.mesh->vertexBuffer, offset);
            cmd.bindIndexBuffer(batch.mesh->indexBuffer, 0, vk::IndexType::eUint32);
            recordBatchSubmeshes(cmd, batch);
        }
    }

    // Records the thin G-buffer prepass: view-space normal + view-Z, for SSAO. The push
    // constant is the camera viewProj + view (set per frame on the Ssao state).
    void recordGbuffer(Renderer& renderer, vk::CommandBuffer cmd)
    {
        SceneDrawList& list = renderer.frame.sceneDrawList;
        if (!list.valid || !renderer.pipelines.gbuffer)
        {
            return;
        }
        vk::PipelineLayout layout = renderer.pipelines.gbuffer->layout;
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, renderer.pipelines.gbuffer->pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 2, list.instanceSet, {});
        struct GbufferPush
        {
            glm::mat4 viewProj;
            glm::mat4 view;
        } push{ renderer.ssao.viewProj, renderer.ssao.view };
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(push), &push);
        for (const DrawBatch& batch : list.batches)
        {
            if (batch.skinned)
            {
                continue;  // skinned draws render in the scene pass only (v1)
            }
            vk::DeviceSize offset = 0;
            cmd.bindVertexBuffers(0, batch.mesh->vertexBuffer, offset);
            cmd.bindIndexBuffer(batch.mesh->indexBuffer, 0, vk::IndexType::eUint32);
            recordBatchSubmeshes(cmd, batch);
        }
    }

    // Records the motion-vector prepass: per-pixel screen motion from camera reprojection
    // (cur vs prev viewProj). The push constant carries both, stored on the Ssao/Renderer
    // state (viewProj this frame, prevViewProj last frame).
    void recordMotion(Renderer& renderer, vk::CommandBuffer cmd)
    {
        SceneDrawList& list = renderer.frame.sceneDrawList;
        if (!list.valid || !renderer.pipelines.motion)
        {
            return;
        }
        vk::PipelineLayout layout = renderer.pipelines.motion->layout;
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, renderer.pipelines.motion->pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 2, list.instanceSet, {});
        struct MotionPush
        {
            glm::mat4 curViewProj;
            glm::mat4 prevViewProj;
        } push{ list.viewProj, renderer.prevViewProj };
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(push), &push);
        for (const DrawBatch& batch : list.batches)
        {
            if (batch.skinned)
            {
                continue;  // skinned draws render in the scene pass only (v1)
            }
            vk::DeviceSize offset = 0;
            cmd.bindVertexBuffers(0, batch.mesh->vertexBuffer, offset);
            cmd.bindIndexBuffer(batch.mesh->indexBuffer, 0, vk::IndexType::eUint32);
            recordBatchSubmeshes(cmd, batch);
        }
    }

    // Renders world distance-to-light into the 6 faces of the point shadow cube. Runs as a
    // Compute-kind graph pass body (so the graph opens no rendering scope); this opens its
    // own per-face dynamic-rendering scope + transitions the cube General<->ShaderReadOnly,
    // since the graph's single-layer barrier can't span the 6-layer cube.
    void recordPointShadow(Renderer& renderer, vk::CommandBuffer cmd, glm::vec3 lightPos, f32 farPlane)
    {
        SceneDrawList& list = renderer.frame.sceneDrawList;
        if (!list.valid || !renderer.pipelines.pointShadow)
        {
            return;
        }
        Targets& targets = renderer.targets;
        const std::array<glm::mat4, 6> faces = pointShadowFaceMatrices(lightPos, farPlane);
        vk::PipelineLayout layout = renderer.pipelines.pointShadow->layout;
        const vk::Extent2D extent = targets.pointShadowCube.extent;

        // All 6 cube layers: ShaderReadOnly (entry) -> ColorAttachment for rendering.
        auto cubeBarrier = [&](vk::ImageLayout oldL, vk::ImageLayout newL, vk::PipelineStageFlags2 srcS,
                               vk::AccessFlags2 srcA, vk::PipelineStageFlags2 dstS, vk::AccessFlags2 dstA)
        {
            vk::ImageMemoryBarrier2 b{};
            b.srcStageMask = srcS;
            b.srcAccessMask = srcA;
            b.dstStageMask = dstS;
            b.dstAccessMask = dstA;
            b.oldLayout = oldL;
            b.newLayout = newL;
            b.image = targets.pointShadowCube.image;
            b.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 6 };
            vk::DependencyInfo d{};
            d.setImageMemoryBarriers(b);
            cmd.pipelineBarrier2(d);
        };
        cubeBarrier(vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eColorAttachmentOptimal,
                    vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
                    vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite);
        // The shared depth scratch: Undefined -> DepthAttachment (cleared each face).
        transitionImage(
            cmd, targets.pointShadowDepth.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthAttachmentOptimal,
            vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
            vk::AccessFlagBits2::eDepthStencilAttachmentWrite, vk::ImageAspectFlagBits::eDepth);

        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, renderer.pipelines.pointShadow->pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 2, list.instanceSet, {});
        vk::Viewport viewport{
            0.0f, 0.0f, static_cast<f32>(extent.width), static_cast<f32>(extent.height), 0.0f, 1.0f
        };
        vk::Rect2D scissor{ vk::Offset2D{ 0, 0 }, extent };

        for (u32 face = 0; face < 6; face = face + 1)
        {
            vk::RenderingAttachmentInfo colorAttach{};
            colorAttach.imageView = targets.pointShadowFaces[face];
            colorAttach.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
            colorAttach.loadOp = vk::AttachmentLoadOp::eClear;
            colorAttach.storeOp = vk::AttachmentStoreOp::eStore;
            // Clear to far distance so untouched texels read "no occluder".
            colorAttach.clearValue =
                vk::ClearValue{ vk::ClearColorValue{ std::array<f32, 4>{ farPlane * 2.0f, 0.0f, 0.0f, 0.0f } } };
            vk::RenderingAttachmentInfo depthAttach{};
            depthAttach.imageView = targets.pointShadowDepth.view;
            depthAttach.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
            depthAttach.loadOp = vk::AttachmentLoadOp::eClear;
            depthAttach.storeOp = vk::AttachmentStoreOp::eDontCare;
            depthAttach.clearValue = vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } };
            vk::RenderingInfo rendering{};
            rendering.renderArea = scissor;
            rendering.layerCount = 1;
            rendering.setColorAttachments(colorAttach);
            rendering.setPDepthAttachment(&depthAttach);

            // The depth scratch is reused across faces; barrier write->write between faces.
            if (face > 0)
            {
                transitionImage(
                    cmd, targets.pointShadowDepth.image, vk::ImageLayout::eDepthAttachmentOptimal,
                    vk::ImageLayout::eDepthAttachmentOptimal, vk::PipelineStageFlagBits2::eLateFragmentTests,
                    vk::AccessFlagBits2::eDepthStencilAttachmentWrite, vk::PipelineStageFlagBits2::eEarlyFragmentTests,
                    vk::AccessFlagBits2::eDepthStencilAttachmentWrite, vk::ImageAspectFlagBits::eDepth);
            }
            cmd.beginRendering(rendering);
            cmd.setViewport(0, viewport);
            cmd.setScissor(0, scissor);
            struct PointPush
            {
                glm::mat4 viewProj;
                glm::vec4 lightPos;
            } push{ faces[face], glm::vec4(lightPos, farPlane) };
            cmd.pushConstants(layout, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
                              sizeof(push), &push);
            for (const DrawBatch& batch : list.batches)
            {
                if (batch.skinned)
                {
                    continue;  // skinned draws render in the scene pass only (v1)
                }
                vk::DeviceSize offset = 0;
                cmd.bindVertexBuffers(0, batch.mesh->vertexBuffer, offset);
                cmd.bindIndexBuffer(batch.mesh->indexBuffer, 0, vk::IndexType::eUint32);
                recordBatchSubmeshes(cmd, batch);
            }
            cmd.endRendering();
        }

        // All 6 layers back to ShaderReadOnly for the scene pass to sample.
        cubeBarrier(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                    vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                    vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
        targets.pointShadowCube.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    }
}
