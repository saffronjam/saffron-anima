module;

#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_NO_SMART_HANDLE
#include <vulkan/vulkan.hpp>
#include <vk_mem_alloc.h>
#include <VkBootstrap.h>
#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module Saffron.Rendering:Types;

import Saffron.Core;
import Saffron.Window;
import Saffron.Geometry;
import :RenderGraph;

export namespace se
{
    // The graphics queue is externally synchronized in Vulkan. The frame loop and the thumbnail
    // worker thread both submit/present on it, so every `submit2`/`presentKHR` takes this lock —
    // one queue, one mutex, process-wide.
    inline auto gpuQueueMutex() -> std::mutex&
    {
        static std::mutex m;
        return m;
    }

    // The bindless texture array + its free-list are host-side shared state. `vkUpdateDescriptorSets`
    // on the same set and the free-list push/pop must be externally synchronized once the worker can
    // also upload textures, so claim/write/return all take this lock.
    inline auto bindlessMutex() -> std::mutex&
    {
        static std::mutex m;
        return m;
    }

    // Format of the offscreen depth buffer. D32_SFLOAT is universally supported.
    inline constexpr vk::Format DepthFormat = vk::Format::eD32Sfloat;

    // Format of the offscreen color target. RGBA16_SFLOAT so the scene pass can write
    // linear HDR radiance that survives to the mandatory tonemap pass; it is also a
    // storage image (the tonemap/FXAA compute passes write it) and sampled by the present blit.
    inline constexpr vk::Format OffscreenColorFormat = vk::Format::eR16G16B16A16Sfloat;

    // A unit of GPU work recorded into the active command buffer — the deferred
    // submission seam. The backend supplies the command buffer.
    using RenderFn = std::function<void(vk::CommandBuffer)>;

    inline constexpr u32 MaxFramesInFlight = 2;

    /// Which editor pane a render view targets. Each view owns its own ViewTargets (offscreen
    /// images + temporal state); the renderer renders the active view.
    enum class ViewId : u8
    {
        Scene = 0,
        AssetPreview = 1,
    };

    inline constexpr u32 ViewCount = 2;

    // Capacity of the bindless texture array (set 0). One global combined-image-sampler
    // array indexed per-instance; lavapipe + desktop GPUs allow far more, this is plenty.
    inline constexpr u32 MaxBindlessTextures = 1024;

    // Per-frame ceiling on compute-skinning descriptor sets — one per skinned mesh-instance.
    // The skin pool is reset and re-allocated each frame; instances past this are skipped (logged).
    inline constexpr u32 SkinMaxSetsPerFrame = 64;

    // Upper bound on GPU scopes the profiler times per frame — top-level passes plus any
    // opt-in sub-scopes nested inside them. Each scope takes two timestamp slots (begin +
    // end); scopes past this cap are simply not timed (the recorder truncates gracefully).
    inline constexpr u32 MaxProfiledScopes = 128;

    struct FrameData
    {
        vk::CommandPool commandPool;
        vk::CommandBuffer commandBuffer;
        vk::Semaphore imageAvailable;
        vk::Fence inFlight;
    };

    // A graphics pipeline that owns its vk handles and frees them on destruction.
    // Move-only: the destructor + move-assignment are resource management. Owned
    // by the Renderer (never crosses to client code), destroyed before the device.
    struct Pipeline
    {
        vk::Device device;  // borrowed
        vk::Pipeline pipeline;
        vk::PipelineLayout layout;

        Pipeline() = default;
        Pipeline(const Pipeline&) = delete;
        auto operator=(const Pipeline&) -> Pipeline& = delete;

        Pipeline(Pipeline&& other) noexcept : device(other.device), pipeline(other.pipeline), layout(other.layout)
        {
            other.device = nullptr;
            other.pipeline = nullptr;
            other.layout = nullptr;
        }

        auto operator=(Pipeline&& other) noexcept -> Pipeline&
        {
            if (this != &other)
            {
                reset();
                device = other.device;
                pipeline = other.pipeline;
                layout = other.layout;
                other.device = nullptr;
                other.pipeline = nullptr;
                other.layout = nullptr;
            }
            return *this;
        }

        ~Pipeline()
        {
            reset();
        }

        void reset()
        {
            if (device)
            {
                if (pipeline)
                {
                    device.destroyPipeline(pipeline);
                }
                if (layout)
                {
                    device.destroyPipelineLayout(layout);
                }
            }
            pipeline = nullptr;
            layout = nullptr;
        }
    };

    // A VMA-allocated image that owns its handle + view + allocation and frees
    // them on destruction. Move-only, like Pipeline. Used for offscreen targets.
    struct Image
    {
        vk::Device device;                 // borrowed
        VmaAllocator allocator = nullptr;  // borrowed
        vk::Image image;
        vk::ImageView view;
        VmaAllocation alloc = nullptr;
        vk::Extent2D extent;
        vk::Format format = vk::Format::eUndefined;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;  // tracked across frames

        Image() = default;
        Image(const Image&) = delete;
        auto operator=(const Image&) -> Image& = delete;

        Image(Image&& other) noexcept
            : device(other.device), allocator(other.allocator), image(other.image), view(other.view),
              alloc(other.alloc), extent(other.extent), format(other.format), layout(other.layout)
        {
            other.device = nullptr;
            other.allocator = nullptr;
            other.image = nullptr;
            other.view = nullptr;
            other.alloc = nullptr;
            other.layout = vk::ImageLayout::eUndefined;
        }

        auto operator=(Image&& other) noexcept -> Image&
        {
            if (this != &other)
            {
                reset();
                device = other.device;
                allocator = other.allocator;
                image = other.image;
                view = other.view;
                alloc = other.alloc;
                extent = other.extent;
                format = other.format;
                layout = other.layout;
                other.device = nullptr;
                other.allocator = nullptr;
                other.image = nullptr;
                other.view = nullptr;
                other.alloc = nullptr;
                other.layout = vk::ImageLayout::eUndefined;
            }
            return *this;
        }

        ~Image()
        {
            reset();
        }

        void reset()
        {
            if (device && view)
            {
                device.destroyImageView(view);
            }
            if (allocator != nullptr && image)
            {
                vmaDestroyImage(allocator, static_cast<VkImage>(image), alloc);
            }
            view = nullptr;
            image = nullptr;
            alloc = nullptr;
        }
    };

    struct AccelerationStructure;  // defined below; GpuMesh holds a Ref (shared_ptr) to its BLAS

    // A device-local mesh: vertex + index buffers + the submesh ranges. Move-only
    // like Pipeline/Image; owned by the Renderer and freed before the allocator.
    struct GpuMesh
    {
        VmaAllocator allocator = nullptr;  // borrowed
        vk::Buffer vertexBuffer;
        VmaAllocation vertexAlloc = nullptr;
        vk::Buffer indexBuffer;
        VmaAllocation indexAlloc = nullptr;
        vk::Buffer skinBuffer;  // second vertex stream (VertexSkin); null for unskinned meshes
        VmaAllocation skinAlloc = nullptr;
        u32 indexCount = 0;
        u32 vertexCount = 0;
        std::vector<Submesh> submeshes;
        glm::vec3 boundsMin{ 0.0f };  // local-space AABB, for ray picking
        glm::vec3 boundsMax{ 0.0f };
        Ref<AccelerationStructure> blas;  // ray-tracing BLAS (null when RT is unsupported)

        GpuMesh() = default;
        GpuMesh(const GpuMesh&) = delete;
        auto operator=(const GpuMesh&) -> GpuMesh& = delete;

        GpuMesh(GpuMesh&& other) noexcept
            : allocator(other.allocator), vertexBuffer(other.vertexBuffer), vertexAlloc(other.vertexAlloc),
              indexBuffer(other.indexBuffer), indexAlloc(other.indexAlloc), skinBuffer(other.skinBuffer),
              skinAlloc(other.skinAlloc), indexCount(other.indexCount), vertexCount(other.vertexCount),
              submeshes(std::move(other.submeshes)), boundsMin(other.boundsMin), boundsMax(other.boundsMax)
        {
            other.allocator = nullptr;
            other.vertexBuffer = nullptr;
            other.vertexAlloc = nullptr;
            other.indexBuffer = nullptr;
            other.indexAlloc = nullptr;
            other.skinBuffer = nullptr;
            other.skinAlloc = nullptr;
        }

        auto operator=(GpuMesh&& other) noexcept -> GpuMesh&
        {
            if (this != &other)
            {
                reset();
                allocator = other.allocator;
                vertexBuffer = other.vertexBuffer;
                vertexAlloc = other.vertexAlloc;
                indexBuffer = other.indexBuffer;
                indexAlloc = other.indexAlloc;
                skinBuffer = other.skinBuffer;
                skinAlloc = other.skinAlloc;
                indexCount = other.indexCount;
                vertexCount = other.vertexCount;
                submeshes = std::move(other.submeshes);
                boundsMin = other.boundsMin;
                boundsMax = other.boundsMax;
                other.allocator = nullptr;
                other.vertexBuffer = nullptr;
                other.vertexAlloc = nullptr;
                other.indexBuffer = nullptr;
                other.indexAlloc = nullptr;
                other.skinBuffer = nullptr;
                other.skinAlloc = nullptr;
            }
            return *this;
        }

        ~GpuMesh()
        {
            reset();
        }

        void reset()
        {
            if (allocator != nullptr)
            {
                if (vertexBuffer)
                {
                    vmaDestroyBuffer(allocator, static_cast<VkBuffer>(vertexBuffer), vertexAlloc);
                }
                if (indexBuffer)
                {
                    vmaDestroyBuffer(allocator, static_cast<VkBuffer>(indexBuffer), indexAlloc);
                }
                if (skinBuffer)
                {
                    vmaDestroyBuffer(allocator, static_cast<VkBuffer>(skinBuffer), skinAlloc);
                }
            }
            vertexBuffer = nullptr;
            indexBuffer = nullptr;
            skinBuffer = nullptr;
            vertexAlloc = nullptr;
            indexAlloc = nullptr;
            skinAlloc = nullptr;
        }
    };

    // A device-local sampled texture (image + view), move-only like GpuMesh and
    // owned by the Renderer (freed before the allocator). The sampler is shared
    // (renderer.linearSampler), so it is not owned here.
    struct GpuTexture
    {
        vk::Device device;                 // borrowed (frees the view)
        VmaAllocator allocator = nullptr;  // borrowed (frees the image)
        vk::Image image;
        vk::ImageView view;
        VmaAllocation alloc = nullptr;
        u32 bindlessIndex = 0;  // slot in the bindless texture array (set 0)
        vk::Extent2D extent;
        vk::Format format = vk::Format::eUndefined;
        // The shared bindless free-list (Descriptors::bindlessFreeList). When set, reset() returns this
        // texture's slot to it on destroy. Held as a Ref so it outlives the descriptors.
        Ref<std::vector<u32>> bindlessFreeList;

        GpuTexture() = default;
        GpuTexture(const GpuTexture&) = delete;
        auto operator=(const GpuTexture&) -> GpuTexture& = delete;

        GpuTexture(GpuTexture&& other) noexcept
            : device(other.device), allocator(other.allocator), image(other.image), view(other.view),
              alloc(other.alloc), bindlessIndex(other.bindlessIndex), extent(other.extent), format(other.format),
              bindlessFreeList(std::move(other.bindlessFreeList))
        {
            other.device = nullptr;
            other.allocator = nullptr;
            other.image = nullptr;
            other.view = nullptr;
            other.alloc = nullptr;
        }

        auto operator=(GpuTexture&& other) noexcept -> GpuTexture&
        {
            if (this != &other)
            {
                reset();
                device = other.device;
                allocator = other.allocator;
                image = other.image;
                view = other.view;
                alloc = other.alloc;
                bindlessIndex = other.bindlessIndex;
                extent = other.extent;
                format = other.format;
                bindlessFreeList = std::move(other.bindlessFreeList);
                other.device = nullptr;
                other.allocator = nullptr;
                other.image = nullptr;
                other.view = nullptr;
                other.alloc = nullptr;
            }
            return *this;
        }

        ~GpuTexture()
        {
            reset();
        }

        void reset()
        {
            // Reclaim the bindless slot for reuse (a live `view` marks a real, non-moved-from texture).
            // A reclaimed slot's descriptor still points at the destroyed view, but no live material
            // references it; the next upload overwrites it. Locked because a worker-uploaded texture
            // may be destroyed off the main thread (a failed/duplicate handback).
            if (bindlessFreeList && view)
            {
                const std::lock_guard<std::mutex> lock(bindlessMutex());
                bindlessFreeList->push_back(bindlessIndex);
            }
            bindlessFreeList = nullptr;
            if (device && view)
            {
                device.destroyImageView(view);
            }
            if (allocator != nullptr && image)
            {
                vmaDestroyImage(allocator, static_cast<VkImage>(image), alloc);
            }
            view = nullptr;
            image = nullptr;
            alloc = nullptr;
        }
    };

    // A ray-tracing acceleration structure (BLAS or TLAS): owns the vk handle + its backing
    // device buffer, freed (handle then buffer) before the allocator. Move-only like the
    // other meta-layer resources. Destroyed via the resolved RtDispatch (no static dispatch).
    struct AccelerationStructure
    {
        vk::Device device;                                          // borrowed
        VmaAllocator allocator = nullptr;                           // borrowed
        PFN_vkDestroyAccelerationStructureKHR destroyFn = nullptr;  // borrowed
        vk::AccelerationStructureKHR handle;
        vk::DeviceAddress address = 0;
        vk::Buffer buffer;
        VmaAllocation alloc = nullptr;

