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
#include <optional>
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
    // Format of the offscreen depth buffer. D32_SFLOAT is universally supported.
    inline constexpr vk::Format DepthFormat = vk::Format::eD32Sfloat;

    // Format of the offscreen color target. RGBA16_SFLOAT so the scene pass can write
    // linear HDR radiance that survives to the mandatory tonemap pass; it is also a
    // storage image (the tonemap/FXAA compute passes write it) and sampled by ImGui.
    inline constexpr vk::Format OffscreenColorFormat = vk::Format::eR16G16B16A16Sfloat;

    // A unit of GPU work recorded into the active command buffer — the deferred
    // submission seam. The backend supplies the command buffer.
    using RenderFn = std::function<void(vk::CommandBuffer)>;

    inline constexpr u32 MaxFramesInFlight = 2;

    // Capacity of the bindless texture array (set 0). One global combined-image-sampler
    // array indexed per-instance; lavapipe + desktop GPUs allow far more, this is plenty.
    inline constexpr u32 MaxBindlessTextures = 1024;

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

        Pipeline(Pipeline&& other) noexcept
            : device(other.device), pipeline(other.pipeline), layout(other.layout)
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
            : device(other.device), allocator(other.allocator), image(other.image),
              view(other.view), alloc(other.alloc), extent(other.extent),
              format(other.format), layout(other.layout)
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
              indexBuffer(other.indexBuffer), indexAlloc(other.indexAlloc), indexCount(other.indexCount),
              submeshes(std::move(other.submeshes)), boundsMin(other.boundsMin), boundsMax(other.boundsMax)
        {
            other.allocator = nullptr;
            other.vertexBuffer = nullptr;
            other.vertexAlloc = nullptr;
            other.indexBuffer = nullptr;
            other.indexAlloc = nullptr;
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
                indexCount = other.indexCount;
                submeshes = std::move(other.submeshes);
                boundsMin = other.boundsMin;
                boundsMax = other.boundsMax;
                other.allocator = nullptr;
                other.vertexBuffer = nullptr;
                other.vertexAlloc = nullptr;
                other.indexBuffer = nullptr;
                other.indexAlloc = nullptr;
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
            }
            vertexBuffer = nullptr;
            indexBuffer = nullptr;
            vertexAlloc = nullptr;
            indexAlloc = nullptr;
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
        u32 bindlessIndex = 0;             // slot in the bindless texture array (set 0)
        vk::Extent2D extent;
        vk::Format format = vk::Format::eUndefined;

        GpuTexture() = default;
        GpuTexture(const GpuTexture&) = delete;
        auto operator=(const GpuTexture&) -> GpuTexture& = delete;

        GpuTexture(GpuTexture&& other) noexcept
            : device(other.device), allocator(other.allocator), image(other.image),
              view(other.view), alloc(other.alloc), bindlessIndex(other.bindlessIndex),
              extent(other.extent), format(other.format)
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
            // The bindless slot is not reclaimed here: no live material references a
            // destroyed texture's index, so its stale descriptor is never sampled.
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
        vk::Device device;                 // borrowed
        VmaAllocator allocator = nullptr;  // borrowed
        PFN_vkDestroyAccelerationStructureKHR destroyFn = nullptr;  // borrowed
        vk::AccelerationStructureKHR handle;
        vk::DeviceAddress address = 0;
        vk::Buffer buffer;
        VmaAllocation alloc = nullptr;

        AccelerationStructure() = default;
        AccelerationStructure(const AccelerationStructure&) = delete;
        auto operator=(const AccelerationStructure&) -> AccelerationStructure& = delete;
        AccelerationStructure(AccelerationStructure&& o) noexcept
            : device(o.device), allocator(o.allocator), destroyFn(o.destroyFn), handle(o.handle),
              address(o.address), buffer(o.buffer), alloc(o.alloc)
        {
            o.handle = nullptr; o.buffer = nullptr; o.alloc = nullptr; o.allocator = nullptr;
        }
        auto operator=(AccelerationStructure&& o) noexcept -> AccelerationStructure&
        {
            if (this != &o)
            {
                reset();
                device = o.device; allocator = o.allocator; destroyFn = o.destroyFn; handle = o.handle;
                address = o.address; buffer = o.buffer; alloc = o.alloc;
                o.handle = nullptr; o.buffer = nullptr; o.alloc = nullptr; o.allocator = nullptr;
            }
            return *this;
        }
        ~AccelerationStructure() { reset(); }
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
            handle = nullptr; buffer = nullptr; alloc = nullptr;
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
            : allocator(other.allocator), buffer(other.buffer), alloc(other.alloc),
              mapped(other.mapped), size(other.size)
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

    /// One renderable for the scene draw list: a mesh + its albedo texture (null =>
    /// default white) + transform + base color + material. submitDrawList resolves each
    /// material to a cached PSO and batches by (pipeline, mesh, texture) into instanced
    /// draws that the scene + depth passes both consume.
    struct DrawItem
    {
        Ref<GpuMesh> mesh;
        Ref<GpuTexture> texture;
        glm::mat4 model{ 1.0f };
        glm::mat4 normalMatrix{ 1.0f };
        glm::vec4 baseColor{ 1.0f };
        f32 metallic = 0.0f;
        f32 roughness = 1.0f;
        glm::vec3 emissive{ 0.0f };
        f32 emissiveStrength = 1.0f;
        Material material;
    };

    // A batch of instances sharing a pipeline + mesh, drawn as one instanced drawIndexed.
    // Bindless means the albedo texture is a per-instance index (in the instance buffer),
    // not a per-batch descriptor — so batches no longer split by texture. baseInstance
    // offsets into the frame's instance buffer.
    struct DrawBatch
    {
        Ref<Pipeline> pipeline;      // resolved from the material via the PSO cache
        Ref<GpuMesh> mesh;
        u32 baseInstance = 0;
        u32 instanceCount = 0;
    };

    // The scene's structured draw list for the frame: built by submitDrawList from the
    // DrawItems, consumed by the scene pass (shaded) and the depth pre-pass (depth only).
    struct SceneDrawList
    {
        glm::mat4 viewProj{ 1.0f };
        std::vector<DrawBatch> batches;
        std::vector<Ref<GpuTexture>> liveTextures;  // pins indexed textures for the frame
        vk::DescriptorSet lightSet;
        vk::DescriptorSet instanceSet;
        bool valid = false;
    };

    // Per-frame scene draw counters, refreshed by submitDrawList; inspectable via the
    // control plane so batching can be verified live.
    struct RenderStats
    {
        u32 drawCalls = 0;  // drawIndexed calls (one per submesh per batch)
        u32 batches = 0;    // distinct (mesh, texture) buckets
        u32 instances = 0;  // total entities drawn
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
        std::vector<vk::Fence> imagesInFlight;       // borrowed per-frame fence per image
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

    // Descriptor infrastructure built once: layouts, pools, the bindless set, and the
    // post-process compute sets. set 0 = bindless image array, 1 = light UBO, 2 = instances.
    struct Descriptors
    {
        vk::Sampler linearSampler;
        vk::Sampler shadowSampler;                   // depth-compare sampler (PCF) for the shadow map
        vk::DescriptorSetLayout bindlessSetLayout;   // set 0: bindless combined-image-sampler array
        vk::DescriptorSetLayout lightSetLayout;      // set 1: directional light UBO
        vk::DescriptorSetLayout instanceSetLayout;   // set 2: per-instance storage buffer
        vk::DescriptorPool descriptorPool;           // eFreeDescriptorSet (texture sets freed on Ref drop)
        // Bindless: one global texture array bound once; uploadTexture writes a stable
        // slot (update-after-bind) and returns its index. The default white is slot 0.
        vk::DescriptorPool bindlessPool;             // eUpdateAfterBindPool
        vk::DescriptorSet bindlessSet;               // the single set 0 for all draws
        u32 nextBindlessIndex = 0;
        vk::DescriptorSetLayout tonemapSetLayout;    // compute set 0: storage image
        vk::DescriptorSet tonemapSet;                // points at the offscreen color view (GENERAL)
        vk::DescriptorSetLayout fxaaSetLayout;
        vk::DescriptorSet fxaaSet;
        // TAA resolve set (compute): current + history + motion samplers, offscreen +
        // history storage. Two sets (one per ping-pong parity) rewritten when targets change.
        vk::DescriptorSetLayout taaSetLayout;
        std::array<vk::DescriptorSet, 2> taaSets;
        vk::DescriptorSetLayout clusterSetLayout;    // compute set 0
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
        std::array<vk::DescriptorSet, MaxFramesInFlight> clusterSets;  // compute
        std::array<Ref<Buffer>, MaxFramesInFlight> clusterBuffers;     // per-cluster count + indices
        std::array<VkBuffer, MaxFramesInFlight> clusterParamBuffers{}; // cluster params UBO
        std::array<VmaAllocation, MaxFramesInFlight> clusterParamAllocs{};
        std::array<void*, MaxFramesInFlight> clusterParamMapped{};
        bool useClustered = true;        // false = fragment loops all lights (reference)
        u32 frameLightCount = 0;         // punctual lights uploaded this frame
        bool clusterDispatchPending = false;
        // Directional shadow: the light-space transform written into the light UBO + the
        // shadow pass push constant. shadowPending arms the depth pass for the frame.
        bool useShadows = true;          // master toggle (se set-shadows)
        bool shadowPending = false;      // a shadow-casting directional light is present this frame
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
    // capacity is in InstanceData elements.
    struct Instancing
    {
        std::array<Ref<Buffer>, MaxFramesInFlight> buffers;
        std::array<vk::DescriptorSet, MaxFramesInFlight> sets;
        std::array<u32, MaxFramesInFlight> capacity{};
    };

    // Renderer-owned pipelines + the mesh PSO cache (built on demand, keyed by variant;
    // the uebershader maps many materials to one PSO, permutations add cache entries).
    struct Pipelines
    {
        Ref<Pipeline> thumbnail;     // lazy mesh-thumbnail graphics pipeline
        Ref<Pipeline> tonemap;       // in-place compute tonemap (post-process)
        Ref<Pipeline> depthPrepass;  // vertex-only depth pre-pass
        Ref<Pipeline> shadowDepth;   // vertex-only depth pass into the shadow map (depth-biased)
        Ref<Pipeline> pointShadow;   // color (distance) + depth pass into a point shadow cube face
        Ref<Pipeline> gbuffer;       // thin G-buffer prepass (view normal + view-Z)
        Ref<Pipeline> gtao;          // compute screen-space AO from the G-buffer
        Ref<Pipeline> aoBlur;        // bilateral denoise of the raw AO
        Ref<Pipeline> contact;       // screen-space contact shadows (directional)
        Ref<Pipeline> ssgi;          // screen-space one-bounce GI
        Ref<Pipeline> copyColor;     // capture linear-HDR scene color into prevColor
        Ref<Pipeline> motion;        // motion-vector prepass (camera reprojection)
        Ref<Pipeline> taa;           // compute TAA resolve
        Ref<Pipeline> ddgiVoxelize;  // build the voxel scene proxy
        Ref<Pipeline> ddgiTrace;     // software probe ray trace
        Ref<Pipeline> ddgiBlendIrr;  // blend rays -> irradiance atlas
        Ref<Pipeline> ddgiBlendDist; // blend rays -> moment atlas
        Ref<Pipeline> ddgiBorder;    // octahedral gutter copy
        Ref<Pipeline> restirInitial; // ReSTIR initial candidate sampling
        Ref<Pipeline> restirReuse;   // ReSTIR temporal + spatial reuse
        Ref<Pipeline> restirResolve; // ReSTIR resolve (1 shadow ray) + shade
        Ref<Pipeline> fxaa;          // compute FXAA post-process
        Ref<Pipeline> cull;          // compute light-cull (clustered forward)
        std::unordered_map<std::string, Ref<Pipeline>> cache;
    };

    // The offscreen render targets + the AA state that decides which targets exist.
    struct Targets
    {
        Image offscreen;     // scene render target shown in the Viewport panel
        Image depth;         // depth buffer for the scene pass, sized to the viewport
        Image shadowMap;     // directional-light depth map (sampled with the compare sampler)
        Image spotShadowMap; // first shadow-casting spot light's depth map (same compare sampler)
        // First shadow-casting point light's omnidirectional distance cubemap: a color cube
        // (R32_SFLOAT = world distance to the nearest occluder) rendered face-by-face with a
        // shared depth scratch; the mesh samples it by direction and compares linear distance.
        Image pointShadowCube;
        Image pointShadowDepth;
        std::array<vk::ImageView, 6> pointShadowFaces{};  // per-face render views (freed manually)
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
        u32 historyIndex = 0;      // this frame writes history[historyIndex], reads the other
        bool historyValid = false; // false on the first frame / after a resize
        // MSAA: when sampleCount > 1 the scene renders to these multisampled targets and
        // resolves color into offscreen. Sized to the viewport, recreated with it.
        Image msaaColor;
        Image msaaDepth;
        // FXAA: when on, the scene renders to scratch (1x sampled), and a compute pass
        // edge-blurs it into offscreen.
        Image scratch;
        vk::SampleCountFlagBits sampleCount = vk::SampleCountFlagBits::e1;     // 1 = MSAA off
        vk::SampleCountFlagBits maxSampleCount = vk::SampleCountFlagBits::e1;  // device cap
        bool fxaaEnabled = false;  // FXAA post-process (mutually exclusive with MSAA)
        bool taaEnabled = false;   // TAA resolve (mutually exclusive with MSAA/FXAA)
        u32 desiredWidth = 0;      // requested by the UI panel (applied next frame)
        u32 desiredHeight = 0;
        u32 generation = 0;        // bumped whenever the offscreen image is recreated
    };

    // Image-based lighting: a procedural/HDR environment cubemap convolved into a diffuse
    // irradiance cube + a roughness-mipped prefiltered specular cube + a split-sum BRDF
    // LUT. Sampled as the mesh ambient (set 3). Baked once at startup.
    struct Ibl
    {
        Image envCube;          // source environment (procedural sky)
        Image irradianceCube;   // diffuse irradiance convolution
        Image prefilteredCube;  // GGX-prefiltered specular (one mip per roughness step)
        Image brdfLut;          // split-sum (scale, bias) table (rgba16f, RG used)
        u32 prefilterMips = 1;
        vk::Sampler sampler;                // linear, clamp, mipped — all three sampled in the mesh
        vk::DescriptorSetLayout setLayout;  // set 3 (irradiance, prefiltered, brdf LUT)
        vk::DescriptorSet set;
        bool ready = false;
        bool useIbl = true;     // false = flat scalar ambient fallback
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
        bool ready = false;                 // set written + envCube baked
    };

    // Screen-space ambient occlusion (GTAO-lite). When on, a G-buffer prepass + a compute
    // pass produce an AO factor the mesh multiplies into the IBL/flat ambient term. The
    // descriptor sets/layouts are built once; the camera transforms are written per frame.
    // Screen-space effects driven off the thin G-buffer: ambient occlusion (GTAO +
    // bilateral denoise), directional contact shadows, and one-bounce SSGI. They share the
    // G-buffer + the scene's view/proj; the mesh samples their three maps via set 4.
    struct Ssao
    {
        bool useSsao = true;     // GTAO ambient occlusion
        bool useContact = true;  // screen-space contact shadows (directional)
        bool useSsgi = true;     // screen-space one-bounce GI
        bool ready = false;                          // sets/views valid (built after targets exist)
        glm::mat4 view{ 1.0f };                      // world -> view (G-buffer prepass)
        glm::mat4 viewProj{ 1.0f };                  // world -> clip (G-buffer prepass)
        glm::mat4 projection{ 1.0f };                // view -> clip (contact/SSGI reproject)
        glm::mat4 invProjection{ 1.0f };             // clip -> view (reconstruct view pos)
        glm::vec3 sunDirView{ 0.0f, 1.0f, 0.0f };    // direction TO the sun, view space (contact)
        f32 radius = 1.0f;
        f32 strength = 3.0f;
        vk::Sampler sampler;                         // nearest, clamp — samples the G-buffer
        // Two compute set layouts shared by the screen-space passes: 2-binding
        // (sampler + storage) and 3-binding (sampler + sampler + storage).
        vk::DescriptorSetLayout compute2Layout;
        vk::DescriptorSetLayout compute3Layout;
        vk::DescriptorSet gtaoSet;       // gbuffer + aoRaw         (compute2)
        vk::DescriptorSet aoBlurSet;     // aoRaw + gbuffer + aoMap (compute3)
        vk::DescriptorSet contactSet;    // gbuffer + contactMap    (compute2)
        vk::DescriptorSet ssgiSet;       // gbuffer + prevColor + ssgiMap (compute3)
        vk::DescriptorSet copyColorSet;  // sceneColor + prevColor  (compute2)
        vk::DescriptorSetLayout meshSetLayout;       // set 4 in the mesh pipeline (AO + contact + SSGI)
        vk::DescriptorSet meshSet;
        u32 generation = 0;                          // bumped when targets recreate (sets refreshed)
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
            : device(o.device), allocator(o.allocator), image(o.image), view(o.view),
              alloc(o.alloc), extent(o.extent), format(o.format), layout(o.layout)
        {
            o.device = nullptr; o.allocator = nullptr; o.image = nullptr; o.view = nullptr; o.alloc = nullptr;
        }
        auto operator=(Image3D&& o) noexcept -> Image3D&
        {
            if (this != &o)
            {
                reset();
                device = o.device; allocator = o.allocator; image = o.image; view = o.view;
                alloc = o.alloc; extent = o.extent; format = o.format; layout = o.layout;
                o.device = nullptr; o.allocator = nullptr; o.image = nullptr; o.view = nullptr; o.alloc = nullptr;
            }
            return *this;
        }
        ~Image3D() { reset(); }
        void reset()
        {
            if (device && view) { device.destroyImageView(view); }
            if (allocator != nullptr && image) { vmaDestroyImage(allocator, static_cast<VkImage>(image), alloc); }
            view = nullptr; image = nullptr; alloc = nullptr;
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
        u32 frameIndex = 0;        // rotates the trace ray set
        // Volume placement (world space) — fit to the scene AABB each frame.
        glm::vec3 volumeMin{ -8.0f };
        glm::vec3 volumeExtent{ 16.0f };
        glm::vec3 sunDir{ -0.5f, -1.0f, -0.3f };
        glm::vec3 sunColor{ 1.0f };
        f32 sunIntensity = 1.0f;
        glm::vec3 skyColor{ 0.1f, 0.13f, 0.2f };
        vk::Sampler sampler;       // linear clamp — atlases sampled in mesh + trace
        vk::DescriptorSetLayout voxelLayout;     // voxelize: 3D storage + box SSBO
        vk::DescriptorSetLayout traceLayout;     // trace: voxel storage + irr sampler + ray storage
        vk::DescriptorSetLayout blendIrrLayout;  // ray sampler + irr storage
        vk::DescriptorSetLayout blendDistLayout; // ray sampler + dist storage
        vk::DescriptorSetLayout borderLayout;    // irr storage
        vk::DescriptorSetLayout meshLayout;      // set 5: irr sampler + dist sampler
        vk::DescriptorSet voxelSet;
        vk::DescriptorSet traceSet;
        vk::DescriptorSet blendIrrSet;
        vk::DescriptorSet blendDistSet;
        vk::DescriptorSet borderSet;
        vk::DescriptorSet meshSet;
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
        std::array<Ref<Buffer>, MaxFramesInFlight> scratchBuffers;   // TLAS build scratch
        std::array<u32, MaxFramesInFlight> scratchCapacity{};
        u32 frameInstanceCount = 0;
        bool tlasReady = false;     // a TLAS has been built this frame (bind is valid)
        vk::DescriptorSetLayout meshLayout;  // set 6: the TLAS
        std::array<vk::DescriptorSet, MaxFramesInFlight> meshSets;
        u32 blasCount = 0;          // built BLAS count (rt-stats)
        // This frame's instance transforms + meshes, captured by renderScene + consumed by
        // the TLAS-build graph pass. Cleared each frame in beginFrame.
        std::vector<glm::mat4> frameModels;
        std::vector<Ref<GpuMesh>> frameMeshes;
        bool buildPending = false;  // RT shadows on + instances present this frame
    };

    // ReSTIR DI (reservoir spatiotemporal importance resampling) — stochastic many-light
    // direct lighting. Feeds off the froxel candidate lists (set via the cluster SSBO) +
    // the phase-7 TLAS for one visibility ray per pixel + phase-5 motion for temporal reuse.
    // Gated on rtSupported (needs ray-query). Per-pixel reservoir SSBOs (initial, combined,
    // previous) + an output radiance image the mesh adds in. Diffuse direct only in v1.
    struct Restir
    {
        bool useRestir = false;
        bool ready = false;
        bool historyReset = true;
        Image radiance;             // per-pixel resolved direct radiance (rgba16f)
        Ref<Buffer> initial;        // initial reservoirs (this frame's candidate sampling)
        Ref<Buffer> combined;       // after temporal+spatial reuse
        Ref<Buffer> previous;       // last frame's combined (temporal source)
        u32 reservoirCapacity = 0;  // pixels the buffers are sized for
        vk::Sampler sampler;        // nearest, clamp — samples G-buffer/motion
        vk::DescriptorSetLayout initialLayout;   // 4 bindings
        vk::DescriptorSetLayout reuseLayout;     // 6 bindings
        vk::DescriptorSetLayout resolveLayout;   // 6 bindings (incl. TLAS)
        vk::DescriptorSetLayout meshLayout;      // set 7: the radiance sampler
        vk::DescriptorSet initialSet;
        vk::DescriptorSet reuseSet;
        vk::DescriptorSet resolveSet;
        vk::DescriptorSet meshSet;
        u32 frameIndex = 0;
        u32 candidateCount = 16;    // K initial candidates per pixel
    };

    // The frame as a render graph + the resource handles app-authored passes reference.
    struct FrameGraphState
    {
        RenderGraph current;
        RgResource sceneColor;  // the offscreen color handle, for app-authored passes
        RgResource swapImage;
        RgResource aoResource;        // the AO map handle when SSAO ran this frame
        RgResource contactResource;   // contact-shadow map handle when contact ran
        RgResource ssgiResource;      // SSGI radiance handle when SSGI ran
        RgResource prevColorResource; // prevColor handle (imported once; read by SSGI, written by copy)
        RgResource ddgiIrradiance;    // DDGI irradiance atlas handle when DDGI ran
        RgResource ddgiDistance;      // DDGI moment atlas handle when DDGI ran
        RgResource restirRadiance;    // ReSTIR direct-radiance handle when ReSTIR ran
        bool hasAo = false;
        bool hasContact = false;
        bool hasSsgi = false;
        bool hasDdgi = false;
        bool hasGbuffer = false;      // the thin G-buffer prepass ran (screen effects or ReSTIR)
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
        Pipelines pipelines;
        Targets targets;
        Ibl ibl;
        Sky sky;
        Ssao ssao;
        Ddgi ddgi;
        Rt rt;
        Restir restir;
        FrameGraphState graph;

        bool useDepthPrepass = false;
        f32 exposureEv = 0.0f;  // tonemap exposure in stops; the tonemap pass applies exp2(this)
        glm::mat4 prevViewProj{ 1.0f };  // last frame's camera viewProj, for TAA motion vectors
        bool prevViewProjValid = false;  // false until the first frame stores one
        Ref<GpuTexture> defaultWhiteTexture;  // 1x1 white; bound when a material has no albedo
        RenderStats stats;                    // populated each frame by submitDrawList
        // Pending window screenshot, consumed in endFrame: the swapchain image is
        // only safely owned in-frame, so the copy is deferred there.
        std::optional<std::string> captureNextSwapchainPath;
        Window* window = nullptr;  // borrowed
    };

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

    // The offscreen Viewport target the editor samples + displays in a panel.
    void setViewportDesiredSize(Renderer& renderer, u32 width, u32 height);
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
        glm::mat4 normalMatrix;  // transpose(inverse(mat3(model))), correct under non-uniform scale
        glm::vec4 baseColor;
        glm::uvec4 texture{ 0 };  // .x = bindless albedo index; rest pads to std430 16 bytes
        glm::vec4 pbr{ 0.0f, 1.0f, 0.0f, 0.0f };  // x = metallic, y = roughness
        glm::vec4 emissive{ 0.0f };               // rgb = emissive radiance (strength baked in)
    };

    // Mesh rendering: a depth-tested instanced pipeline (set 0 = material albedo,
    // set 1 = directional light, set 2 = per-instance data; push constant = viewProj),
    // device-local mesh + texture uploads, and a batched instanced draw via submit().
    auto newMeshPipeline(Renderer& renderer, std::string_view shaderName, bool unlit = false) -> Result<Ref<Pipeline>>;
    // The PSO cache front door: returns the mesh pipeline for a material variant, building
    // + caching it on first request. The renderer owns it; the client never creates PSOs.
    auto requestMeshPipeline(Renderer& renderer, const Material& material) -> Ref<Pipeline>;
    // Number of distinct mesh PSOs the cache holds (inspectable to verify übershader reuse).
    auto pipelineCount(const Renderer& renderer) -> u32;
    auto uploadMesh(Renderer& renderer, const Mesh& mesh) -> Result<Ref<GpuMesh>>;
    auto uploadTexture(Renderer& renderer, const u8* rgba, u32 width, u32 height, bool srgb) -> Result<Ref<GpuTexture>>;

    // Rasterizes an SVG to a square RGBA icon (tint multiplied in) and uploads it as a
    // GPU texture — used for asset-browser type icons. "currentColor" maps to white.
    auto uploadSvgIcon(Renderer& renderer, const std::string& svgPath,
                                                              u32 pixelSize, glm::vec4 tint) -> Result<Ref<GpuTexture>>;

    // Renders a mesh to a square GPU texture (a 3/4 view framed by the mesh AABB, lit by
    // a fixed light) for an asset thumbnail. Synchronous one-off render; safe between frames.
    auto renderMeshThumbnail(Renderer& renderer, const Ref<GpuMesh>& mesh, u32 size) -> Result<Ref<GpuTexture>>;

    // Resolves each item's material to a cached PSO, batches by (pipeline, mesh, texture),
    // uploads the frame's instance buffer, and stores the structured draw list on the
    // renderer for the scene + depth passes to consume.
    void submitDrawList(Renderer& renderer, const glm::mat4& viewProj, const std::vector<DrawItem>& items);
    // Record the frame's scene geometry into the active pass (the scene-pass body).
    void recordSceneDrawList(Renderer& renderer, vk::CommandBuffer cmd);
    // Record depth-only draws of the frame's geometry (the depth-pre-pass body).
    void recordDepthPrepass(Renderer& renderer, vk::CommandBuffer cmd);

    // Per-frame visible-sky settings, resolved by renderScene from Scene.environment.
    struct SkyRenderSettings
    {
        u32 mode = 2;            // 0 = Color, 1 = Texture, 2 = Procedural
        glm::vec3 clearColor{ 0.05f, 0.06f, 0.08f };
        f32 intensity = 1.0f;
        f32 rotation = 0.0f;     // yaw radians
        bool visible = true;
        u32 textureIndex = 0;    // bindless slot of the panorama (Texture mode)
    };
    // Stores the sky settings for this frame; the sky pass (added in beginFrameGraph when
    // visible) draws from them. Leaves the pipeline/set/ready state intact.
    void submitSky(Renderer& renderer, const SkyRenderSettings& settings);
    // Record the fullscreen sky into the active pass (the sky-pass body).
    void recordSky(Renderer& renderer, vk::CommandBuffer cmd);

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
    // (grown on demand). The eye position feeds the BRDF view vector.
    void setSceneLighting(Renderer& renderer, glm::vec3 direction, glm::vec3 color, f32 intensity,
                          f32 ambient, glm::vec3 eyePosition, const std::vector<GpuLight>& lights);

    // Uploads the camera into the cluster-params UBO and arms the per-frame light-cull
    // compute dispatch (clustered forward). `proj` is the Y-flipped projection used for
    // rendering. Call once per frame after setSceneLighting, before endFrame.
    void setClusterCamera(Renderer& renderer, const glm::mat4& view, const glm::mat4& proj,
                          f32 nearPlane, f32 farPlane);

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
    void setDdgiScene(Renderer& renderer, const std::vector<glm::vec4>& boxMins,
                      const std::vector<glm::vec4>& boxMaxs, const std::vector<glm::vec4>& boxAlbedos,
                      glm::vec3 volumeMin, glm::vec3 volumeExtent,
                      glm::vec3 sunDir, glm::vec3 sunColor, f32 sunIntensity);
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
    void buildTlas(Renderer& renderer, vk::CommandBuffer cmd,
                   const std::vector<glm::mat4>& models, const std::vector<Ref<GpuMesh>>& meshes);
    // ReSTIR many-light direct lighting (feature-gated on rtSupported). Diffuse direct in v1.
    void setRestir(Renderer& renderer, bool enabled);
    auto restirEnabled(const Renderer& renderer) -> bool;
    // Records the G-buffer prepass (view normal + view-Z) for the screen-space pass bodies.
    void recordGbuffer(Renderer& renderer, vk::CommandBuffer cmd);
    // Feeds the camera the screen-space passes need: view, proj (SAME Y-flipped projection
    // the scene renders with, so the maps align), and the world-space sun direction.
    void setSsaoCamera(Renderer& renderer, const glm::mat4& view, const glm::mat4& proj,
                       glm::vec3 sunDirectionWorld);
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
    // Anti-aliasing: msaaSamples is 1 (off) / 2 / 4 / 8 (clamped to the device cap); fxaa
    // and taa toggle their post-process passes. The three modes are mutually exclusive.
    // Recreates the MSAA/TAA targets + rebuilds scene PSOs.
    void setAa(Renderer& renderer, u32 msaaSamples, bool fxaa, bool taa);
    auto aaMode(const Renderer& renderer) -> std::string;  // "off" | "fxaa" | "taa" | "msaa2|4|8"
    // Records the motion-vector prepass (camera reprojection) for the TAA pass body.
    void recordMotion(Renderer& renderer, vk::CommandBuffer cmd);

    // A 1x1 white texture; bind it when a material has no albedo.
    auto defaultTexture(const Renderer& renderer) -> const Ref<GpuTexture>&;

    // The most recent frame's scene draw counters (draw calls, batches, instances).
    auto renderStats(const Renderer& renderer) -> RenderStats;

    // Blocks until the GPU has finished all submitted work. Call before dropping
    // resource Refs at shutdown so no in-flight command buffer still references them.
    void waitGpuIdle(Renderer& renderer);

    // Copies the offscreen viewport image to a PNG. Synchronous (own submit +
    // waitIdle), safe to call between frames.
    auto captureViewport(Renderer& renderer, const std::string& path) -> Result<void>;

    // Requests a PNG of the next presented frame (written in endFrame). Fails here
    // if the surface lacks TRANSFER_SRC; otherwise returns immediately.
    auto requestWindowCapture(Renderer& renderer, std::string path) -> Result<void>;
}