        AccelerationStructure() = default;
        AccelerationStructure(const AccelerationStructure&) = delete;
        auto operator=(const AccelerationStructure&) -> AccelerationStructure& = delete;
        AccelerationStructure(AccelerationStructure&& o) noexcept
            : device(o.device), allocator(o.allocator), destroyFn(o.destroyFn), handle(o.handle), address(o.address),
              buffer(o.buffer), alloc(o.alloc)
        {
            o.handle = nullptr;
            o.buffer = nullptr;
            o.alloc = nullptr;
            o.allocator = nullptr;
        }
        auto operator=(AccelerationStructure&& o) noexcept -> AccelerationStructure&
        {
            if (this != &o)
            {
                reset();
                device = o.device;
                allocator = o.allocator;
                destroyFn = o.destroyFn;
                handle = o.handle;
                address = o.address;
                buffer = o.buffer;
                alloc = o.alloc;
                o.handle = nullptr;
                o.buffer = nullptr;
                o.alloc = nullptr;
                o.allocator = nullptr;
            }
            return *this;
        }
        ~AccelerationStructure()
        {
            reset();
        }
        void reset()
        {
            if (destroyFn != nullptr && device && handle)
            {
                destroyFn(static_cast<VkDevice>(device), static_cast<VkAccelerationStructureKHR>(handle), nullptr);
            }
            if (allocator != nullptr && buffer)
            {
                vmaDestroyBuffer(allocator, static_cast<VkBuffer>(buffer), alloc);
            }
            handle = nullptr;
            buffer = nullptr;
            alloc = nullptr;
        }
    };

    // A move-only VMA buffer. When mapped is non-null the allocation is persistently
    // mapped for per-frame host writes. Frees itself before the allocator.
    struct Buffer
    {
        VmaAllocator allocator = nullptr;  // borrowed
        vk::Buffer buffer;
        VmaAllocation alloc = nullptr;
        void* mapped = nullptr;
        vk::DeviceSize size = 0;

        Buffer() = default;
        Buffer(const Buffer&) = delete;
        auto operator=(const Buffer&) -> Buffer& = delete;

        Buffer(Buffer&& other) noexcept
            : allocator(other.allocator), buffer(other.buffer), alloc(other.alloc), mapped(other.mapped),
              size(other.size)
        {
            other.allocator = nullptr;
            other.buffer = nullptr;
            other.alloc = nullptr;
            other.mapped = nullptr;
            other.size = 0;
        }

        auto operator=(Buffer&& other) noexcept -> Buffer&
        {
            if (this != &other)
            {
                reset();
                allocator = other.allocator;
                buffer = other.buffer;
                alloc = other.alloc;
                mapped = other.mapped;
                size = other.size;
                other.allocator = nullptr;
                other.buffer = nullptr;
                other.alloc = nullptr;
                other.mapped = nullptr;
                other.size = 0;
            }
            return *this;
        }

        ~Buffer()
        {
            reset();
        }

        void reset()
        {
            if (allocator != nullptr && buffer)
            {
                vmaDestroyBuffer(allocator, static_cast<VkBuffer>(buffer), alloc);
            }
            buffer = nullptr;
            alloc = nullptr;
            mapped = nullptr;
            size = 0;
        }
    };

    /// A material: which shader/PSO variant to draw a renderable with. The per-instance
    /// albedo texture + base color live on the DrawItem; this selects the pipeline. For
    /// v1 there is one übershader; a variant flag selects a different cached PSO.
    struct Material
    {
        std::string shader = "shaders/mesh.spv";
        bool unlit = false;  // selects the unlit übershader permutation (a distinct PSO)
    };

    /// One submesh's material: albedo (null => default white) + the PBR factors. A
    /// DrawItem carries one per mesh submesh, indexed by Submesh.materialSlot order.
    struct SubmeshMaterial
    {
        Ref<GpuTexture> albedoTexture;
        Ref<GpuTexture> metallicRoughnessTexture;  // null => default white (factors unchanged)
        glm::vec4 baseColor{ 1.0f };
        f32 metallic = 0.0f;
        f32 roughness = 1.0f;
        glm::vec3 emissive{ 0.0f };
        f32 emissiveStrength = 1.0f;
        Ref<GpuTexture> normalTexture;     // tangent-space normal map (OpenGL +Y)
        Ref<GpuTexture> occlusionTexture;  // ambient occlusion (AO in R)
        Ref<GpuTexture> emissiveTexture;   // emissive map (modulates the emissive factor)
        Ref<GpuTexture> heightTexture;     // height/displacement (R) for parallax occlusion mapping
        f32 normalStrength = 1.0f;
        glm::vec2 uvTiling{ 1.0f };
        glm::vec2 uvOffset{ 0.0f };
        f32 heightScale = 0.05f;
        bool alphaClip = false;  // masked: discard fragments below alphaCutoff
        f32 alphaCutoff = 0.5f;
    };

    /// One renderable for the scene draw list: a mesh + its per-submesh materials +
    /// transform + the PSO-selecting material. submitDrawList resolves the material to a
    /// cached PSO and batches by (pipeline, mesh) into instanced draws (submesh-major
    /// instance rows) that the scene + depth passes both consume.
    struct DrawItem
    {
        Ref<GpuMesh> mesh;
        glm::mat4 model{ 1.0f };
        glm::mat4 normalMatrix{ 1.0f };
        // One entry per mesh submesh; a single entry applies to all submeshes (clamped).
        std::vector<SubmeshMaterial> submeshMaterials;
        Material material;  // selects the PSO (unlit), shared by all submeshes
        // GPU skinning: the item blends the frame's joint palette starting at
        // jointOffset; model stays identity (joints place the vertices entirely).
        bool skinned = false;
        u32 jointOffset = 0;
        u32 jointCount = 0;  // skinned only: matrices this instance contributes to the palette
        // Source entity uuid (0 = none). Keys the cross-frame motion caches: this frame's
        // model + palette are cached under it so next frame can reproject from the prior pose.
        u64 entity = 0;
    };

    // A batch of instances sharing a pipeline + mesh, drawn as one instanced drawIndexed.
    // Bindless means the albedo texture is a per-instance index (in the instance buffer),
    // not a per-batch descriptor — so batches no longer split by texture. baseInstance
    // offsets into the frame's instance buffer.
    struct DrawBatch
    {
        Ref<Pipeline> pipeline;  // resolved from the material via the PSO cache
        Ref<GpuMesh> mesh;
        u32 baseInstance = 0;
        u32 instanceCount = 0;
        // Skinned batches draw the frame's deformed-vertex buffer (binding 0) as a static
        // mesh; the compute pre-pass concatenates each instance's vertices, so this is the
        // base vertex of this batch's instance in that buffer (0 for unskinned batches).
        bool skinned = false;
        u32 deformedVertexOffset = 0;
    };

    /// One skinned mesh-instance's compute work for the frame: the descriptor set wiring
    /// its static + skin streams, the joint palette, and the deformed output, plus the push
    /// constant fields the skin kernel reads.
    struct SkinDispatch
    {
        vk::DescriptorSet set;
        u32 vertexCount = 0;
        u32 jointOffset = 0;
        u32 deformedOffset = 0;
    };

    /// One skinned mesh-instance the TLAS references via its own per-frame refit BLAS. The
    /// deformed vertices are already in WORLD space (the joint palette is worldBone *
    /// inverseBind and the skin kernel omits the model matrix), so the TLAS transform for it
    /// is identity. deformedOffset is the instance's base vertex in the frame's deformed
    /// buffer (the exact value submitDrawList assigned), and mesh supplies the index stream.
    struct SkinnedRtInstance
    {
        u64 entity = 0;  // keys the grow-only per-instance refit BLAS (BUILD once, then UPDATE)
        u32 deformedOffset = 0;
        u32 vertexCount = 0;
        u32 indexCount = 0;
        Ref<GpuMesh> mesh;  // index buffer source for the BLAS geometry
    };

    // The scene's structured draw list for the frame: built by submitDrawList from the
    // DrawItems, consumed by the scene pass (shaded) and the depth pre-pass (depth only).
    struct SceneDrawList
    {
        glm::mat4 viewProj{ 1.0f };
        std::vector<DrawBatch> batches;
        // Per skinned mesh-instance: the compute work the skin pre-pass dispatches before
        // the scene pass reads the deformed buffer. Empty when no skinned instances exist.
        std::vector<SkinDispatch> skinDispatches;
        // The parallel dispatches that deform the PREVIOUS pose into the prev-deformed buffer
        // (previous palette + previous-deformed output), read only by the motion pass for TAA.
        std::vector<SkinDispatch> prevSkinDispatches;
        // Per skinned instance: the entity + deformed offset the RT refit BLAS reads (carries the
        // exact deformedVertexOffset submitDrawList assigned). Empty unless RT consumes the scene.
        std::vector<SkinnedRtInstance> skinnedRtInstances;
        std::vector<Ref<GpuTexture>> liveTextures;  // pins indexed textures for the frame
        vk::DescriptorSet lightSet;
        vk::DescriptorSet instanceSet;
        bool valid = false;
    };

    // How much per-frame instrumentation the GPU profiler captures. `Off` (the default)
    // keeps the present-only host at zero added cost; the deeper levels are opt-in over
    // the control plane (profiler.set-mode).
    enum class ProfilerMode
    {
        Off,            ///< no queries, no VMA budget read — today's baseline cost
        Timestamps,     ///< per-pass GPU timestamps + throughput counters + VMA budget
        PipelineStats,  ///< Timestamps plus per-pass pipeline-statistics (deepest, heaviest)
    };

    // Raw pipeline-statistics counts for one pass (VK_QUERY_TYPE_PIPELINE_STATISTICS), recorded
    // only in PipelineStats mode. The consumer derives the optimization ratios: overdraw
    // (fragmentInvocations / pixels), culling efficiency (clippingPrimitives /
    // clippingInvocations), and vertex reuse (vertexInvocations / inputVertices);
    // computeInvocations sizes the GI/lighting compute passes.
    struct PipelineStats
    {
        u64 inputVertices = 0;
        u64 vertexInvocations = 0;
        u64 clippingInvocations = 0;
        u64 clippingPrimitives = 0;
        u64 fragmentInvocations = 0;
        u64 computeInvocations = 0;
        u64 pixels = 0;  // render-area pixels for the overdraw ratio (0 for compute / no query)
    };

    // One GPU scope's measured time for a frame, plus its place in the scope tree. `gpuMs`
    // is the wall-clock span between the scope's begin/end timestamps — relative, since
    // sibling scopes can overlap on the GPU, so the per-scope numbers do not cleanly sum to
    // the frame total. `startNs`/`endNs` are frame-relative (from the earliest begin), so a
    // consumer can lay the scopes onto one timeline; `parentIndex`/`depth` give the tree.
    // `stats` is populated only for top-level passes captured in PipelineStats mode.
    struct PassTiming
    {
        std::string name;
        f32 gpuMs = 0.0f;
        u64 startNs = 0;       // frame-relative begin (GPU clock, ticks→ns)
        u64 endNs = 0;         // frame-relative end
        i32 parentIndex = -1;  // enclosing scope's index in this list, or -1 at top level
        u32 depth = 0;
        bool hasStats = false;
        PipelineStats stats;
    };

    // Per-frame scene draw counters + CPU/GPU frame breakdown, refreshed each frame and
    // inspectable via the control plane so batching and the CPU-vs-GPU split can be
    // verified live. The GPU/VRAM fields are only populated when the profiler is enabled.
    struct RenderStats
    {
        u32 drawCalls = 0;         // drawIndexed calls (one per submesh per batch)
        u32 batches = 0;           // distinct (mesh, texture) buckets
        u32 instances = 0;         // total entities drawn
        u32 triangles = 0;         // triangles submitted (sum indexCount/3 over instances)
        u32 descriptorBinds = 0;   // descriptor-set binds recorded in the scene pass
        u32 commandBuffers = 0;    // primary command buffers submitted this frame
        u32 queueSubmits = 0;      // vkQueueSubmit2 calls this frame
        u32 pipelinesCreated = 0;  // PSOs compiled this frame (non-zero = a PSO-compile hitch)
        f32 frameMs = 0.0f;        // smoothed frame-to-frame wall-clock time
        f32 fps = 0.0f;            // derived from frameMs
        f32 gpuMs = 0.0f;          // smoothed GPU frame time (timestamp span); 0 until profiled
        f32 cpuFrameMs = 0.0f;     // smoothed render-thread CPU busy time (excludes blocking waits)
        f32 cpuWaitMs = 0.0f;      // smoothed time blocked on fences (high ⇒ GPU-bound)
        u64 vramUsageBytes = 0;    // device-local heap usage (VMA budget); 0 until profiled
        u64 vramBudgetBytes = 0;   // device-local heap budget
        bool softwareGpu = false;  // llvmpipe/lavapipe: GPU timings are CPU rasterization time
        ProfilerMode profilerMode = ProfilerMode::Off;
    };

    // The GPU profiler's per-frame-in-flight timestamp pools and the last completed
    // read-back. Each frame writes begin/end timestamps around every graph pass; the pool
    // is read back MaxFramesInFlight frames later (after that slot's fence has signalled),
    // so the read is always non-blocking.
    // VK_EXT_calibrated_timestamps state: the offset that projects a GPU tick onto the CPU
    // steady_clock axis (hostNs = deviceTick * timestampPeriod + deviceToHostNsOffset),
    // re-sampled periodically to track clock drift. `correlated` is false until a sample lands
    // (or stays false forever when the extension/host domain is absent) — the read-back then
    // keeps GPU spans on their own frame-relative axis.
    struct GpuCalibration
    {
        bool available = false;        // extension present + a host domain matching steady_clock
        VkTimeDomainEXT hostDomain{};  // the host clock domain (CLOCK_MONOTONIC = steady_clock)
        i64 deviceToHostNsOffset = 0;  // additive ns offset, device-ns -> host-ns
        u64 maxDeviationNs = 0;        // sample confidence; larger = looser correlation
        bool correlated = false;       // a valid offset has been sampled this session
        u64 lastCalibratedSerial = 0;  // frameSerial of the last (re)calibration
    };

    struct GpuProfiler
    {
        ProfilerMode mode = ProfilerMode::Off;
        bool poolsReady = false;
        bool subScopes = false;  // opt-in: instrument pass interiors as nested sub-scopes
        GpuCalibration calibration;
        std::array<vk::QueryPool, MaxFramesInFlight> timestampPools{};
        // Pipeline-statistics pools (parallel to the timestamp pools), allocated only when
        // pipelineStatsSupported. One query slot per top-level pass; read back in the same slot.
        std::array<vk::QueryPool, MaxFramesInFlight> statsPools{};
        // Scope records written into slot i this cycle, consumed when slot i is read back.
        std::array<std::vector<ScopeRecord>, MaxFramesInFlight> recordedScopes{};
        RgTimestamps scopeRecorder;           // per-frame recorder; pool/records rebound each frame
        std::vector<PassTiming> lastTimings;  // last completed read-back (the scope tree, flat)
        f32 lastGpuTotalMs = 0.0f;            // raw span of the last read-back
        f32 timestampPeriod = 1.0f;           // ns per timestamp tick (device limit)
        u64 timestampMask = ~0ull;            // graphics-queue timestampValidBits mask
        bool timestampsSupported = false;     // validBits != 0 && timestampComputeAndGraphics
        bool pipelineStatsSupported = false;  // pipelineStatisticsQuery device feature present
    };

    // The CPU side of the profiler: a persistent name registry plus one span buffer per
    // frame-in-flight, mirroring GpuProfiler's slot discipline. CpuScope records into the
    // current slot while a profiler mode is active; the spans are plain host memory, read at
    // end of frame with no GPU deferral. Phase 4 maps these onto the GPU timestamp axis.
    struct CpuProfiler
    {
        CpuMarkerRegistry registry;
        std::array<CpuSpanBuffer, MaxFramesInFlight> buffers{};
    };

    // The merged span record the whole profiler stack speaks: a CPU phase/pass span (Phase 2)
    // or a GPU scope (Phase 3) projected onto the CPU axis (Phase 4). Kept flat-and-tagged;
    // `parentIndex`/`depth` decode the tree, `lane` separates the two timelines. Stored as a
    // capture's `spans[]`, serialized to Chrome-Trace / the editor's ProfileCaptureDto.
    enum class ProfileLane
    {
        Cpu,
        Gpu,
    };

    struct ProfileSpan
    {
        std::string name;
        ProfileLane lane = ProfileLane::Cpu;
        u64 startNs = 0;
        u64 endNs = 0;
        i32 parentIndex = -1;  ///< enclosing span index in this capture, or -1 at top level
        u32 depth = 0;
        bool hasStats = false;  ///< pipeline-statistics present (PipelineStats-mode GPU passes)
        PipelineStats stats;
    };

    // Self-documenting capture metadata: the honesty flags (softwareGpu/correlated) plus the
    // device + clock facts a downloaded trace needs to be interpreted on its own.
    struct ProfileCaptureMeta
    {
        bool softwareGpu = false;
        bool correlated = false;
        std::string deviceName;
        f32 timestampPeriod = 1.0f;
        f32 targetFps = 60.0f;
        ProfilerMode mode = ProfilerMode::Off;
        std::string filter;
        u32 frameCount = 0;
    };

    struct ProfileCapture
    {
        std::vector<ProfileSpan> spans;
        ProfileCaptureMeta meta;
    };

    // Bounded capture: single frame (the default snapshot), a fixed N-frame window, or rolling
    // (a recent window; v1 records it forward like frames). Hard-capped so the span buffer
    // cannot OOM.
    enum class CaptureMode
    {
        Single,
        Frames,
        Rolling,
    };

    inline constexpr u32 MaxCaptureFrames = 256;

    // The capture state machine driven by profiler.capture-start/stop. `Arming` warms up for the
    // GPU read-back delay so every recorded frame reflects the arm-time settings; `Recording`
    // copies each finalized frame's merged spans; `Ready` holds the result until it is drained.
    enum class CaptureState
    {
        Idle,
        Arming,
        Recording,
        Ready,
    };

    struct CaptureRecorder
    {
        CaptureState state = CaptureState::Idle;
        CaptureMode mode = CaptureMode::Single;
        u32 targetFrames = 1;                        // frames to record before going Ready
        u32 capturedFrames = 0;                      // frames copied so far
        u32 warmup = 0;                              // Arming frames left (covers the read-back delay)
        std::string filter;                          // pass-name prefix, carried into metadata (a view hint)
        bool includeCpu = true;                      // include the CPU lane
        bool includeStats = false;                   // request PipelineStats mode (if supported)
        ProfilerMode priorMode = ProfilerMode::Off;  // restored on stop
        bool priorSubScopes = false;                 // restored on stop
        u32 captureId = 0;                           // id of the in-flight / last capture
        u32 nextCaptureId = 1;
        ProfileCapture capture;  // accumulates while Recording
    };

    // Frames kept in the rolling history ring (≈8–17 s at 60–120 Hz). Percentiles and
    // stutter live in the per-frame distribution, so the engine records every frame.
    inline constexpr u32 FrameHistoryCapacity = 1024;

    // One frame's raw (un-smoothed) timing, pushed once per frame at endFrame. The frame
    // time used for percentiles + stutter is cpuMs + cpuWaitMs (the render-thread wall
    // clock: work plus the fence wait, which absorbs GPU-bound stalls). gpuMs is 0 unless
    // the profiler is enabled; the history itself is always recorded.
    struct FrameSample
    {
        u64 frameIndex = 0;  // absolute, monotonic; lets the editor accumulate a long history
        f32 cpuMs = 0.0f;
        f32 gpuMs = 0.0f;
        f32 cpuWaitMs = 0.0f;
    };

    // Percentile / consistency summary computed on demand over the ring. The p99 frame
    // time is the 1%-low; average FPS is deliberately absent (it hides hitches).
    struct FrameHistoryStats
    {
        f32 p50Ms = 0.0f;
        f32 p95Ms = 0.0f;
        f32 p99Ms = 0.0f;
        f32 p999Ms = 0.0f;
        f32 maxMs = 0.0f;
        f32 meanMs = 0.0f;
        f32 stddevMs = 0.0f;
        u64 stutterCount = 0;
        u32 sampleCount = 0;
    };

    // The single source of truth for green/amber/red, shared over the wire so the engine,
    // the editor HUD, and e2e tests all agree. budget = 1000 / targetFps. A frame is over
    // budget (a dropped frame) past 1.0× budget; the multipliers grade it against the
    // running median; frozenMs is a hard-hitch floor that is always red.
    struct PerfConfig
    {
        f32 targetFps = 60.0f;       // 30/60/90/120 + custom; budget derives from this
        f32 greenBudgetFrac = 0.8f;  // < 0.8×budget (with the median check) = green
        f32 greenMedianMul = 1.5f;   // < 1.5×median = consistent
        f32 amberMedianMul = 2.0f;   // 1.5–2×median = amber; beyond = red
        f32 frozenMs = 250.0f;       // a hard hitch → always red
        f32 vramWarnFrac = 0.8f;     // 80% of the VRAM budget = warn
        f32 vramCritFrac = 0.95f;    // 95% = critical (≥100% = over)
    };

    // Capacity of the alarm event ring (FIRING/RESOLVED history). At the editor's ~6 Hz
    // drain this is ample headroom; older events fall off and the cursor reports overflow.
    inline constexpr u32 AlarmEventRingCapacity = 256;

    enum class AlarmSeverity
    {
        Info,      ///< log only (a single PSO-compile hitch, a TAA reset)
        Warning,   ///< throttled toast + highlight the offending row
        Critical,  ///< persistent log entry + active-alarms badge
    };

    enum class AlarmEventKind
    {
        Firing,
        Resolved,
    };

    // A currently-firing alarm, keyed by fingerprint = hash(metric + "|" + pass) so a
    // repeated breach coalesces into one entry (count/peak update in place) instead of
    // spamming. Severity escalates in place (warning → critical).
    struct ActiveAlarm
    {
        u64 fingerprint = 0;
        std::string metric;  // "frame-budget", "frame-hitch", "burn-rate", "vram", "pso-compile"
        std::string pass;    // the offending pass, empty for whole-frame alarms
        AlarmSeverity severity = AlarmSeverity::Warning;
        f32 value = 0.0f;      // current breached value (ms for time metrics, % for vram/burn)
        f32 threshold = 0.0f;  // the threshold it crossed, same units
        f32 peak = 0.0f;       // worst value seen while active
        u64 sinceFrame = 0;
        u64 sinceNs = 0;
        u64 lastSeenNs = 0;
        u32 count = 0;  // times re-observed while active
    };

    // An append-only, seq-stamped FIRING/RESOLVED event drained over a non-blocking cursor.
    struct AlarmEvent
    {
        u64 seq = 0;
        u64 fingerprint = 0;
        std::string metric;
        std::string pass;
        AlarmSeverity severity = AlarmSeverity::Warning;
        AlarmEventKind kind = AlarmEventKind::Firing;
        f32 value = 0.0f;
        f32 threshold = 0.0f;
        u64 sinceFrame = 0;
        u32 count = 0;
        f32 durationMs = 0.0f;  // wall-clock duration (RESOLVED only)
    };

    // The snapshot drainAlarms returns: events with seq > since, the high-water seq, the
    // oldest still-retained seq, and whether the ring dropped events past `since`.
    struct AlarmDrain
    {
        std::vector<AlarmEvent> events;
        u64 highWaterSeq = 0;
        u64 oldestSeq = 0;
        bool overflowed = false;
    };

    // The engine-side alarm engine: the active set, the seq-stamped event ring, and the
    // per-detector smoothing/debounce state advanced once per frame in endFrame.
    struct AlarmState
    {
        std::vector<ActiveAlarm> active;
        std::array<AlarmEvent, AlarmEventRingCapacity> events{};
        u32 eventHead = 0;
        u32 eventCount = 0;
        u64 nextSeq = 1;               // seq to assign to the next event
        u64 frameCounter = 0;          // monotonic frame index for sinceFrame
        f32 emaFrameMs = 0.0f;         // tau≈300 ms smoothed frame time (the sustained gate)
        f32 budgetWarnHeldSec = 0.0f;  // debounce: time the smoothed frame time held over enter
        f32 budgetCritHeldSec = 0.0f;  // time it held over the critical threshold
        u32 hitchClearFrames = 0;      // clean frames since the last spike (to auto-resolve)
    };

    // A single screen-space overlay vertex: NDC position + flat color. The editor
    // overlay (gizmo handles + entity billboards) builds a triangle list of these.
    // `edge` drives analytic edge anti-aliasing (the overlay draws post-resolve, so
    // scene MSAA can't touch it): xy are signed coordinates per feather direction
    // (±1 at the nominal edge) and zw the matching half-extents in pixels; a
    // non-positive half-extent disables that direction. Lines feather one direction
    // (across the thickness), filled quads feather both.
    struct OverlayVertex
    {
        glm::vec2 position;  // clip-space NDC ([-1,1])
        glm::vec4 color;
        glm::vec4 edge{ 0.0f, 0.0f, 0.0f, 0.0f };
        f32 depth = 0.0f;  // NDC z ([0,1]); 0 = on the near plane (on top). Only the
                           // depth-tested range uses it; on-top geometry leaves it 0.
    };

    // Per-frame editor overlay geometry, drawn into the post-tonemap scene color so it
    // composites under present-only mode. Buffers grow on demand. The list is laid out
    // depth-tested range first ([0, depthTestedCount)) then the always-on-top range, so the
    // overlay pass can draw each range with its own pipeline from one buffer.
    struct OverlayState
    {
        std::vector<OverlayVertex> vertices;
        u32 depthTestedCount = 0;
        std::array<Ref<Buffer>, MaxFramesInFlight> buffers;
        std::array<u32, MaxFramesInFlight> capacity{};
    };

    // The Vulkan device + allocator the whole renderer borrows from.
    // KHR acceleration-structure / ray-query entry points are NOT statically exported by
    // the system loader (only core funcs are). When RT is available we resolve them via
    // vkGetDeviceProcAddr and call through the C API — the engine otherwise uses Vulkan-Hpp
    // static dispatch, so these few extension calls stay manual.
    struct RtDispatch
    {
        PFN_vkGetAccelerationStructureBuildSizesKHR getBuildSizes = nullptr;
        PFN_vkCreateAccelerationStructureKHR createAccel = nullptr;
        PFN_vkDestroyAccelerationStructureKHR destroyAccel = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR cmdBuild = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR getAccelAddress = nullptr;
    };

    // VK_EXT_calibrated_timestamps entry points, resolved when the device extension is present.
    // Null => the profiler cannot correlate GPU spans onto the CPU clock (graceful fallback).
    struct CalibratedTimestampsDispatch
    {
        PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT getDomains = nullptr;
        PFN_vkGetCalibratedTimestampsEXT getTimestamps = nullptr;
    };

    struct VulkanContext
    {
        vkb::Instance vkbInstance;  // vk-bootstrap keeps the bits we need for clean teardown
        vkb::Device vkbDevice;
        vk::Instance instance;
        vk::SurfaceKHR surface;
        vk::PhysicalDevice physicalDevice;
        vk::Device device;
        vk::Queue graphicsQueue;
        u32 graphicsQueueFamily = 0;
        VmaAllocator allocator = nullptr;
        bool rtSupported = false;  // KHR acceleration_structure + ray_query present + enabled
        RtDispatch rt;
        RgDebugLabels debugLabels;                  // VK_EXT_debug_utils pass markers; null when the ext is absent
        CalibratedTimestampsDispatch calibratedTs;  // VK_EXT_calibrated_timestamps; null when absent
    };

    // The surface swapchain + its per-image sync, recreated as a unit on resize.
    struct Swapchain
    {
        vk::SwapchainKHR handle;
        vk::Format format = vk::Format::eUndefined;
        vk::Extent2D extent;
        bool captureSupported = false;  // surface allows TRANSFER_SRC (window screenshots)
        std::vector<vk::Image> images;
        std::vector<vk::ImageView> imageViews;
        std::vector<vk::Semaphore> renderFinished;  // one per swapchain image
        std::vector<vk::Fence> imagesInFlight;      // borrowed per-frame fence per image
    };

    // Per-frame ring + the frame-scoped geometry beginFrame resets / endFrame consumes.
    struct FrameSync
    {
        std::array<FrameData, MaxFramesInFlight> frames{};
        u32 index = 0;
        u32 imageIndex = 0;
        std::array<f32, 4> clearColor{ 0.05f, 0.06f, 0.08f, 1.0f };
        SceneDrawList sceneDrawList;             // structured scene geometry for the frame
        std::vector<RenderFn> sceneSubmissions;  // ad-hoc geometry replayed into the offscreen pass
        std::vector<RenderFn> uiSubmissions;     // replayed into the swapchain pass
    };

    // Pipelined publication of the offscreen viewport into POSIX shared memory for the
    // editor's compositor-side presenter. Per frame-in-flight slot: a BGRA8 image the GPU
    // blits into (format conversion) and a persistently mapped staging buffer the CPU
    // memcpys from once the slot's fence has signalled — no waitIdle anywhere on the path.
    struct ShmPublishSlot
    {
        vk::Image image;
        VmaAllocation imageAlloc = nullptr;
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation bufferAlloc = nullptr;
        VmaAllocationInfo bufferInfo{};
        u32 width = 0;
        u32 height = 0;
        bool valid = false;  // a recorded readback is in flight / completed for this slot
    };

    struct ShmPublish
    {
        bool enabled = false;
        std::string name;
        std::array<ShmPublishSlot, MaxFramesInFlight> slots{};
        // Grow-only mapped segment: a 32-byte header [magic, width, height, seq, ringSlots,
        // slotCapacity, 0, 0] followed by ringSlots fixed-capacity BGRA8 frames. Frame with
        // sequence s lands in ring slot s % ringSlots, so a compositor can keep reading an
        // older slot while the engine writes the next.
        int fd = -1;
        unsigned char* base = nullptr;
        std::size_t mappedSize = 0;
        std::size_t slotCapacity = 0;
        u32 seq = 0;
    };

    // Descriptor infrastructure built once: layouts, pools, the bindless set, and the
    // post-process compute sets. set 0 = bindless image array, 1 = light UBO, 2 = instances.
    struct Descriptors
    {
        vk::Sampler linearSampler;
        vk::Sampler shadowSampler;                  // depth-compare sampler (PCF) for the shadow map
        vk::DescriptorSetLayout bindlessSetLayout;  // set 0: bindless combined-image-sampler array
        vk::DescriptorSetLayout lightSetLayout;     // set 1: directional light UBO
        vk::DescriptorSetLayout instanceSetLayout;  // set 2: per-instance storage buffer
        vk::DescriptorPool descriptorPool;          // eFreeDescriptorSet (texture sets freed on Ref drop)
        // Bindless: one global texture array bound once; uploadTexture writes a stable
        // slot (update-after-bind) and returns its index. The default white is slot 0.
        vk::DescriptorPool bindlessPool;  // eUpdateAfterBindPool
        vk::DescriptorSet bindlessSet;    // the single set 0 for all draws
        u32 nextBindlessIndex = 0;
        // Reclaimed bindless slots, returned when a GpuTexture is destroyed. Shared (Ref) with every
        // texture so it outlives both the descriptors and any texture whose dtor pushes to it; an upload
        // pops a freed slot before growing nextBindlessIndex, so a churny scene keeps the pool bounded.
        Ref<std::vector<u32>> bindlessFreeList = std::make_shared<std::vector<u32>>();
        // The post-process compute set LAYOUTS are device-shared; the SETS that bind per-view
        // images (offscreen/scratch/history/motion) live per-view in ViewTargets.
        vk::DescriptorSetLayout tonemapSetLayout;  // compute set 0: storage image (offscreen)
        vk::DescriptorSetLayout fxaaSetLayout;
        // TAA resolve set (compute): current + history + motion samplers, offscreen + history storage.
        // Two sets per view (one per ping-pong parity) rewritten when that view's targets change.
        vk::DescriptorSetLayout taaSetLayout;
        vk::DescriptorSetLayout clusterSetLayout;  // compute set 0
    };

    // Directional + punctual lights and the clustered-forward froxel apparatus.
    struct Lighting
    {
        // Per-frame light UBO + set (one per in-flight frame), so the host write in
        // setDirectionalLight never races a frame still reading the light on the GPU.
        std::array<vk::DescriptorSet, MaxFramesInFlight> lightSets;
        std::array<VkBuffer, MaxFramesInFlight> lightBuffers{};
        std::array<VmaAllocation, MaxFramesInFlight> lightAllocs{};
        std::array<void*, MaxFramesInFlight> lightMapped{};
        // Per-frame punctual-light storage buffer (set 1, binding 1), grown on demand.
        std::array<Ref<Buffer>, MaxFramesInFlight> lightListBuffers;
        std::array<u32, MaxFramesInFlight> lightListCapacity{};
        std::array<vk::DescriptorSet, MaxFramesInFlight> clusterSets;   // compute
        std::array<Ref<Buffer>, MaxFramesInFlight> clusterBuffers;      // per-cluster count + indices
        std::array<VkBuffer, MaxFramesInFlight> clusterParamBuffers{};  // cluster params UBO
        std::array<VmaAllocation, MaxFramesInFlight> clusterParamAllocs{};
        std::array<void*, MaxFramesInFlight> clusterParamMapped{};
        bool useClustered = true;  // false = fragment loops all lights (reference)
        u32 frameLightCount = 0;   // punctual lights uploaded this frame
        u32 frameProbeCount = 0;   // reflection probes sampled this frame (encoded in ambientColor.w)
        bool clusterDispatchPending = false;
        // Directional shadow: the light-space transform written into the light UBO + the
        // shadow pass push constant. shadowPending arms the depth pass for the frame.
        bool useShadows = true;      // master toggle (se set-shadows)
        bool shadowPending = false;  // a shadow-casting directional light is present this frame
        glm::mat4 shadowViewProj{ 1.0f };
        // Spot shadow: the first shadow-casting spot light gets a dedicated depth map. Its
        // perspective light-space transform + its index in the per-frame light list.
        bool spotShadowPending = false;
        glm::mat4 spotShadowViewProj{ 1.0f };
        u32 spotShadowLightIndex = 0;
        // Point shadow: the first point light gets an omnidirectional distance cubemap. Its
        // world position + far plane + index in the per-frame light list.
        bool pointShadowPending = false;
        glm::vec3 pointShadowPos{ 0.0f };
        f32 pointShadowFar = 1.0f;
        u32 pointShadowLightIndex = 0;
    };

    // Per-frame instance storage buffer + set. Grown on demand (never shrunk);
    // capacity is in InstanceData elements. The joint palette (set 2, binding 1) is
    // the skinning matrices for the frame's skinned draws; created only when one exists.
    struct Instancing
    {
        std::array<Ref<Buffer>, MaxFramesInFlight> buffers;
        std::array<vk::DescriptorSet, MaxFramesInFlight> sets;
        std::array<u32, MaxFramesInFlight> capacity{};
        // Per-frame deduplicated material params (set 2, binding 2): one MaterialParamsData
        // entry per distinct material this frame, indexed by InstanceData.texture.w. Grown
        // on demand like the instance buffer.
        std::array<Ref<Buffer>, MaxFramesInFlight> materialBuffers;
        std::array<u32, MaxFramesInFlight> materialCapacity{};
        std::array<Ref<Buffer>, MaxFramesInFlight> jointBuffers;
        std::array<u32, MaxFramesInFlight> jointCapacity{};
        // The previous frame's joint palette, slotted at the same jointOffset as the current
        // palette so the skin pre-pass deforms the prior pose into the prev-deformed buffer.
        // Grown like jointBuffers; only the motion pass consumes the result.
        std::array<Ref<Buffer>, MaxFramesInFlight> prevJointBuffers;
        std::array<u32, MaxFramesInFlight> prevJointCapacity{};
    };

    /// The compute skinning pre-pass apparatus: a per-frame-in-flight deformed-vertex
    /// buffer (base 32-byte Vertex layout, STORAGE|VERTEX, grow-only) every skinned
    /// instance deforms into, plus the descriptor pool the per-instance dispatch sets are
    /// allocated from (reset each frame). The scene pass then draws skinned meshes as
    /// ordinary static meshes reading this buffer.
    struct Skinning
    {
        vk::DescriptorSetLayout setLayout;
        std::array<vk::DescriptorPool, MaxFramesInFlight> pools{};
        std::array<Ref<Buffer>, MaxFramesInFlight> deformedBuffers;
        std::array<u32, MaxFramesInFlight> deformedCapacity{};  // in Vertex (32 B) elements
        // The previous frame's deformed vertices, laid out identically to deformedBuffers (same
        // deformedVertexOffset per instance). The skin pre-pass writes it from the previous
        // palette; the motion pass reads it as the prev-position vertex stream for TAA.
        std::array<Ref<Buffer>, MaxFramesInFlight> prevDeformedBuffers;
        std::array<u32, MaxFramesInFlight> prevDeformedCapacity{};
        u32 peakVertices = 0;
        // CPU-side cross-frame caches keyed by entity uuid: last frame's joint palette slice
        // (deformation motion) + last frame's world matrix (object motion). A new entity reads
        // back current == previous, so its first frame emits zero motion (no velocity flash).
        std::unordered_map<u64, std::vector<glm::mat4>> prevPaletteByEntity;
        std::unordered_map<u64, glm::mat4> prevModelByEntity;
    };

    // Renderer-owned pipelines + the mesh PSO cache (built on demand, keyed by variant;
    // the uebershader maps many materials to one PSO, permutations add cache entries).
    struct Pipelines
    {
        Ref<Pipeline> thumbnail;      // lazy mesh-thumbnail graphics pipeline
        Ref<Pipeline> preview;        // lazy material-preview graphics pipeline (sphere + studio light)
        Ref<Pipeline> tonemap;        // in-place compute tonemap (post-process)
        Ref<Pipeline> depthPrepass;   // vertex-only depth pre-pass
        Ref<Pipeline> shadowDepth;    // vertex-only depth pass into the shadow map (depth-biased)
        Ref<Pipeline> pointShadow;    // color (distance) + depth pass into a point shadow cube face
        Ref<Pipeline> gbuffer;        // thin G-buffer prepass (view normal + view-Z)
        Ref<Pipeline> gtao;           // compute screen-space AO from the G-buffer
        Ref<Pipeline> aoBlur;         // bilateral denoise of the raw AO
        Ref<Pipeline> contact;        // screen-space contact shadows (directional)
        Ref<Pipeline> ssgi;           // screen-space one-bounce GI
        Ref<Pipeline> copyColor;      // capture linear-HDR scene color into prevColor
        Ref<Pipeline> motion;         // motion-vector prepass (camera reprojection)
        Ref<Pipeline> taa;            // compute TAA resolve
        Ref<Pipeline> ddgiVoxelize;   // build the voxel scene proxy
        Ref<Pipeline> ddgiTrace;      // software probe ray trace
        Ref<Pipeline> ddgiBlendIrr;   // blend rays -> irradiance atlas
        Ref<Pipeline> ddgiBlendDist;  // blend rays -> moment atlas
        Ref<Pipeline> ddgiBorder;     // octahedral gutter copy
        Ref<Pipeline> restirInitial;  // ReSTIR initial candidate sampling
        Ref<Pipeline> restirReuse;    // ReSTIR temporal + spatial reuse
        Ref<Pipeline> restirResolve;  // ReSTIR resolve (1 shadow ray) + shade
        Ref<Pipeline> fxaa;           // compute FXAA post-process
        Ref<Pipeline> cull;           // compute light-cull (clustered forward)
        Ref<Pipeline> skin;           // compute skinning pre-pass (deforms skinned meshes once)
        Ref<Pipeline> overlay;        // screen-space editor overlay (gizmo + billboards), no depth test
        Ref<Pipeline> overlayDepth;   // overlay variant that depth-tests against the scene depth
        std::unordered_map<std::string, Ref<Pipeline>> cache;
    };

    // The per-view offscreen render targets: every viewport-sized image plus the per-view
    // sizing/history state. One instance per editor pane (indexed by ViewId); the scene-global
    // and device-global state lives in Targets.
    struct ViewTargets
    {
        Image offscreen;  // scene render target shown in the Viewport panel
        Image depth;      // depth buffer for the scene pass, sized to the viewport
        // Screen-space effects: a thin G-buffer (view normal rgb + view-Z in .a) + its depth
        // scratch; the raw + denoised AO (r8), the contact-shadow map (r8), the SSGI radiance
        // (rgba16f), and the persistent previous-frame linear-HDR color SSGI gathers from.
        // All viewport-sized, recreated with it.
        Image gNormal;
        Image gDepth;
        Image aoRaw;
        Image aoMap;
        Image contactMap;
        Image ssgiMap;
        Image prevColor;
        // TAA: a screen-space motion-vector target (rg16f) + its depth scratch, and two
        // ping-pong history color images. Sized to the viewport, recreated with it.
        Image motion;
        Image motionDepth;
        std::array<Image, 2> history;
        u32 historyIndex = 0;       // this frame writes history[historyIndex], reads the other
        bool historyValid = false;  // false on the first frame / after a resize
        // MSAA: when sampleCount > 1 the scene renders to these multisampled targets and
        // resolves color into offscreen. Sized to the viewport, recreated with it.
        Image msaaColor;
        Image msaaDepth;
        // FXAA: when on, the scene renders to scratch (1x sampled), and a compute pass
        // edge-blurs it into offscreen.
        Image scratch;
        // The descriptor sets that bind this view's screenspace + post-process images. Per-view
        // (allocated once per view, written when this view's images are (re)created) so a view switch
        // never leaves a set bound to another view's images — the shared LAYOUTS/samplers/pipelines stay
        // device-global in Ssao/Descriptors. The screen-space compute chain (set into Ssao's layouts):
        vk::DescriptorSet gtaoSet;       // gNormal + aoRaw            (compute2)
        vk::DescriptorSet aoBlurSet;     // aoRaw + gNormal + aoMap    (compute3)
        vk::DescriptorSet contactSet;    // gNormal + contactMap       (compute2)
        vk::DescriptorSet ssgiSet;       // gNormal + prevColor + ssgiMap (compute3)
        vk::DescriptorSet copyColorSet;  // offscreen + prevColor      (compute2)
        vk::DescriptorSet meshSet;       // set 4: aoMap + contactMap + ssgiMap (sampled by the mesh pass)
        // Post-process compute sets (Descriptors' layouts): tonemap (offscreen), FXAA (scratch+offscreen),
        // and the two TAA ping-pong-parity sets (scratch/history/motion/offscreen).
        vk::DescriptorSet tonemapSet;
        vk::DescriptorSet fxaaSet;
        std::array<vk::DescriptorSet, 2> taaSets;
        // Last frame's camera viewProj, for TAA motion vectors; invalid until the first frame
        // stores one. Per-view so a re-activated view reprojects against ITS own last frame.
        glm::mat4 prevViewProj{ 1.0f };
        bool prevViewProjValid = false;
        u32 desiredWidth = 0;  // requested by the UI panel (applied next frame)
        u32 desiredHeight = 0;
        u32 generation = 0;  // bumped whenever the offscreen image is recreated
    };

    // The scene-global + device-global render state shared across all views: the shadow maps
    // (the scene has one light rig) and the AA capability/toggle state (a device fact + a
    // session toggle). The per-view, viewport-sized images live in ViewTargets.
    struct Targets
    {
        Image shadowMap;      // directional-light depth map (sampled with the compare sampler)
        Image spotShadowMap;  // first shadow-casting spot light's depth map (same compare sampler)
        // First shadow-casting point light's omnidirectional distance cubemap: a color cube
        // (R32_SFLOAT = world distance to the nearest occluder) rendered face-by-face with a
        // shared depth scratch; the mesh samples it by direction and compares linear distance.
        Image pointShadowCube;
        Image pointShadowDepth;
        std::array<vk::ImageView, 6> pointShadowFaces{};                       // per-face render views (freed manually)
        vk::SampleCountFlagBits sampleCount = vk::SampleCountFlagBits::e1;     // 1 = MSAA off
        vk::SampleCountFlagBits maxSampleCount = vk::SampleCountFlagBits::e1;  // device cap
        vk::SampleCountFlags supportedSampleCounts =
            vk::SampleCountFlagBits::e1;  // counts the color+depth MSAA formats accept
        bool fxaaEnabled = false;         // FXAA post-process (mutually exclusive with MSAA)
        bool taaEnabled = false;          // TAA resolve (mutually exclusive with MSAA/FXAA)
    };

    // Which shader fills the IBL environment cube before the convolution chain.
    enum class EnvSource
    {
        Procedural,  // ibl_skygen.slang from SkygenParams (default)
        Equirect,    // ibl_equirect.slang projecting a user panorama
        Atmosphere,  // atmos_* LUT chain into atmos_skygen (Hillaire 2020)
    };

    // Renderer-side mirror of Scene's AtmosphereSettings (the renderer does not import
    // Saffron.Scene). A plain aggregate, compared memberwise to gate the re-bake. Carried on
    // SkygenParams so the existing pending/baked round-trip plumbing moves it through unchanged.
    struct AtmosphereParams
    {
        bool enabled = false;
        f32 planetRadius = 6360.0f;
        f32 atmosphereHeight = 100.0f;
        glm::vec3 rayleighScattering{ 5.802f, 13.558f, 33.1f };
        f32 rayleighScaleHeight = 8.0f;
        f32 mieScattering = 3.996f;
        f32 mieScaleHeight = 1.2f;
        f32 mieAnisotropy = 0.8f;
        glm::vec3 ozoneAbsorption{ 0.650f, 1.881f, 0.085f };
        f32 sunDiskAngularRadius = 0.00465f;
        f32 sunDiskIntensity = 20.0f;
    };

    // Inputs that drive the procedural-sky bake (ibl_skygen). The sun follows the scene's
    // directional light, so a re-bake re-tints the visible sky AND the IBL together. (Overall
    // sky intensity is applied by the visible-sky pass, not baked, to avoid double-counting.)
    struct SkygenParams
    {
        glm::vec3 sunDir{ 0.5f, 1.0f, 0.3f };  // direction TO the sun (= -lightDir); shader normalizes
        f32 sunIntensity = 1.0f;
        glm::vec3 sunColor{ 1.0f };
        AtmosphereParams atmosphere;  // physically based source params (enabled gates the LUT chain)
    };

    // A per-frame snapshot of one ReflectionProbeComponent, passed from renderScene into the
    // renderer without the renderer depending on Saffron.Scene (the same decoupling Sky.mode
    // uses). origin is the entity's world translation; dirty arms a (re)capture.
    struct ReflectionProbeUpload
    {
        u64 entity = 0;
        glm::vec3 origin{ 0.0f };
        f32 influenceRadius = 10.0f;
        f32 intensity = 1.0f;
        bool boxProjection = false;
        glm::vec3 boxExtent{ 10.0f };
        bool dirty = false;
    };

    // Image-based lighting: a procedural/HDR environment cubemap convolved into a diffuse
    // irradiance cube + a roughness-mipped prefiltered specular cube + a split-sum BRDF
    // LUT. Sampled as the mesh ambient (set 3). Baked at startup; re-baked on demand when the
    // sky inputs change (the directional light moves).
    struct Ibl
    {
        Image envCube;           // source environment (procedural sky)
        Image transmittanceLut;  // atmosphere: view-zenith x altitude extinction (rgba16f)
        Image multiScatterLut;   // atmosphere: isotropic multiple-scattering term (rgba16f)
        Image skyViewLut;        // atmosphere: azimuth x elevation in-scatter, horizon-densified
        Image irradianceCube;    // diffuse irradiance convolution
        Image prefilteredCube;   // GGX-prefiltered specular (one mip per roughness step)
        Image brdfLut;           // split-sum (scale, bias) table (rgba16f, RG used)
        u32 prefilterMips = 1;
        vk::Sampler sampler;                // linear, clamp, mipped — all three sampled in the mesh
        vk::DescriptorSetLayout setLayout;  // set 3 (irradiance, prefiltered, brdf LUT)
        vk::DescriptorSet set;
        bool ready = false;
        bool useIbl = true;                             // false = flat scalar ambient fallback
        SkygenParams bakedParams;                       // the params the current envCube was baked with
        SkygenParams pendingParams;                     // requested params (applied at the next beginFrameGraph)
        bool rebakePending = false;                     // pendingParams differ from bakedParams -> re-bake
        EnvSource source = EnvSource::Procedural;       // which shader fills envCube
        EnvSource bakedSource = EnvSource::Procedural;  // source the current envCube was baked with
        Ref<GpuTexture> envPanorama;                    // Equirect source (held alive across the bake)
    };

    inline constexpr u32 MaxReflectionProbes = 8;  // hard cap; excess probes ignored (logged once)

    // One captured + prefiltered local reflection probe. Mirrors the Ibl cube layout but
    // per-probe; baked on demand (the capture pass renders the scene into 6 faces, then the
    // shared ibl_irradiance/ibl_prefilter convolve into these). Sampled via the IBL set (set 3).
    struct ReflectionProbe
    {
        Image envCube;                             // captured local environment (newColorCubeImage)
        std::array<vk::ImageView, 6> faceViews{};  // per-face color attachment views
        Image envDepth;                            // transient per-face depth scratch
        Image irradianceCube;                      // diffuse irradiance convolution (per-probe)
        Image prefilteredCube;                     // GGX-prefiltered specular (per-probe)
        glm::vec3 origin{ 0.0f };
        f32 influenceRadius = 10.0f;
        f32 intensity = 1.0f;
        bool boxProjection = false;
        glm::vec3 boxExtent{ 10.0f };
        u64 entity = 0;          // owning entity id (capture re-uses the slot when re-armed)
        bool allocated = false;  // cubes created
        bool valid = false;      // captured + written into meshSet at least once
        bool dirty = false;      // (re)capture pending this frame
    };

    // The probe cube arrays + metadata SSBO (IBL set bindings 3-5), plus the per-frame capture state. Every
    // array slot is seeded with the global IBL cubes so the bind is always valid (unused
    // slots harmlessly resolve to the global env).
    struct ReflectionProbes
    {
        std::array<ReflectionProbe, MaxReflectionProbes> probes;
        u32 count = 0;              // active probe slots (<= MaxReflectionProbes)
        vk::DescriptorSet meshSet;  // the IBL set (set 3); probes live at bindings 3-5
        vk::Sampler sampler;        // linear, clamp, mipped — cube sampling in the mesh
        Ref<Buffer> metaBuffer;     // MaxReflectionProbes ProbeMeta records (origin/radius/...)
        bool useProbes = true;
        bool capturePending = false;  // any probe dirty this frame -> capture in beginFrameGraph
        bool warnedOverflow = false;  // logged once when probe count exceeds the cap
    };

    // Visible sky background, drawn by a fullscreen graphics pass before the scene pass.
    // Procedural mode samples the IBL envCube (so the background matches the lighting),
    // Texture mode samples a bindless equirectangular panorama, Color mode is a flat fill.
    // mode: 0 = Color, 1 = Texture, 2 = Procedural — matches Saffron.Scene SkyMode's values
    // (the renderer does not import Scene, so it carries the mode as a plain int).
    struct Sky
    {
        u32 mode = 2;
        glm::vec3 clearColor{ 0.05f, 0.06f, 0.08f };
        f32 intensity = 1.0f;
        f32 rotation = 0.0f;
        bool visible = true;
        u32 textureIndex = 0;               // bindless panorama slot (Texture mode)
        Ref<Pipeline> pipeline;             // fullscreen PSO; bakes the sample count, rebuilt on AA change
        vk::DescriptorSetLayout setLayout;  // set 1: envCube (combined image sampler)
        vk::DescriptorSet set;
        bool ready = false;  // set written + envCube baked
    };

    // Screen-space ambient occlusion (GTAO-lite). When on, a G-buffer prepass + a compute
    // pass produce an AO factor the mesh multiplies into the IBL/flat ambient term. The
    // descriptor sets/layouts are built once; the camera transforms are written per frame.
    // Screen-space effects driven off the thin G-buffer: ambient occlusion (GTAO +
    // bilateral denoise), directional contact shadows, and one-bounce SSGI. They share the
    // G-buffer + the scene's view/proj; the mesh samples their three maps via set 4.
    struct Ssao
    {
        bool useSsao = true;                       // GTAO ambient occlusion
        bool useContact = true;                    // screen-space contact shadows (directional)
        bool useSsgi = true;                       // screen-space one-bounce GI
        bool ready = false;                        // sets/views valid (built after targets exist)
        glm::mat4 view{ 1.0f };                    // world -> view (G-buffer prepass)
        glm::mat4 viewProj{ 1.0f };                // world -> clip (G-buffer prepass)
        glm::mat4 projection{ 1.0f };              // view -> clip (contact/SSGI reproject)
        glm::mat4 invProjection{ 1.0f };           // clip -> view (reconstruct view pos)
        glm::vec3 sunDirView{ 0.0f, 1.0f, 0.0f };  // direction TO the sun, view space (contact)
        f32 radius = 1.0f;
        f32 strength = 3.0f;
        vk::Sampler sampler;  // nearest, clamp — samples the G-buffer
        // Two compute set layouts shared by the screen-space passes: 2-binding
        // (sampler + storage) and 3-binding (sampler + sampler + storage). The SETS that bind
        // per-view images live in ViewTargets (one per view); these layouts are device-shared.
        vk::DescriptorSetLayout compute2Layout;
        vk::DescriptorSetLayout compute3Layout;
        vk::DescriptorSetLayout meshSetLayout;  // set 4 in the mesh pipeline (AO + contact + SSGI)
        u32 generation = 0;                     // bumped when targets recreate (sets refreshed)
    };

    // A VMA-allocated 3D image (the DDGI voxel scene proxy). Move-only like Image; a 3D
    // view + storage usage. Kept separate from Image (which is 2D-only) per the roadmap.
    struct Image3D
    {
        vk::Device device;
        VmaAllocator allocator = nullptr;
        vk::Image image;
        vk::ImageView view;
        VmaAllocation alloc = nullptr;
        vk::Extent3D extent;
        vk::Format format = vk::Format::eUndefined;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;

        Image3D() = default;
        Image3D(const Image3D&) = delete;
        auto operator=(const Image3D&) -> Image3D& = delete;
        Image3D(Image3D&& o) noexcept
            : device(o.device), allocator(o.allocator), image(o.image), view(o.view), alloc(o.alloc), extent(o.extent),
              format(o.format), layout(o.layout)
        {
            o.device = nullptr;
            o.allocator = nullptr;
            o.image = nullptr;
            o.view = nullptr;
            o.alloc = nullptr;
        }
        auto operator=(Image3D&& o) noexcept -> Image3D&
        {
            if (this != &o)
            {
                reset();
                device = o.device;
                allocator = o.allocator;
                image = o.image;
                view = o.view;
                alloc = o.alloc;
                extent = o.extent;
                format = o.format;
                layout = o.layout;
                o.device = nullptr;
                o.allocator = nullptr;
                o.image = nullptr;
                o.view = nullptr;
                o.alloc = nullptr;
            }
            return *this;
        }
        ~Image3D()
        {
            reset();
        }
        void reset()
        {
            if (device && view)
            {
                device.destroyImageView(view);
            }
            if (allocator != nullptr && image)
            {
                vmaDestroyImage(allocator, static_cast<VkImage>(image), alloc);
            }
            view = nullptr;
            image = nullptr;
            alloc = nullptr;
        }
    };

    // Dynamic Diffuse Global Illumination: one irradiance probe volume updated by a
    // software voxel-ray trace, sampled in the mesh fragment for multi-bounce indirect.
    // Octahedral atlases with 1-texel gutters; a 3D voxel proxy rebuilt each frame.
    struct Ddgi
    {
        bool useDdgi = false;  // off by default (it adds several compute passes / frame)
        bool ready = false;
        bool historyReset = true;  // first frame after enable/resize → no temporal history
        Image3D voxels;            // scene proxy (rgba16f: albedo + occupancy)
        Image irradiance;          // octahedral irradiance atlas (rgba16f)
        Image distance;            // octahedral moment atlas (rg16f: r, r^2)
        Image rays;                // per-frame ray radiance+distance (rays x probeCount)
        Ref<Buffer> boxBuffer;     // per-frame scene box SSBO (world AABB + albedo)
        u32 boxCapacity = 0;
        u32 frameBoxCount = 0;
        u32 frameIndex = 0;  // rotates the trace ray set
        // Volume placement (world space) — fit to the scene AABB each frame.
        glm::vec3 volumeMin{ -8.0f };
        glm::vec3 volumeExtent{ 16.0f };
        glm::vec3 sunDir{ -0.5f, -1.0f, -0.3f };
        glm::vec3 sunColor{ 1.0f };
        f32 sunIntensity = 1.0f;
        glm::vec3 skyColor{ 0.1f, 0.13f, 0.2f };
        vk::Sampler sampler;                      // linear clamp — atlases sampled in mesh + trace
        vk::DescriptorSetLayout voxelLayout;      // voxelize: 3D storage + box SSBO
        vk::DescriptorSetLayout traceLayout;      // trace: voxel storage + irr sampler + ray storage
        vk::DescriptorSetLayout blendIrrLayout;   // ray sampler + irr storage
        vk::DescriptorSetLayout blendDistLayout;  // ray sampler + dist storage
        vk::DescriptorSetLayout borderLayout;     // irr storage
        vk::DescriptorSetLayout meshLayout;       // set 5: irr sampler + dist sampler
        vk::DescriptorSet voxelSet;
        vk::DescriptorSet traceSet;
        vk::DescriptorSet blendIrrSet;
        vk::DescriptorSet blendDistSet;
        vk::DescriptorSet borderSet;
        vk::DescriptorSet meshSet;
    };

    /// A per-skinned-instance refit BLAS, keyed by entity uuid in Rt::skinnedBlas. Built once
    /// (MODE_BUILD with ALLOW_UPDATE) and refit in place every frame (MODE_UPDATE, src == dst)
    /// from the current frame's deformed buffer — topology never changes for a skinned mesh.
    struct SkinnedBlas
    {
        Ref<AccelerationStructure> as;
        bool built = false;  // false until the first MODE_BUILD; gates BUILD vs UPDATE
    };

    // Hardware ray tracing: a per-frame TLAS over the scene's mesh instances + inline
    // ray-query shadows in the mesh fragment, feature-gated (off unless context.rtSupported).
    // BLAS live on each GpuMesh; this owns the TLAS + its instance buffer (ping-ponged per
    // in-flight frame) + the TLAS descriptor set (set 6) the mesh binds.
    struct Rt
    {
        bool useRtShadows = false;  // runtime toggle (only meaningful when rtSupported)
        std::array<Ref<AccelerationStructure>, MaxFramesInFlight> tlas;
        std::array<u32, MaxFramesInFlight> tlasCapacity{};  // instances the slot's TLAS is sized for (0 = empty seed)
        std::array<Ref<Buffer>, MaxFramesInFlight> instanceBuffers;  // VkAccelerationStructureInstanceKHR[]
        std::array<u32, MaxFramesInFlight> instanceCapacity{};
        std::array<Ref<Buffer>, MaxFramesInFlight> scratchBuffers;  // TLAS build scratch
        std::array<u32, MaxFramesInFlight> scratchCapacity{};
        u32 frameInstanceCount = 0;
        bool tlasReady = false;              // a TLAS has been built this frame (bind is valid)
        vk::DescriptorSetLayout meshLayout;  // set 6: the TLAS
        std::array<vk::DescriptorSet, MaxFramesInFlight> meshSets;
        u32 blasCount = 0;  // built BLAS count (rt-stats)
        // This frame's static instance transforms + meshes, captured by renderScene + consumed
        // by the TLAS-build graph pass. Cleared each frame in beginFrame. Skinned instances ride
        // the SceneDrawList (their deformed offsets are authoritative there), not these vectors.
        std::vector<glm::mat4> frameModels;
        std::vector<Ref<GpuMesh>> frameMeshes;
        bool buildPending = false;  // RT shadows on + instances present this frame
        // Per-skinned-instance refit BLAS, per frame-in-flight then keyed by entity uuid. The
        // map is grow-only across frames so a steady entity keeps its AS and refits in place
        // (BUILD on the slot's first sight, MODE_UPDATE after). It is per-slot — like the
        // deformed buffer it reads — because an in-place UPDATE rewrites the AS while frame N's
        // GPU work may still trace the SAME slot's prior contents; the per-slot fence wait in
        // beginFrame serializes each slot, so slot f's BLAS is never refit under a live read.
        std::array<std::unordered_map<u64, SkinnedBlas>, MaxFramesInFlight> skinnedBlas;
        std::array<Ref<Buffer>, MaxFramesInFlight> blasScratchBuffers;  // skinned BLAS build/refit scratch
        std::array<u32, MaxFramesInFlight> blasScratchCapacity{};
        u32 skinnedBlasCount = 0;  // skinned refit BLAS active this frame (rt-stats)
    };

    // ReSTIR DI (reservoir spatiotemporal importance resampling) — stochastic many-light
    // direct lighting. Feeds off the froxel candidate lists (set via the cluster SSBO) +
    // the phase-7 TLAS for one visibility ray per pixel + phase-5 motion for temporal reuse.
    // Gated on rtSupported (needs ray-query). Diffuse direct only in v1.
    //
    // Restir holds the device-shared pipeline scaffolding (sampler + descriptor-set LAYOUTS);
    // the per-pixel reservoir buffers, viewport-sized radiance image, and the descriptor SETS
    // that bind them are per-view (RestirView) so two views never read each other's reservoirs.
    struct Restir
    {
        bool useRestir = false;
        vk::Sampler sampler;                    // nearest, clamp — samples G-buffer/motion
        vk::DescriptorSetLayout initialLayout;  // 4 bindings
        vk::DescriptorSetLayout reuseLayout;    // 6 bindings
        vk::DescriptorSetLayout resolveLayout;  // 6 bindings (incl. TLAS)
        vk::DescriptorSetLayout meshLayout;     // set 7: the radiance sampler
        u32 candidateCount = 16;                // K initial candidates per pixel
    };

    // Per-view ReSTIR state: the per-pixel reservoir SSBOs (initial, combined, previous), the
    // resolved-radiance image, and the descriptor sets binding them. Sized to the view's pixel
    // count, recreated with the offscreen. frameIndex/historyReset are per-view temporal state.
    struct RestirView
    {
        bool ready = false;
        bool historyReset = true;
        Image radiance;             // per-pixel resolved direct radiance (rgba16f)
        Ref<Buffer> initial;        // initial reservoirs (this frame's candidate sampling)
        Ref<Buffer> combined;       // after temporal+spatial reuse
        Ref<Buffer> previous;       // last frame's combined (temporal source)
        u32 reservoirCapacity = 0;  // pixels the buffers are sized for
        vk::DescriptorSet initialSet;
        vk::DescriptorSet reuseSet;
        vk::DescriptorSet resolveSet;
        vk::DescriptorSet meshSet;
        u32 frameIndex = 0;
    };

    // The frame as a render graph + the resource handles app-authored passes reference.
    struct FrameGraphState
    {
        RenderGraph current;
        RgResource sceneColor;  // the offscreen color handle, for app-authored passes
        RgResource swapImage;
        RgResource aoResource;         // the AO map handle when SSAO ran this frame
        RgResource contactResource;    // contact-shadow map handle when contact ran
        RgResource ssgiResource;       // SSGI radiance handle when SSGI ran
        RgResource prevColorResource;  // prevColor handle (imported once; read by SSGI, written by copy)
        RgResource ddgiIrradiance;     // DDGI irradiance atlas handle when DDGI ran
        RgResource ddgiDistance;       // DDGI moment atlas handle when DDGI ran
        RgResource restirRadiance;     // ReSTIR direct-radiance handle when ReSTIR ran
        bool hasAo = false;
        bool hasContact = false;
        bool hasSsgi = false;
        bool hasDdgi = false;
        bool hasGbuffer = false;  // the thin G-buffer prepass ran (screen effects or ReSTIR)
        bool hasRestir = false;
    };

    struct Renderer
    {
        VulkanContext context;
        Swapchain swapchain;
        FrameSync frame;
        Descriptors descriptors;
        Lighting lighting;
        Instancing instancing;
        Skinning skinning;
        Pipelines pipelines;
        Targets targets;
        std::array<ViewTargets, ViewCount> views{};  // per-pane offscreen targets (indexed by ViewId)
        ViewId activeView = ViewId::Scene;           // the view rendered + presented this frame
        Ibl ibl;
        ReflectionProbes reflection;
        Sky sky;
        Ssao ssao;
        Ddgi ddgi;
        Rt rt;
        Restir restir;
        std::array<RestirView, ViewCount> restirViews{};  // per-view reservoirs + sets (indexed by ViewId)
        FrameGraphState graph;

        bool useDepthPrepass = false;
        bool useSkinning = true;           // gate for the GPU skinning path; off = skinned items never gather
        bool presentViewportOnly = false;  // native-viewport host: blit offscreen->swapchain, skip the ui pass
        std::array<ShmPublish, ViewCount>
            shmPublish{};                     // per-view offscreen→shm publish; replaces present when enabled
        f32 exposureEv = 0.0f;                // tonemap exposure in stops; the tonemap pass applies exp2(this)
        Ref<GpuTexture> defaultWhiteTexture;  // 1x1 white; bound when a material has no albedo
        Ref<GpuMesh> previewSphere;           // lazy unit UV sphere for material previews
        RenderStats stats;                    // populated each frame by submitDrawList
        f32 frameMs = 0.0f;                   // EMA-smoothed frame-to-frame CPU time, updated in endFrame
        u64 lastFrameNs = 0;                  // steady_clock stamp of the previous endFrame; 0 until one lands
        GpuProfiler profiler;                 // per-pass GPU timestamps + capability flags (opt-in)
        CpuProfiler cpuProfiler;              // per-phase + per-pass CPU spans (opt-in, same gate)
        CaptureRecorder captureRecorder;      // bounded profiler capture state machine (request-scoped)
        f32 cpuFrameMs = 0.0f;                // EMA render-thread busy time (beginFrame→endFrame minus waits)
        f32 cpuWaitMs = 0.0f;                 // EMA time blocked on fences this frame
        f32 gpuFrameMs = 0.0f;                // EMA GPU frame time (timestamp span)
        u64 frameCpuStartNs = 0;              // steady_clock stamp at beginFrame entry
        u64 frameWaitNs = 0;                  // blocking-wait ns accumulated this frame
        bool softwareGpu = false;             // llvmpipe/lavapipe detected at device creation
        PerfConfig perfConfig;                // shared budget/threshold config (green/amber/red)
        std::array<FrameSample, FrameHistoryCapacity> frameRing{};  // rolling per-frame history
        u32 frameRingHead = 0;                                      // next write slot
        u32 frameRingCount = 0;                                     // filled entries (saturates at capacity)
        u64 frameSerial = 0;    // monotonic absolute frame index stamped into each FrameSample
        u64 stutterCount = 0;   // per-session stutter count (relative-2× + floor rule)
        u64 lastStutterNs = 0;  // steady_clock stamp of the last detected stutter
        AlarmState alarms;      // perf-degradation detectors + the non-blocking event ring
        OverlayState overlay;   // editor gizmo + billboard geometry for the frame
        // Pending window screenshot, consumed in endFrame: the swapchain image is
        // only safely owned in-frame, so the copy is deferred there.
        std::optional<std::string> captureNextSwapchainPath;
        Window* window = nullptr;  // borrowed
        // Dedicated command pool for the thumbnail worker thread (Vulkan command pools are not
        // thread-safe, so the worker never borrows a frame pool). Created in newRenderer.
        vk::CommandPool workerCommandPool;
    };

    /// The render targets for the view the renderer is currently rendering + presenting.
    inline auto activeView(Renderer& renderer) -> ViewTargets&
    {
        return renderer.views[static_cast<std::size_t>(renderer.activeView)];
    }

    inline auto activeView(const Renderer& renderer) -> const ViewTargets&
    {
        return renderer.views[static_cast<std::size_t>(renderer.activeView)];
    }

    /// The shared-memory publish slot for the view the renderer is currently presenting.
    inline auto activeShmPublish(Renderer& renderer) -> ShmPublish&
    {
        return renderer.shmPublish[static_cast<std::size_t>(renderer.activeView)];
    }

    /// The ReSTIR per-pixel reservoir state for the view the renderer is currently rendering.
    inline auto activeRestir(Renderer& renderer) -> RestirView&
    {
        return renderer.restirViews[static_cast<std::size_t>(renderer.activeView)];
    }

    auto newRenderer(Window& window) -> Result<Renderer>;
    void destroyRenderer(Renderer& renderer);

    auto beginFrame(Renderer& renderer) -> bool;
    void submit(Renderer& renderer, RenderFn fn);    // scene pass (offscreen target)
    void submitUi(Renderer& renderer, RenderFn fn);  // ui pass (swapchain)
    // Build the frame's graph (cull + scene). The run loop then lets layers add passes
    // (onRenderGraph) before endFrame finishes it with the ui pass + executes it.
    void beginFrameGraph(Renderer& renderer);
    auto frameGraph(Renderer& renderer) -> RenderGraph&;
    // The offscreen color resource in the current frame graph — an app-authored pass
    // declares its reads/writes against this handle.
    auto viewportColorResource(const Renderer& renderer) -> RgResource;
    // The mandatory HDR->display tonemap pass on the offscreen color (exposure +
    // Reinhard + gamma). beginFrameGraph adds it after the scene + AA passes.
    void addTonemapPass(Renderer& renderer, RenderGraph& graph);
    void endFrame(Renderer& renderer);

    // Native-viewport host: present-only mode blits the offscreen color straight to the
    // swapchain (no ui pass) for embedding the scene in an external window.
    void setPresentViewportOnly(Renderer& renderer, bool enabled);

    // The screen-space editor overlay pipeline (gizmo handles + entity billboards). Built
    // once in initDescriptorResources; the overlay pass draws the per-frame vertex list.
    // `depthTest` builds the variant that reads the scene depth (write off, less-or-equal).
    auto newOverlayPipeline(Renderer& renderer, bool depthTest) -> Result<Ref<Pipeline>>;
    // Stashes the editor overlay triangle list for the current frame; the editor-overlay graph
    // pass uploads + draws it over the tonemapped scene color. `depthTested` (camera frustums)
    // is occluded by scene geometry; `onTop` (handles, billboards) always draws.
    void submitOverlay(Renderer& renderer, std::vector<OverlayVertex> depthTested, std::vector<OverlayVertex> onTop);

    // The offscreen Viewport target the editor samples + displays in a panel. Records the
    // requested size on that view; beginFrame applies it only when the view is active (a
    // non-active view's size is deferred until it becomes active).
    void setViewportDesiredSize(Renderer& renderer, ViewId view, u32 width, u32 height);
    auto viewportImageView(const Renderer& renderer) -> vk::ImageView;
    auto viewportGeneration(const Renderer& renderer) -> u32;
    auto viewportWidth(const Renderer& renderer) -> u32;
    auto viewportHeight(const Renderer& renderer) -> u32;

    auto assetPath(std::string_view relative) -> std::string;

    // One entry per drawn entity in the per-frame instance storage buffer (set 2).
    // The vertex shader indexes it by InstanceIndex (firstInstance + gl_InstanceID).
    // std430-compatible: every member is 16-byte aligned.
    struct InstanceData
    {
        glm::mat4 model;
        glm::mat4 normalMatrix;       // transpose(inverse(mat3(model))), correct under non-uniform scale
        glm::mat4 prevModel{ 1.0f };  // last frame's world matrix (TAA object-motion reprojection)
        glm::vec4 baseColor;
        glm::uvec4 texture{ 0 };                  // .x = albedo index, .y = joint offset, .z = metallic-roughness index
        glm::vec4 pbr{ 0.0f, 1.0f, 0.0f, 0.0f };  // x = metallic, y = roughness
        glm::vec4 emissive{ 0.0f };               // rgb = emissive radiance (strength baked in)
    };

    // Per-distinct-material data (set 2, binding 2), indexed by InstanceData.texture.w.
    // Many instances of one material share a single entry (deduplicated per frame). The
    // übershader reads its texture indices + factors from here; later phases grow the
    // payload (normal/orm/emissive/height, uv transform, feature bits) without changing
    // the per-instance stride. std430-compatible: every member is 16-byte aligned.
    struct MaterialParamsData
    {
        glm::vec4 baseColor{ 1.0f };
        glm::vec4 pbr{ 0.0f, 1.0f, 1.0f, 0.5f };  // metallic, roughness, normalStrength, alphaCutoff
        glm::vec4 emissive{ 0.0f };               // rgb = radiance, w = heightScale
        glm::vec4 uv{ 1.0f, 1.0f, 0.0f, 0.0f };   // tiling.xy, offset.xy
        glm::uvec4 tex0{ 0u };                    // albedo, orm/mr, normal, emissive (bindless indices)
        glm::uvec4 tex1{ 0u };                    // height, reserved, reserved, featureBits
    };
    static_assert(sizeof(MaterialParamsData) == 96, "MaterialParams must match the std430 shader layout");

    // Mesh rendering: a depth-tested instanced pipeline (set 0 = material albedo,
    // set 1 = directional light, set 2 = per-instance data; push constant = viewProj),
    // device-local mesh + texture uploads, and a batched instanced draw via submit().
    auto newMeshPipeline(Renderer& renderer, std::string_view shaderName, bool unlit = false) -> Result<Ref<Pipeline>>;
    // The PSO cache front door: returns the mesh pipeline for a material variant, building
    // + caching it on first request. The renderer owns it; the client never creates PSOs.
    auto requestMeshPipeline(Renderer& renderer, const Material& material) -> Ref<Pipeline>;
    // The skinned permutation: vertexMainSkinned + the VertexSkin stream on binding 1.
    auto requestMeshPipeline(Renderer& renderer, const Material& material, bool skinned) -> Ref<Pipeline>;
    // Number of distinct mesh PSOs the cache holds (inspectable to verify übershader reuse).
    auto pipelineCount(const Renderer& renderer) -> u32;
    auto bindlessTextureCount(const Renderer& renderer) -> u32;
    auto bindlessFreeCount(const Renderer& renderer) -> u32;
    auto uploadMesh(Renderer& renderer, const Mesh& mesh) -> Result<Ref<GpuMesh>>;
    // As above, plus the VertexSkin stream (must parallel mesh.vertices) uploaded as the
    // second vertex buffer the skinned PSO binds.
    auto uploadMesh(Renderer& renderer, const Mesh& mesh, const std::vector<VertexSkin>& skin) -> Result<Ref<GpuMesh>>;
    auto uploadTexture(Renderer& renderer, const u8* rgba, u32 width, u32 height, bool srgb) -> Result<Ref<GpuTexture>>;
    /// Uploads tightly-packed linear float RGBA (width*height*4 floats) as a half-float
    /// (eR16G16B16A16Sfloat) sampled texture in the bindless array. For HDR panoramas /
    /// environment sources; no sRGB encoding. Narrows f32 -> f16 on the CPU before staging.
    auto uploadTextureFloat(Renderer& renderer, const f32* rgba, u32 width, u32 height) -> Result<Ref<GpuTexture>>;

    // Rasterizes an SVG to a square RGBA icon (tint multiplied in) and uploads it as a
    // GPU texture — used for asset-browser type icons. "currentColor" maps to white.
    auto uploadSvgIcon(Renderer& renderer, const std::string& svgPath, u32 pixelSize, glm::vec4 tint)
        -> Result<Ref<GpuTexture>>;

    // Renders a mesh to a square GPU texture (a 3/4 view framed by the mesh AABB, lit by
    // a fixed light) for an asset thumbnail. Synchronous one-off render; safe between frames.
    auto renderMeshThumbnail(Renderer& renderer, const Ref<GpuMesh>& mesh, u32 size) -> Result<Ref<GpuTexture>>;
    // Renders a unit sphere with `material` under fixed studio lighting (the preview.slang pipeline)
    // into a size×size sampled texture — the material preview pane + cached material thumbnails. Like
    // renderMeshThumbnail, an out-of-band render that waits idle (never on the present path).
    // shaderSpv: empty => the default studio preview shader; otherwise a codegen'd preview .spv path
    // (a node-graph material's compiled surface), used for a fresh per-call pipeline.
    auto renderMaterialPreview(Renderer& renderer, const SubmeshMaterial& material, u32 size,
                               const std::string& shaderSpv = std::string{}) -> Result<Ref<GpuTexture>>;
    // Renders a model's mesh with its per-submesh materials (indexed by Submesh.materialSlot) under the
    // same studio lighting as renderMaterialPreview, framed by the mesh AABB — the textured asset-tile
    // preview. An out-of-band render that waits idle, like the other thumbnail renders.
    auto renderModelThumbnail(Renderer& renderer, const Ref<GpuMesh>& mesh,
                              const std::vector<SubmeshMaterial>& submeshMaterials, u32 size)
        -> Result<Ref<GpuTexture>>;

    // How a captured RGBA16F buffer is mapped to 8-bit PNG. `Clamp` clamps [0,1]×255 — correct for
    // an already-tonemapped offscreen (screenshots, mesh/material previews). `Tonemap` Reinhard-maps
    // + gamma-encodes scene-linear radiance — for thumbnails of HDR *assets*, whose values run past 1.
    enum class PngTransfer
    {
        Clamp,
        Tonemap
    };

    // PNG bytes plus the actual pixel dimensions encoded (so the control reply can report
    // truthful width/height rather than echoing the requested size).
    struct ThumbnailPng
    {
        std::vector<u8> bytes;
        u32 width = 0;
        u32 height = 0;
    };

    // Renders/loads an asset to PNG bytes in memory (synchronous, own command buffer + waitIdle;
    // never on the present path). Mesh: framed like renderMeshThumbnail at size×size. Texture:
    // GPU-downscaled to fit within size×size (chained linear halving) before readback, so the
    // cost is bounded by `size` regardless of source resolution; `width`/`height` report the PNG.
    // `transfer` controls HDR mapping — pass Tonemap for an HDR asset, Clamp otherwise.
    auto encodeAssetThumbnailPng(Renderer& renderer, const Ref<GpuMesh>& mesh, u32 size) -> Result<ThumbnailPng>;
    // Like encodeAssetThumbnailPng but shades the mesh with its per-submesh materials (the textured
    // model preview) rather than the flat mesh look.
    auto encodeModelThumbnailPng(Renderer& renderer, const Ref<GpuMesh>& mesh,
                                 const std::vector<SubmeshMaterial>& submeshMaterials, u32 size)
        -> Result<ThumbnailPng>;
    auto encodeTextureThumbnailPng(Renderer& renderer, const Ref<GpuTexture>& texture, u32 size,
                                   PngTransfer transfer = PngTransfer::Clamp) -> Result<ThumbnailPng>;

    // Binds the calling thread to the renderer's dedicated thumbnail command pool: every subsequent
    // one-off upload/render/readback on this thread allocates from `workerCommandPool` instead of a
    // frame pool. Call once at the top of the thumbnail worker thread. Idempotent per thread.
    void bindThumbnailWorkerThread(Renderer& renderer);

    // Eagerly creates the lazily-built thumbnail/preview pipelines + preview sphere on the calling
    // (main) thread, so the worker thread never races their first-use initialization. Call before
    // starting the thumbnail worker. Idempotent.
    auto prewarmThumbnailResources(Renderer& renderer) -> Result<void>;

    // Resolves each item's material to a cached PSO, batches by (pipeline, mesh, texture),
    // uploads the frame's instance buffer, and stores the structured draw list on the
    // renderer for the scene + depth passes to consume.
    void submitDrawList(Renderer& renderer, const glm::mat4& viewProj, const std::vector<DrawItem>& items);
    // As above, plus the frame's joint palette: skinned items index it via jointOffset
    // (uploaded to set 2, binding 1). The three-arg form passes an empty palette.
    void submitDrawList(Renderer& renderer, const glm::mat4& viewProj, const std::vector<DrawItem>& items,
                        const std::vector<glm::mat4>& joints);
    // Record the frame's scene geometry into the active pass (the scene-pass body).
    void recordSceneDrawList(Renderer& renderer, vk::CommandBuffer cmd);
    // Record depth-only draws of the frame's geometry (the depth-pre-pass body).
    void recordDepthPrepass(Renderer& renderer, vk::CommandBuffer cmd);

    // Per-frame visible-sky settings, resolved by renderScene from Scene.environment.
    struct SkyRenderSettings
    {
        u32 mode = 2;  // 0 = Color, 1 = Texture, 2 = Procedural
        glm::vec3 clearColor{ 0.05f, 0.06f, 0.08f };
        f32 intensity = 1.0f;
        f32 rotation = 0.0f;  // yaw radians
        bool visible = true;
        u32 textureIndex = 0;  // bindless slot of the panorama (Texture mode)
    };
    // Stores the sky settings for this frame; the sky pass (added in beginFrameGraph when
    // visible) draws from them. Leaves the pipeline/set/ready state intact.
    void submitSky(Renderer& renderer, const SkyRenderSettings& settings);
    // Record the fullscreen sky into the active pass (the sky-pass body).
    void recordSky(Renderer& renderer, vk::CommandBuffer cmd);

    // Selects the IBL environment source and requests a re-bake when it changed. Procedural
    // fills envCube from `params` (ibl_skygen); Equirect projects `panorama` into envCube
    // (ibl_equirect), holding the Ref alive across the bake. Arms a re-bake (consumed in
    // beginFrameGraph) when the source, the panorama identity, or — for Procedural — `params`
    // change. No-op if nothing changed, so it is cheap to call every frame.
    void requestEnvBake(Renderer& renderer, EnvSource source, Ref<GpuTexture> panorama, const SkygenParams& params);
    // Procedural-source convenience wrapper over requestEnvBake.
    void requestSkyBake(Renderer& renderer, const SkygenParams& params);

    // One punctual (point or spot) light in the per-frame light storage buffer
    // (set 1, binding 1). Positions/directions are world space; the fragment shader
    // attenuates by distance and (for spots) the cone. std430-compatible.
    struct GpuLight
    {
        glm::vec4 positionRange;   // xyz world position, w range
        glm::vec4 colorIntensity;  // rgb color, a intensity
        glm::vec4 directionType;   // xyz world direction (spot), w type (0 = point, 1 = spot)
        glm::vec4 spotCos;         // x = cos(innerAngle), y = cos(outerAngle)
    };

    // Updates the shared per-frame directional light UBO (set 1). direction points
    // the way the light travels. Sets the punctual light count to zero.
    void setDirectionalLight(Renderer& renderer, glm::vec3 direction, glm::vec3 color, f32 intensity, f32 ambient);

    // Writes the whole per-frame lighting state: the directional light + ambient + the
    // camera eye position into the UBO and the punctual lights into the storage buffer
    // (grown on demand). The eye position feeds the BRDF view vector. `ambient` is the
    // premultiplied RGB fallback ambient (color * intensity), used when IBL is off.
    void setSceneLighting(Renderer& renderer, glm::vec3 direction, glm::vec3 color, f32 intensity, glm::vec3 ambient,
                          glm::vec3 eyePosition, const std::vector<GpuLight>& lights);

    // Uploads the camera into the cluster-params UBO and arms the per-frame light-cull
    // compute dispatch (clustered forward). `proj` is the Y-flipped projection used for
    // rendering. Call once per frame after setSceneLighting, before endFrame.
    void setClusterCamera(Renderer& renderer, const glm::mat4& view, const glm::mat4& proj, f32 nearPlane,
                          f32 farPlane);

    // Toggles clustered light culling. When off, the fragment shader loops every light
    // (the reference path) — useful for A/B verification.
    void setClustered(Renderer& renderer, bool enabled);
    auto clusteredEnabled(const Renderer& renderer) -> bool;
    // Tonemap exposure in stops (EV). exp2(ev) scales radiance before the tonemap.
    void setExposure(Renderer& renderer, f32 ev);
    auto exposureEv(const Renderer& renderer) -> f32;
    // Image-based lighting: toggle split-sum IBL ambient vs the flat scalar fallback.
    void setIbl(Renderer& renderer, bool enabled);
    auto iblEnabled(const Renderer& renderer) -> bool;
    // Reflection probes: sync the renderer's probe slots from the scene each frame (add/update/
    // remove, allocate cubes on first sight, arm a capture for any dirty/moved slot, upload the
    // metadata SSBO). Cheap to call every frame — capture itself runs only when capturePending.
    void submitReflectionProbes(Renderer& renderer, std::span<const ReflectionProbeUpload> probes);
    // Global probe-sampling toggle (set-probes 0|1); A/B identity gate.
    void setReflectionProbes(Renderer& renderer, bool enabled);
    auto reflectionProbesEnabled(const Renderer& renderer) -> bool;
    // Screen-space effects off the thin G-buffer. AO darkens indirect; contact shadows
    // darken the directional direct term; SSGI adds one-bounce indirect. Each toggleable.
    void setSsao(Renderer& renderer, bool enabled);
    auto ssaoEnabled(const Renderer& renderer) -> bool;
    void setContactShadows(Renderer& renderer, bool enabled);
    auto contactShadowsEnabled(const Renderer& renderer) -> bool;
    void setSsgi(Renderer& renderer, bool enabled);
    auto ssgiEnabled(const Renderer& renderer) -> bool;
    // True if any screen-space effect is on (so the G-buffer prepass is worth running).
    auto screenEffectsEnabled(const Renderer& renderer) -> bool;
    // DDGI probe global illumination: toggle multi-bounce indirect (software voxel trace).
    void setDdgi(Renderer& renderer, bool enabled);
    auto ddgiEnabled(const Renderer& renderer) -> bool;
    // Uploads the scene's per-draw world AABBs + albedo into the DDGI box SSBO and fits the
    // probe volume to them; arms the per-frame DDGI update. Called from renderScene.
    void setDdgiScene(Renderer& renderer, const std::vector<glm::vec4>& boxMins, const std::vector<glm::vec4>& boxMaxs,
                      const std::vector<glm::vec4>& boxAlbedos, glm::vec3 volumeMin, glm::vec3 volumeExtent,
                      glm::vec3 sunDir, glm::vec3 sunColor, f32 sunIntensity, glm::vec3 skyColor);
    // Hardware ray tracing (feature-gated). rtSupported reports device capability; the
    // toggle is a no-op when unsupported. RT shadows trace one ray-query per light.
    auto rtSupported(const Renderer& renderer) -> bool;
    void setRtShadows(Renderer& renderer, bool enabled);
    auto rtShadowsEnabled(const Renderer& renderer) -> bool;
    auto rtBlasCount(const Renderer& renderer) -> u32;
    // Captures the frame's instance transforms + meshes for the TLAS-build pass (renderScene).
    void setRtScene(Renderer& renderer, std::vector<glm::mat4> models, std::vector<Ref<GpuMesh>> meshes);
    // Builds the per-frame TLAS over the scene's mesh instances (model matrix per instance).
    // Records into the active command buffer (a graph compute pass). Arms tlasReady.
    void buildTlas(Renderer& renderer, vk::CommandBuffer cmd, const std::vector<glm::mat4>& models,
                   const std::vector<Ref<GpuMesh>>& meshes);
    // ReSTIR many-light direct lighting (feature-gated on rtSupported). Diffuse direct in v1.
    void setRestir(Renderer& renderer, bool enabled);
    auto restirEnabled(const Renderer& renderer) -> bool;
    // Resets every temporal accumulator for one view so it re-converges from scratch: TAA history,
    // the motion reprojection's previous-viewProj, and the view's ReSTIR reservoir history. DDGI is
    // scene-global but its probes must re-converge too, so its history reset is armed here as well.
    // Call only at a frame boundary (TAA captures historyValid by value into its execute closure).
    void resetViewTemporal(Renderer& renderer, ViewId view);
    // Selects which view the renderer renders + presents. When the active view changes, the
    // newly-shown view's temporal accumulators are stale, so this resets them (reset-on-activate).
    // Call at a frame boundary (before beginFrameGraph) so the reset settles before TAA captures
    // historyValid. A no-op when `view` is already active.
    void setActiveView(Renderer& renderer, ViewId view);
    // Records the G-buffer prepass (view normal + view-Z) for the screen-space pass bodies.
    void recordGbuffer(Renderer& renderer, vk::CommandBuffer cmd);
    // Feeds the camera the screen-space passes need: view, proj (SAME Y-flipped projection
    // the scene renders with, so the maps align), and the world-space sun direction.
    void setSsaoCamera(Renderer& renderer, const glm::mat4& view, const glm::mat4& proj, glm::vec3 sunDirectionWorld);
    // Directional shadow map: toggle + the per-frame light-space transform. renderScene
    // fits the transform to the scene each frame; beginFrameGraph runs the depth pass.
    void setShadows(Renderer& renderer, bool enabled);
    auto shadowsEnabled(const Renderer& renderer) -> bool;
    void setDirectionalShadow(Renderer& renderer, const glm::mat4& lightViewProj, bool casting);
    // Arms the spot shadow depth pass for the first shadow-casting spot light: its
    // perspective light-space transform + its index in the frame's punctual light list.
    void setSpotShadow(Renderer& renderer, const glm::mat4& lightViewProj, u32 lightIndex, bool casting);
    // Arms the omnidirectional point shadow pass for the first point light: its world
    // position + far plane + index in the frame's punctual light list.
    void setPointShadow(Renderer& renderer, glm::vec3 lightPos, f32 farPlane, u32 lightIndex, bool casting);
    // Record the scene geometry depth-only from the light's point of view (shadow pass body).
    void recordShadowDepth(Renderer& renderer, vk::CommandBuffer cmd, const glm::mat4& lightViewProj);
    // Record the 6 cube faces of the point shadow distance map (its own rendering scopes).
    void recordPointShadow(Renderer& renderer, vk::CommandBuffer cmd, glm::vec3 lightPos, f32 farPlane);
    void setDepthPrepass(Renderer& renderer, bool enabled);
    auto depthPrepassEnabled(const Renderer& renderer) -> bool;
    // GPU skinning gate: off makes renderScene skip the skinned gather entirely, so a
    // scene renders exactly as it would without the skinned path.
    void setSkinning(Renderer& renderer, bool enabled);
    auto skinningEnabled(const Renderer& renderer) -> bool;
    // Anti-aliasing: msaaSamples is 1 (off) / 2 / 4 / 8 (clamped to the device cap); fxaa
    // and taa toggle their post-process passes. The three modes are mutually exclusive.
    // Recreates the MSAA/TAA targets + rebuilds scene PSOs.
    void setAa(Renderer& renderer, u32 msaaSamples, bool fxaa, bool taa);
    void setAaMode(Renderer& renderer, const std::string& mode);  // inverse of aaMode
    auto aaMode(const Renderer& renderer) -> std::string;         // "off" | "fxaa" | "taa" | "msaa2|4|8"
    // Records the motion-vector prepass (camera reprojection) for the TAA pass body.
    void recordMotion(Renderer& renderer, vk::CommandBuffer cmd);

    // A 1x1 white texture; bind it when a material has no albedo.
    auto defaultTexture(const Renderer& renderer) -> const Ref<GpuTexture>&;

    // The most recent frame's scene draw counters (draw calls, batches, instances).
    auto renderStats(const Renderer& renderer) -> RenderStats;

    // GPU profiler control. `off` keeps the present-only host at baseline cost; deeper
    // modes enable per-pass timestamps + throughput counters + VMA budget. A requested
    // mode the device cannot support degrades to the deepest it can (timestamps, or off).
    void setProfilerMode(Renderer& renderer, ProfilerMode mode);
    auto profilerMode(const Renderer& renderer) -> ProfilerMode;
    auto profilerTimestampsSupported(const Renderer& renderer) -> bool;
    auto profilerPipelineStatsSupported(const Renderer& renderer) -> bool;
    // True on a software rasterizer (llvmpipe/lavapipe): GPU timings are CPU time, not
    // representative of real hardware — downstream should annotate or suppress them.
    auto softwareGpu(const Renderer& renderer) -> bool;
    // The last frame's per-pass GPU times, keyed by graph pass name, plus their summed
    // span. Empty until a timestamps-mode frame has been recorded and read back.
    auto passTimings(const Renderer& renderer) -> std::vector<PassTiming>;
    auto passTimingsTotalMs(const Renderer& renderer) -> f32;

    // Bounded profiler capture (request-scoped, driven by profiler.capture-start/stop). Start
    // arms the recorder (forcing Timestamps mode + sub-scopes for the duration, restored on
    // stop) and returns its id; the engine then copies each finalized frame's merged spans into
    // the capture until it reaches the frame count. Stop finalizes and returns the result; an
    // empty (not-ready) capture is returned when nothing was recorded.
    auto startProfileCapture(Renderer& renderer, CaptureMode mode, u32 frames, std::string filter, bool includeCpu,
                             bool includeStats) -> u32;
    auto profileStatsSupported(const Renderer& renderer) -> bool;
    auto profileCaptureState(const Renderer& renderer) -> CaptureState;
    auto profileCaptureReady(const Renderer& renderer) -> bool;
    auto profileCaptureMode(const Renderer& renderer) -> CaptureMode;
    auto profileCaptureCapturedFrames(const Renderer& renderer) -> u32;
    auto profileCaptureTargetFrames(const Renderer& renderer) -> u32;
    auto stopProfileCapture(Renderer& renderer) -> ProfileCapture;

    // Frame-time history (always recorded, independent of the profiler mode). The summary
    // is computed on demand over the ring; `frameSamples` returns up to `maxSamples` of the
    // most recent raw samples, oldest→newest, for the editor's live graph.
    auto frameHistoryStats(const Renderer& renderer) -> FrameHistoryStats;
    auto frameSamples(const Renderer& renderer, u32 maxSamples) -> std::vector<FrameSample>;

    // The shared budget/threshold config. budget(ms) = 1000 / targetFps.
    auto perfConfig(const Renderer& renderer) -> PerfConfig;
    void setPerfConfig(Renderer& renderer, const PerfConfig& config);
    auto perfBudgetMs(const PerfConfig& config) -> f32;

    // Non-blocking alarm delivery. drainAlarms returns events with seq > since plus the
    // cursor metadata (high-water/oldest/overflow); the handler only snapshots the ring, so
    // it never stalls the render thread. activeAlarms is the current firing set (the badge).
    auto drainAlarms(const Renderer& renderer, u64 since) -> AlarmDrain;
    auto activeAlarms(const Renderer& renderer) -> std::vector<ActiveAlarm>;

    // Blocks until the GPU has finished all submitted work. Call before dropping
    // resource Refs at shutdown so no in-flight command buffer still references them.
    void waitGpuIdle(Renderer& renderer);

    // Copies the offscreen viewport image to a PNG. Synchronous (own submit +
    // waitIdle), safe to call between frames.
    auto captureViewport(Renderer& renderer, const std::string& path) -> Result<void>;

    // Requests a PNG of the next presented frame (written in endFrame). Fails here
    // if the surface lacks TRANSFER_SRC; otherwise returns immediately.
    auto requestWindowCapture(Renderer& renderer, std::string path) -> Result<void>;

    // Pipelined offscreen→shared-memory publish for the editor's presenter. Once enabled,
    // endFrame records an offscreen→BGRA8→staging readback into each frame's own command
    // buffer and skips swapchain acquire/present entirely; beginFrame memcpys the ready
    // slot into the shared segment after its fence wait. Zero added stalls.
    void enableViewportShmPublish(Renderer& renderer, ViewId view, const std::string& shmName);
    auto ensureShmPublishSlot(Renderer& renderer, ShmPublishSlot& slot, u32 width, u32 height) -> bool;
    void publishShmPublishSlot(Renderer& renderer, ShmPublishSlot& slot);
    // Frees every per-view shm publisher's readback slots + mapped segment on teardown.
    void destroyShmPublish(Renderer& renderer);

    // Frees one view's per-view resources (offscreen targets, ReSTIR reservoirs + radiance, and
    // its shm publisher) under a device idle, honoring the Ref-drop-then-idle order. Safe on a
    // never-allocated view (null Refs / zero extents). Pool-owned descriptor sets are left intact.
    // Called when an editor pane closes; the device + global state outlive it.
    void destroyView(Renderer& renderer, ViewId view);
}
