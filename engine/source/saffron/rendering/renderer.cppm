module;

// Vulkan-Hpp in no-exceptions mode: every call returns a result we convert to
// std::expected. We never use vk::raii (it throws). Classic includes (no import std).
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

export module Saffron.Rendering;

export import :RenderGraph;

import Saffron.Core;
import Saffron.Window;
import Saffron.Geometry;

export namespace se
{
    // Format of the offscreen depth buffer. D32_SFLOAT is universally supported.
    inline constexpr vk::Format DepthFormat = vk::Format::eD32Sfloat;

    // Format of the offscreen color target. RGBA8_UNORM (not the swapchain's BGRA8) so
    // it is a spec-guaranteed storage image — the post-process compute pass writes it.
    inline constexpr vk::Format OffscreenColorFormat = vk::Format::eR8G8B8A8Unorm;

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
        Pipeline& operator=(const Pipeline&) = delete;

        Pipeline(Pipeline&& other) noexcept
            : device(other.device), pipeline(other.pipeline), layout(other.layout)
        {
            other.device = nullptr;
            other.pipeline = nullptr;
            other.layout = nullptr;
        }

        Pipeline& operator=(Pipeline&& other) noexcept
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
        Image& operator=(const Image&) = delete;

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

        Image& operator=(Image&& other) noexcept
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
        std::vector<Submesh> submeshes;
        glm::vec3 boundsMin{ 0.0f };  // local-space AABB, for ray picking
        glm::vec3 boundsMax{ 0.0f };

        GpuMesh() = default;
        GpuMesh(const GpuMesh&) = delete;
        GpuMesh& operator=(const GpuMesh&) = delete;

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

        GpuMesh& operator=(GpuMesh&& other) noexcept
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
        GpuTexture& operator=(const GpuTexture&) = delete;

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

        GpuTexture& operator=(GpuTexture&& other) noexcept
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
        Buffer& operator=(const Buffer&) = delete;

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

        Buffer& operator=(Buffer&& other) noexcept
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

    struct Renderer
    {
        // vk-bootstrap keeps the bits we need for clean teardown.
        vkb::Instance vkbInstance;
        vkb::Device vkbDevice;

        vk::Instance instance;
        vk::SurfaceKHR surface;
        vk::PhysicalDevice physicalDevice;
        vk::Device device;
        vk::Queue graphicsQueue;
        u32 graphicsQueueFamily = 0;
        VmaAllocator allocator = nullptr;

        vk::SwapchainKHR swapchain;
        vk::Format swapchainFormat = vk::Format::eUndefined;
        vk::Extent2D swapchainExtent;
        bool swapchainCaptureSupported = false;  // surface allows TRANSFER_SRC (window screenshots)
        std::vector<vk::Image> swapchainImages;
        std::vector<vk::ImageView> swapchainImageViews;
        std::vector<vk::Semaphore> renderFinished;  // one per swapchain image
        std::vector<vk::Fence> imagesInFlight;       // borrowed per-frame fence per image

        std::array<FrameData, MaxFramesInFlight> frames{};
        u32 frameIndex = 0;
        u32 imageIndex = 0;

        std::array<f32, 4> clearColor{ 0.05f, 0.06f, 0.08f, 1.0f };
        SceneDrawList sceneDrawList;              // structured scene geometry for the frame
        std::vector<RenderFn> sceneSubmissions;  // ad-hoc geometry replayed into the offscreen pass
        std::vector<RenderFn> uiSubmissions;     // replayed into the swapchain pass

        // Meshes/textures/pipelines are passed around as Ref<T> objects owned by the
        // caller (AssetServer, editor), not by the Renderer; their RAII dtors free
        // the vk/VMA resources when the last Ref drops (before the allocator/device).
        vk::Sampler linearSampler;
        vk::DescriptorSetLayout bindlessSetLayout;   // set 0: bindless combined-image-sampler array
        vk::DescriptorSetLayout lightSetLayout;      // set 1: directional light UBO
        vk::DescriptorSetLayout instanceSetLayout;   // set 2: per-instance storage buffer
        vk::DescriptorPool descriptorPool;           // eFreeDescriptorSet (texture sets freed on Ref drop)
        // Bindless: one global texture array bound once; uploadTexture writes a stable
        // slot (update-after-bind) and returns its index. The default white is slot 0.
        vk::DescriptorPool bindlessPool;             // eUpdateAfterBindPool
        vk::DescriptorSet bindlessSet;               // the single set 0 for all draws
        u32 nextBindlessIndex = 0;
        // Per-frame light UBO + set (one per in-flight frame), so the host write in
        // setDirectionalLight never races a frame still reading the light on the GPU.
        std::array<vk::DescriptorSet, MaxFramesInFlight> lightSets;
        std::array<VkBuffer, MaxFramesInFlight> lightBuffers{};
        std::array<VmaAllocation, MaxFramesInFlight> lightAllocs{};
        std::array<void*, MaxFramesInFlight> lightMapped{};
        // Per-frame punctual-light storage buffer (set 1, binding 1), grown on demand.
        std::array<Ref<Buffer>, MaxFramesInFlight> lightListBuffers;
        std::array<u32, MaxFramesInFlight> lightListCapacity{};
        Ref<Pipeline> thumbnailPipeline;              // lazy mesh-thumbnail graphics pipeline
        // Post-process: an in-place compute tonemap on the offscreen color (the
        // offscreen bound as a storage image). Added to the frame graph by an app layer.
        Ref<Pipeline> tonemapPipeline;
        vk::DescriptorSetLayout tonemapSetLayout;     // compute set 0: storage image
        vk::DescriptorSet tonemapSet;                 // points at the offscreen color view (GENERAL)
        bool usePostProcess = false;
        // Depth pre-pass: a vertex-only pipeline lays down scene depth before the scene
        // pass (which then loads it + tests eLessOrEqual). Reduces shaded overdraw.
        Ref<Pipeline> depthPrepassPipeline;
        bool useDepthPrepass = false;
        // Clustered forward (Forward+): a compute pass culls the punctual lights into a
        // froxel grid each frame; the fragment loops only its cluster's lights.
        Ref<Pipeline> cullPipeline;                   // compute light-cull pipeline
        vk::DescriptorSetLayout clusterSetLayout;     // compute set 0
        std::array<vk::DescriptorSet, MaxFramesInFlight> clusterSets;     // compute
        std::array<Ref<Buffer>, MaxFramesInFlight> clusterBuffers;        // per-cluster count + indices
        std::array<VkBuffer, MaxFramesInFlight> clusterParamBuffers{};    // cluster params UBO
        std::array<VmaAllocation, MaxFramesInFlight> clusterParamAllocs{};
        std::array<void*, MaxFramesInFlight> clusterParamMapped{};
        bool useClustered = true;        // false = fragment loops all lights (reference)
        u32 frameLightCount = 0;         // punctual lights uploaded this frame
        bool clusterDispatchPending = false;
        // Per-frame instance storage buffer + set. Grown on demand (never shrunk);
        // capacity is in InstanceData elements. drawInstanced writes it each frame.
        std::array<Ref<Buffer>, MaxFramesInFlight> instanceBuffers;
        std::array<vk::DescriptorSet, MaxFramesInFlight> instanceSets;
        std::array<u32, MaxFramesInFlight> instanceCapacity{};
        Ref<GpuTexture> defaultWhiteTexture;         // 1x1 white; bound when a material has no albedo

        // PSO cache: mesh pipelines built on demand and keyed by their variant (shader +
        // render-state permutation). The übershader maps many materials to one PSO; a
        // permutation (e.g. unlit) adds an entry. The renderer owns these (not the client).
        std::unordered_map<std::string, Ref<Pipeline>> pipelineCache;

        RenderStats stats;  // populated each frame by submitDrawList

        Image offscreenViewport;       // scene render target shown in the Viewport panel
        Image offscreenDepth;          // depth buffer for the scene pass, sized to the viewport
        // MSAA: when sampleCount > 1 the scene renders to these multisampled targets and
        // resolves color into offscreenViewport. Sized to the viewport, recreated with it.
        Image msaaColor;
        Image msaaDepth;
        vk::SampleCountFlagBits sampleCount = vk::SampleCountFlagBits::e1;  // 1 = MSAA off
        vk::SampleCountFlagBits maxSampleCount = vk::SampleCountFlagBits::e1;  // device cap
        bool fxaaEnabled = false;      // FXAA post-process (mutually exclusive with MSAA)
        u32 viewportDesiredWidth = 0;  // requested by the UI panel (applied next frame)
        u32 viewportDesiredHeight = 0;
        u32 viewportGeneration = 0;    // bumped whenever the offscreen image is recreated

        // The frame as a render graph: built in beginFrameGraph (cull + scene), extended
        // by layers via the onRenderGraph hook, finished + executed in endFrame.
        RenderGraph renderGraph;
        RgResource frameSceneColor;  // the offscreen color handle, for app-authored passes
        RgResource frameSwapImage;

        // Pending window screenshot, consumed in endFrame: the swapchain image is
        // only safely owned in-frame, so the copy is deferred there.
        std::optional<std::string> captureNextSwapchainPath;

        Window* window = nullptr;  // borrowed
    };

    std::expected<Renderer, std::string> newRenderer(Window& window);
    void destroyRenderer(Renderer& renderer);

    bool beginFrame(Renderer& renderer);
    void submit(Renderer& renderer, RenderFn fn);    // scene pass (offscreen target)
    void submitUi(Renderer& renderer, RenderFn fn);  // ui pass (swapchain)
    // Build the frame's graph (cull + scene). The run loop then lets layers add passes
    // (onRenderGraph) before endFrame finishes it with the ui pass + executes it.
    void beginFrameGraph(Renderer& renderer);
    RenderGraph& frameGraph(Renderer& renderer);
    // The offscreen color resource in the current frame graph — an app-authored pass
    // (e.g. post-process) declares its reads/writes against this handle.
    RgResource viewportColorResource(const Renderer& renderer);
    // Add an in-place post-process tonemap pass on the offscreen color (app-authored;
    // called from a layer's onRenderGraph when post-process is enabled).
    void addTonemapPass(Renderer& renderer, RenderGraph& graph);
    void endFrame(Renderer& renderer);

    // The offscreen Viewport target the editor samples + displays in a panel.
    void setViewportDesiredSize(Renderer& renderer, u32 width, u32 height);
    vk::ImageView viewportImageView(const Renderer& renderer);
    u32 viewportGeneration(const Renderer& renderer);
    u32 viewportWidth(const Renderer& renderer);
    u32 viewportHeight(const Renderer& renderer);

    std::string assetPath(std::string_view relative);

    // One entry per drawn entity in the per-frame instance storage buffer (set 2).
    // The vertex shader indexes it by InstanceIndex (firstInstance + gl_InstanceID).
    // std430-compatible: every member is 16-byte aligned.
    struct InstanceData
    {
        glm::mat4 model;
        glm::mat4 normalMatrix;  // transpose(inverse(mat3(model))), correct under non-uniform scale
        glm::vec4 baseColor;
        glm::uvec4 texture{ 0 };  // .x = bindless albedo index; rest pads to std430 16 bytes
    };

    // Mesh rendering: a depth-tested instanced pipeline (set 0 = material albedo,
    // set 1 = directional light, set 2 = per-instance data; push constant = viewProj),
    // device-local mesh + texture uploads, and a batched instanced draw via submit().
    std::expected<Ref<Pipeline>, std::string> newMeshPipeline(Renderer& renderer, std::string_view shaderName, bool unlit = false);
    // The PSO cache front door: returns the mesh pipeline for a material variant, building
    // + caching it on first request. The renderer owns it; the client never creates PSOs.
    Ref<Pipeline> requestMeshPipeline(Renderer& renderer, const Material& material);
    // Number of distinct mesh PSOs the cache holds (inspectable to verify übershader reuse).
    u32 pipelineCount(const Renderer& renderer);
    std::expected<Ref<GpuMesh>, std::string> uploadMesh(Renderer& renderer, const Mesh& mesh);
    std::expected<Ref<GpuTexture>, std::string> uploadTexture(Renderer& renderer, const u8* rgba, u32 width, u32 height, bool srgb);

    // Rasterizes an SVG to a square RGBA icon (tint multiplied in) and uploads it as a
    // GPU texture — used for asset-browser type icons. "currentColor" maps to white.
    std::expected<Ref<GpuTexture>, std::string> uploadSvgIcon(Renderer& renderer, const std::string& svgPath,
                                                              u32 pixelSize, glm::vec4 tint);

    // Renders a mesh to a square GPU texture (a 3/4 view framed by the mesh AABB, lit by
    // a fixed light) for an asset thumbnail. Synchronous one-off render; safe between frames.
    std::expected<Ref<GpuTexture>, std::string> renderMeshThumbnail(Renderer& renderer, const Ref<GpuMesh>& mesh, u32 size);

    // Resolves each item's material to a cached PSO, batches by (pipeline, mesh, texture),
    // uploads the frame's instance buffer, and stores the structured draw list on the
    // renderer for the scene + depth passes to consume.
    void submitDrawList(Renderer& renderer, const glm::mat4& viewProj, const std::vector<DrawItem>& items);
    // Record the frame's scene geometry into the active pass (the scene-pass body).
    void recordSceneDrawList(Renderer& renderer, vk::CommandBuffer cmd);
    // Record depth-only draws of the frame's geometry (the depth-pre-pass body).
    void recordDepthPrepass(Renderer& renderer, vk::CommandBuffer cmd);

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

    // Writes the whole per-frame lighting state: the directional light + ambient into
    // the UBO and the punctual lights into the storage buffer (grown on demand).
    void setSceneLighting(Renderer& renderer, glm::vec3 direction, glm::vec3 color, f32 intensity,
                          f32 ambient, const std::vector<GpuLight>& lights);

    // Uploads the camera into the cluster-params UBO and arms the per-frame light-cull
    // compute dispatch (clustered forward). `proj` is the Y-flipped projection used for
    // rendering. Call once per frame after setSceneLighting, before endFrame.
    void setClusterCamera(Renderer& renderer, const glm::mat4& view, const glm::mat4& proj,
                          f32 nearPlane, f32 farPlane);

    // Toggles clustered light culling. When off, the fragment shader loops every light
    // (the reference path) — useful for A/B verification.
    void setClustered(Renderer& renderer, bool enabled);
    bool clusteredEnabled(const Renderer& renderer);
    void setPostProcess(Renderer& renderer, bool enabled);
    bool postProcessEnabled(const Renderer& renderer);
    void setDepthPrepass(Renderer& renderer, bool enabled);
    bool depthPrepassEnabled(const Renderer& renderer);
    // Anti-aliasing: msaaSamples is 1 (off) / 2 / 4 / 8 (clamped to the device cap); fxaa
    // toggles the post-process pass. Recreates the MSAA targets + rebuilds scene PSOs.
    void setAa(Renderer& renderer, u32 msaaSamples, bool fxaa);
    std::string aaMode(const Renderer& renderer);  // "off" | "fxaa" | "msaa2|4|8"

    // A 1x1 white texture; bind it when a material has no albedo.
    const Ref<GpuTexture>& defaultTexture(const Renderer& renderer);

    // The most recent frame's scene draw counters (draw calls, batches, instances).
    RenderStats renderStats(const Renderer& renderer);

    // Blocks until the GPU has finished all submitted work. Call before dropping
    // resource Refs at shutdown so no in-flight command buffer still references them.
    void waitGpuIdle(Renderer& renderer);

    // Copies the offscreen viewport image to a PNG. Synchronous (own submit +
    // waitIdle), safe to call between frames.
    std::expected<void, std::string> captureViewport(Renderer& renderer, const std::string& path);

    // Requests a PNG of the next presented frame (written in endFrame). Fails here
    // if the surface lacks TRANSFER_SRC; otherwise returns immediately.
    std::expected<void, std::string> requestWindowCapture(Renderer& renderer, std::string path);
}

namespace se
{
    namespace
    {
        // Converts a Vulkan-Hpp ResultValue to std::expected, checked at the call site.
        template <typename T>
        std::expected<T, std::string> checked(vk::ResultValue<T> rv, std::string_view what)
        {
            if (rv.result != vk::Result::eSuccess)
            {
                return std::unexpected(std::format("{}: {}", what, vk::to_string(rv.result)));
            }
            return std::move(rv.value);
        }

        std::expected<void, std::string> checked(vk::Result result, std::string_view what)
        {
            if (result != vk::Result::eSuccess)
            {
                return std::unexpected(std::format("{}: {}", what, vk::to_string(result)));
            }
            return {};
        }

        void transitionImage(
            vk::CommandBuffer cmd,
            vk::Image image,
            vk::ImageLayout oldLayout,
            vk::ImageLayout newLayout,
            vk::PipelineStageFlags2 srcStage,
            vk::AccessFlags2 srcAccess,
            vk::PipelineStageFlags2 dstStage,
            vk::AccessFlags2 dstAccess,
            vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor)
        {
            vk::ImageMemoryBarrier2 barrier{};
            barrier.srcStageMask = srcStage;
            barrier.srcAccessMask = srcAccess;
            barrier.dstStageMask = dstStage;
            barrier.dstAccessMask = dstAccess;
            barrier.oldLayout = oldLayout;
            barrier.newLayout = newLayout;
            barrier.image = image;
            barrier.subresourceRange = vk::ImageSubresourceRange{ aspect, 0, 1, 0, 1 };

            vk::DependencyInfo dependency{};
            dependency.setImageMemoryBarriers(barrier);
            cmd.pipelineBarrier2(dependency);
        }

        void destroySwapchainResources(Renderer& renderer)
        {
            for (vk::ImageView view : renderer.swapchainImageViews)
            {
                renderer.device.destroyImageView(view);
            }
            renderer.swapchainImageViews.clear();

            for (vk::Semaphore semaphore : renderer.renderFinished)
            {
                renderer.device.destroySemaphore(semaphore);
            }
            renderer.renderFinished.clear();

            if (renderer.swapchain)
            {
                renderer.device.destroySwapchainKHR(renderer.swapchain);
                renderer.swapchain = nullptr;
            }
        }

        std::expected<void, std::string> buildSwapchain(Renderer& renderer, u32 width, u32 height)
        {
            // TRANSFER_SRC on swapchain images is not spec-guaranteed (only
            // COLOR_ATTACHMENT is). Query support and only request it when present
            // so an exotic surface disables window screenshots rather than failing
            // the whole swapchain build.
            renderer.swapchainCaptureSupported = false;
            vk::ResultValue<vk::SurfaceCapabilitiesKHR> caps =
                renderer.physicalDevice.getSurfaceCapabilitiesKHR(renderer.surface);
            if (caps.result == vk::Result::eSuccess &&
                (caps.value.supportedUsageFlags & vk::ImageUsageFlagBits::eTransferSrc))
            {
                renderer.swapchainCaptureSupported = true;
            }

            VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (renderer.swapchainCaptureSupported)
            {
                usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            }

            vkb::SwapchainBuilder builder{ renderer.vkbDevice };
            builder.set_desired_format(VkSurfaceFormatKHR{
                       .format = VK_FORMAT_B8G8R8A8_UNORM,
                       .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
                .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                .set_desired_extent(width, height)
                .add_image_usage_flags(usage);

            if (renderer.swapchain)
            {
                builder.set_old_swapchain(static_cast<VkSwapchainKHR>(renderer.swapchain));
            }

            auto result = builder.build();
            if (!result)
            {
                return std::unexpected(std::format("swapchain build failed: {}", result.error().message()));
            }

            destroySwapchainResources(renderer);

            vkb::Swapchain swapchain = result.value();
            renderer.swapchain = vk::SwapchainKHR{ swapchain.swapchain };
            renderer.swapchainFormat = vk::Format{ static_cast<vk::Format>(swapchain.image_format) };
            renderer.swapchainExtent = vk::Extent2D{ swapchain.extent.width, swapchain.extent.height };

            renderer.swapchainImages.clear();
            for (VkImage image : swapchain.get_images().value())
            {
                renderer.swapchainImages.push_back(vk::Image{ image });
            }

            renderer.swapchainImageViews.clear();
            for (vk::Image image : renderer.swapchainImages)
            {
                vk::ImageViewCreateInfo viewInfo{};
                viewInfo.image = image;
                viewInfo.viewType = vk::ImageViewType::e2D;
                viewInfo.format = renderer.swapchainFormat;
                viewInfo.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
                auto view = checked(renderer.device.createImageView(viewInfo), "createImageView");
                if (!view)
                {
                    return std::unexpected(view.error());
                }
                renderer.swapchainImageViews.push_back(*view);
            }

            renderer.renderFinished.clear();
            for (std::size_t i = 0; i < renderer.swapchainImages.size(); i = i + 1)
            {
                auto semaphore = checked(renderer.device.createSemaphore(vk::SemaphoreCreateInfo{}), "createSemaphore");
                if (!semaphore)
                {
                    return std::unexpected(semaphore.error());
                }
                renderer.renderFinished.push_back(*semaphore);
            }

            renderer.imagesInFlight.assign(renderer.swapchainImages.size(), vk::Fence{});
            return {};
        }

        void recreateSwapchain(Renderer& renderer)
        {
            u32 width = renderer.window->width;
            u32 height = renderer.window->height;
            if (width == 0 || height == 0)
            {
                return;  // minimized — keep the old swapchain, retry once restored
            }
            static_cast<void>(renderer.device.waitIdle());
            auto built = buildSwapchain(renderer, width, height);
            if (!built)
            {
                logError(built.error());
            }
        }

        std::expected<vk::ShaderModule, std::string> loadShaderModule(vk::Device device, const std::string& path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
            {
                return std::unexpected(std::format("cannot open shader '{}'", path));
            }
            std::streamsize size = file.tellg();
            if (size <= 0 || (size % 4) != 0)
            {
                return std::unexpected(std::format("invalid spir-v size for '{}'", path));
            }
            std::vector<u32> code(static_cast<std::size_t>(size) / 4);
            file.seekg(0);
            file.read(reinterpret_cast<char*>(code.data()), size);

            vk::ShaderModuleCreateInfo info{};
            info.codeSize = static_cast<std::size_t>(size);
            info.pCode = code.data();
            return checked(device.createShaderModule(info), std::format("createShaderModule '{}'", path));
        }

        std::expected<Image, std::string> newColorImage(Renderer& renderer, u32 width, u32 height,
                                                        vk::Format format, bool storage = false,
                                                        vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1)
        {
            vk::FormatProperties props = renderer.physicalDevice.getFormatProperties(format);
            vk::FormatFeatureFlags needed =
                vk::FormatFeatureFlagBits::eColorAttachment | vk::FormatFeatureFlagBits::eSampledImage;
            if (storage)
            {
                needed = needed | vk::FormatFeatureFlagBits::eStorageImage;
            }
            if ((props.optimalTilingFeatures & needed) != needed)
            {
                return std::unexpected(std::format("format {} cannot be a sampled color attachment", vk::to_string(format)));
            }

            // A multisampled image is a transient resolve source: color attachment only
            // (never sampled / stored / copied — the resolved 1x image is what we read).
            const bool multisampled = samples != vk::SampleCountFlagBits::e1;
            VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = static_cast<VkFormat>(format);
            imageInfo.extent = VkExtent3D{ width, height, 1 };
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = static_cast<VkSampleCountFlagBits>(samples);
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            if (!multisampled)
            {
                imageInfo.usage = imageInfo.usage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                if (storage)
                {
                    imageInfo.usage = imageInfo.usage | VK_IMAGE_USAGE_STORAGE_BIT;
                }
            }
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

            VkImage rawImage = VK_NULL_HANDLE;
            VmaAllocation allocation = nullptr;
            if (vmaCreateImage(renderer.allocator, &imageInfo, &allocInfo, &rawImage, &allocation, nullptr) != VK_SUCCESS)
            {
                return std::unexpected(std::string{ "vmaCreateImage failed for offscreen target" });
            }

            vk::ImageViewCreateInfo viewInfo{};
            viewInfo.image = vk::Image{ rawImage };
            viewInfo.viewType = vk::ImageViewType::e2D;
            viewInfo.format = format;
            viewInfo.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
            vk::ResultValue<vk::ImageView> view = renderer.device.createImageView(viewInfo);
            if (view.result != vk::Result::eSuccess)
            {
                vmaDestroyImage(renderer.allocator, rawImage, allocation);
                return std::unexpected(std::format("createImageView (offscreen): {}", vk::to_string(view.result)));
            }

            Image result;
            result.device = renderer.device;
            result.allocator = renderer.allocator;
            result.image = vk::Image{ rawImage };
            result.view = view.value;
            result.alloc = allocation;
            result.extent = vk::Extent2D{ width, height };
            result.format = format;
            result.layout = vk::ImageLayout::eUndefined;
            return result;
        }

        std::expected<Image, std::string> newDepthImage(Renderer& renderer, u32 width, u32 height,
                                                        vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1)
        {
            VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = static_cast<VkFormat>(DepthFormat);
            imageInfo.extent = VkExtent3D{ width, height, 1 };
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = static_cast<VkSampleCountFlagBits>(samples);
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

            VkImage rawImage = VK_NULL_HANDLE;
            VmaAllocation allocation = nullptr;
            if (vmaCreateImage(renderer.allocator, &imageInfo, &allocInfo, &rawImage, &allocation, nullptr) != VK_SUCCESS)
            {
                return std::unexpected(std::string{ "vmaCreateImage failed for depth target" });
            }

            vk::ImageViewCreateInfo viewInfo{};
            viewInfo.image = vk::Image{ rawImage };
            viewInfo.viewType = vk::ImageViewType::e2D;
            viewInfo.format = DepthFormat;
            viewInfo.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1 };
            vk::ResultValue<vk::ImageView> view = renderer.device.createImageView(viewInfo);
            if (view.result != vk::Result::eSuccess)
            {
                vmaDestroyImage(renderer.allocator, rawImage, allocation);
                return std::unexpected(std::format("createImageView (depth): {}", vk::to_string(view.result)));
            }

            Image result;
            result.device = renderer.device;
            result.allocator = renderer.allocator;
            result.image = vk::Image{ rawImage };
            result.view = view.value;
            result.alloc = allocation;
            result.extent = vk::Extent2D{ width, height };
            result.format = DepthFormat;
            result.layout = vk::ImageLayout::eUndefined;
            return result;
        }

        // (Re)create the multisampled scene color + depth targets at the offscreen extent
        // when MSAA is on; drop them when off. Called after the offscreen is sized + on a
        // sample-count change. The caller has already idled the GPU.
        void recreateMsaaTargets(Renderer& renderer)
        {
            renderer.msaaColor.reset();
            renderer.msaaDepth.reset();
            if (renderer.sampleCount == vk::SampleCountFlagBits::e1)
            {
                return;
            }
            const u32 w = renderer.offscreenViewport.extent.width;
            const u32 h = renderer.offscreenViewport.extent.height;
            if (w == 0 || h == 0)
            {
                return;
            }
            std::expected<Image, std::string> color =
                newColorImage(renderer, w, h, OffscreenColorFormat, false, renderer.sampleCount);
            if (color)
            {
                renderer.msaaColor = std::move(*color);
            }
            else
            {
                logError(color.error());
            }
            std::expected<Image, std::string> depth = newDepthImage(renderer, w, h, renderer.sampleCount);
            if (depth)
            {
                renderer.msaaDepth = std::move(*depth);
            }
            else
            {
                logError(depth.error());
            }
        }

        // Host-visible, mapped buffer the caller owns (vmaDestroyBuffer when done).
        std::expected<void, std::string> newHostCaptureBuffer(
            Renderer& renderer, vk::DeviceSize bytes,
            VkBuffer& outBuffer, VmaAllocation& outAlloc, VmaAllocationInfo& outInfo)
        {
            VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bufferInfo.size = bytes;
            bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            if (vmaCreateBuffer(renderer.allocator, &bufferInfo, &allocInfo, &outBuffer, &outAlloc, &outInfo) != VK_SUCCESS)
            {
                return std::unexpected(std::string{ "capture: vmaCreateBuffer failed" });
            }
            return {};
        }

        // Records a fromLayout->TransferSrc barrier, the image->buffer copy, and a
        // TransferSrc->toLayout barrier into a caller-owned command buffer.
        void captureImageToBuffer(
            vk::CommandBuffer cmd, vk::Image image, vk::Extent2D extent,
            vk::ImageLayout fromLayout, vk::PipelineStageFlags2 fromStage, vk::AccessFlags2 fromAccess,
            vk::ImageLayout toLayout, vk::PipelineStageFlags2 toStage, vk::AccessFlags2 toAccess,
            vk::Buffer destination)
        {
            transitionImage(
                cmd, image, fromLayout, vk::ImageLayout::eTransferSrcOptimal,
                fromStage, fromAccess,
                vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);

            vk::BufferImageCopy region{};
            region.imageSubresource = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
            region.imageExtent = vk::Extent3D{ extent.width, extent.height, 1 };
            cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, destination, region);

            transitionImage(
                cmd, image, vk::ImageLayout::eTransferSrcOptimal, toLayout,
                vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead,
                toStage, toAccess);
        }

        // Writes a PNG, reordering BGRA source pixels to RGB.
        std::expected<void, std::string> writeBufferToPng(
            const unsigned char* pixels, u32 width, u32 height, vk::Format format, const std::string& path)
        {
            const bool bgr = format == vk::Format::eB8G8R8A8Unorm || format == vk::Format::eB8G8R8A8Srgb;
            std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * height * 3);
            for (u32 i = 0; i < width * height; i = i + 1)
            {
                u32 r = i * 4 + 0;
                u32 b = i * 4 + 2;
                if (bgr)
                {
                    r = i * 4 + 2;
                    b = i * 4 + 0;
                }
                rgb[i * 3 + 0] = pixels[r];
                rgb[i * 3 + 1] = pixels[i * 4 + 1];
                rgb[i * 3 + 2] = pixels[b];
            }
            const int ok = stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height),
                                          3, rgb.data(), static_cast<int>(width) * 3);
            if (ok == 0)
            {
                return std::unexpected(std::format("stbi_write_png failed for '{}'", path));
            }
            return {};
        }

        // Matches the shader's set 1 light uniform (std140).
        struct LightUbo
        {
            glm::vec4 directionAmbient;  // xyz direction, w ambient
            glm::vec4 colorIntensity;    // rgb color, a intensity
            glm::uvec4 counts;           // x = punctual light count
        };

        // Froxel cluster grid (X x Y screen tiles, Z exponential view-space slices) and
        // the per-cluster light cap. Must match light_cull.slang + mesh.slang.
        inline constexpr u32 ClusterGridX = 16;
        inline constexpr u32 ClusterGridY = 9;
        inline constexpr u32 ClusterGridZ = 24;
        inline constexpr u32 ClusterCount = ClusterGridX * ClusterGridY * ClusterGridZ;
        inline constexpr u32 MaxLightsPerCluster = 64;
        // One cluster's light list: a count + a fixed slot of indices. Matches the
        // shader's Cluster struct (std430: tight u32 array).
        inline constexpr vk::DeviceSize ClusterStride = sizeof(u32) * (1 + MaxLightsPerCluster);

        // Matches both shaders' cluster-params UBO (std140).
        struct ClusterParams
        {
            glm::mat4 view;               // world -> view (cull: light positions; fragment: froxel Z)
            glm::mat4 inverseProjection;  // clip -> view (cull: tile AABB build)
            glm::uvec4 gridSize;          // xyz = grid dims, w = punctual light count
            glm::uvec4 screenSize;        // xy = offscreen pixel dims, z = clustered flag
            glm::vec4 zPlanes;            // x = near, y = far
        };

        // Initial punctual-light buffer capacity, grown on demand thereafter.
        inline constexpr u32 LightListInitial = 16;

        // A host-visible, persistently mapped storage buffer for per-frame uploads.
        std::expected<Ref<Buffer>, std::string> makeMappedStorageBuffer(Renderer& renderer, vk::DeviceSize bytes)
        {
            VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size = bytes;
            info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo alloc{};
            alloc.usage = VMA_MEMORY_USAGE_AUTO;
            alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VkBuffer raw = VK_NULL_HANDLE;
            VmaAllocation allocation = nullptr;
            VmaAllocationInfo mapped{};
            if (vmaCreateBuffer(renderer.allocator, &info, &alloc, &raw, &allocation, &mapped) != VK_SUCCESS)
            {
                return std::unexpected(std::string{ "makeMappedStorageBuffer: vmaCreateBuffer failed" });
            }
            Buffer buffer;
            buffer.allocator = renderer.allocator;
            buffer.buffer = vk::Buffer{ raw };
            buffer.alloc = allocation;
            buffer.mapped = mapped.pMappedData;
            buffer.size = bytes;
            return std::make_shared<Buffer>(std::move(buffer));
        }

        // A device-local storage buffer (no host access) — for GPU-only scratch like
        // the compute-written cluster light lists.
        std::expected<Ref<Buffer>, std::string> makeDeviceStorageBuffer(Renderer& renderer, vk::DeviceSize bytes)
        {
            VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size = bytes;
            info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo alloc{};
            alloc.usage = VMA_MEMORY_USAGE_AUTO;
            VkBuffer raw = VK_NULL_HANDLE;
            VmaAllocation allocation = nullptr;
            if (vmaCreateBuffer(renderer.allocator, &info, &alloc, &raw, &allocation, nullptr) != VK_SUCCESS)
            {
                return std::unexpected(std::string{ "makeDeviceStorageBuffer: vmaCreateBuffer failed" });
            }
            Buffer buffer;
            buffer.allocator = renderer.allocator;
            buffer.buffer = vk::Buffer{ raw };
            buffer.alloc = allocation;
            buffer.size = bytes;
            return std::make_shared<Buffer>(std::move(buffer));
        }

        // Builds a compute pipeline from a SPIR-V module (entry "computeMain") + a single
        // descriptor set layout. Returned as a Ref<Pipeline> (move-only RAII).
        std::expected<Ref<Pipeline>, std::string> newComputePipeline(
            Renderer& renderer, std::string_view shaderName, vk::DescriptorSetLayout setLayout)
        {
            auto moduleResult = loadShaderModule(renderer.device, assetPath(shaderName));
            if (!moduleResult)
            {
                return std::unexpected(moduleResult.error());
            }
            vk::ShaderModule shaderModule = *moduleResult;

            vk::PipelineLayoutCreateInfo layoutInfo{};
            layoutInfo.setSetLayouts(setLayout);
            auto layoutResult = checked(renderer.device.createPipelineLayout(layoutInfo), "createPipelineLayout (compute)");
            if (!layoutResult)
            {
                renderer.device.destroyShaderModule(shaderModule);
                return std::unexpected(layoutResult.error());
            }

            vk::PipelineShaderStageCreateInfo stage{};
            stage.stage = vk::ShaderStageFlagBits::eCompute;
            stage.module = shaderModule;
            stage.pName = "computeMain";

            vk::ComputePipelineCreateInfo pipelineInfo{};
            pipelineInfo.stage = stage;
            pipelineInfo.layout = *layoutResult;
            vk::ResultValue<vk::Pipeline> created = renderer.device.createComputePipeline(nullptr, pipelineInfo);
            renderer.device.destroyShaderModule(shaderModule);
            if (created.result != vk::Result::eSuccess)
            {
                renderer.device.destroyPipelineLayout(*layoutResult);
                return std::unexpected(std::format("createComputePipeline: {}", vk::to_string(created.result)));
            }

            Pipeline pipeline;
            pipeline.device = renderer.device;
            pipeline.pipeline = created.value;
            pipeline.layout = *layoutResult;
            return std::make_shared<Pipeline>(std::move(pipeline));
        }

        // A vertex-only graphics pipeline for the depth pre-pass: it reuses the mesh
        // vertex shader (instance set 2 + viewProj push constant) but has no fragment
        // shader and no color attachment, so it only lays down depth (test+write LESS).
        // Its pipeline layout matches the mesh layout (so the same set 2 + push bind).
        std::expected<Ref<Pipeline>, std::string> makeDepthPrepassPipeline(Renderer& renderer, std::string_view shaderName)
        {
            auto moduleResult = loadShaderModule(renderer.device, assetPath(shaderName));
            if (!moduleResult)
            {
                return std::unexpected(moduleResult.error());
            }
            vk::ShaderModule shaderModule = *moduleResult;

            vk::PipelineShaderStageCreateInfo stage{};
            stage.stage = vk::ShaderStageFlagBits::eVertex;
            stage.module = shaderModule;
            stage.pName = "vertexMain";

            vk::VertexInputBindingDescription binding{};
            binding.binding = 0;
            binding.stride = sizeof(Vertex);
            binding.inputRate = vk::VertexInputRate::eVertex;
            std::array<vk::VertexInputAttributeDescription, 3> attributes{
                vk::VertexInputAttributeDescription{ 0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position) },
                vk::VertexInputAttributeDescription{ 1, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, normal) },
                vk::VertexInputAttributeDescription{ 2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv0) } };
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
            multisample.rasterizationSamples = renderer.sampleCount;  // match the MSAA target
            vk::PipelineDepthStencilStateCreateInfo depthStencil{};
            depthStencil.depthTestEnable = VK_TRUE;
            depthStencil.depthWriteEnable = VK_TRUE;
            depthStencil.depthCompareOp = vk::CompareOp::eLess;
            vk::PipelineColorBlendStateCreateInfo colorBlend{};  // no color attachments
            std::array<vk::DynamicState, 2> dynamicStates{ vk::DynamicState::eViewport, vk::DynamicState::eScissor };
            vk::PipelineDynamicStateCreateInfo dynamic{};
            dynamic.setDynamicStates(dynamicStates);

            vk::PipelineRenderingCreateInfo renderingInfo{};
            renderingInfo.depthAttachmentFormat = DepthFormat;

            vk::PushConstantRange pushConstant{};
            pushConstant.stageFlags = vk::ShaderStageFlagBits::eVertex;
            pushConstant.offset = 0;
            pushConstant.size = sizeof(glm::mat4);
            std::array<vk::DescriptorSetLayout, 3> setLayouts{
                renderer.bindlessSetLayout, renderer.lightSetLayout, renderer.instanceSetLayout };
            vk::PipelineLayoutCreateInfo layoutInfo{};
            layoutInfo.setSetLayouts(setLayouts);
            layoutInfo.setPushConstantRanges(pushConstant);
            auto layoutResult = checked(renderer.device.createPipelineLayout(layoutInfo), "createPipelineLayout (depth-prepass)");
            if (!layoutResult)
            {
                renderer.device.destroyShaderModule(shaderModule);
                return std::unexpected(layoutResult.error());
            }

            vk::GraphicsPipelineCreateInfo pipelineInfo{};
            pipelineInfo.pNext = &renderingInfo;
            pipelineInfo.setStages(stage);
            pipelineInfo.pVertexInputState = &vertexInput;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewportState;
            pipelineInfo.pRasterizationState = &raster;
            pipelineInfo.pMultisampleState = &multisample;
            pipelineInfo.pDepthStencilState = &depthStencil;
            pipelineInfo.pColorBlendState = &colorBlend;
            pipelineInfo.pDynamicState = &dynamic;
            pipelineInfo.layout = *layoutResult;

            vk::ResultValue<vk::Pipeline> created = renderer.device.createGraphicsPipeline(nullptr, pipelineInfo);
            renderer.device.destroyShaderModule(shaderModule);
            if (created.result != vk::Result::eSuccess)
            {
                renderer.device.destroyPipelineLayout(*layoutResult);
                return std::unexpected(std::format("createGraphicsPipeline (depth-prepass): {}", vk::to_string(created.result)));
            }

            Pipeline pipeline;
            pipeline.device = renderer.device;
            pipeline.pipeline = created.value;
            pipeline.layout = *layoutResult;
            return std::make_shared<Pipeline>(std::move(pipeline));
        }

        // Write a texture into the bindless array at the given slot (set 0, binding 0).
        // update-after-bind: safe to write a live set between draws.
        void writeBindlessTexture(Renderer& renderer, vk::ImageView view, u32 index)
        {
            vk::DescriptorImageInfo info{ renderer.linearSampler, view, vk::ImageLayout::eShaderReadOnlyOptimal };
            vk::WriteDescriptorSet write{};
            write.dstSet = renderer.bindlessSet;
            write.dstBinding = 0;
            write.dstArrayElement = index;
            write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            write.setImageInfo(info);
            renderer.device.updateDescriptorSets(write, {});
        }

        // Point the tonemap set's storage-image binding at the current offscreen color
        // view (GENERAL layout). Called after the offscreen color is (re)created.
        void updateTonemapSet(Renderer& renderer)
        {
            if (!renderer.tonemapSet)
            {
                return;
            }
            vk::DescriptorImageInfo imageInfo{};
            imageInfo.imageView = renderer.offscreenViewport.view;
            imageInfo.imageLayout = vk::ImageLayout::eGeneral;
            vk::WriteDescriptorSet write{};
            write.dstSet = renderer.tonemapSet;
            write.dstBinding = 0;
            write.descriptorType = vk::DescriptorType::eStorageImage;
            write.setImageInfo(imageInfo);
            renderer.device.updateDescriptorSets(write, {});
        }

        // The shared sampler, material/light set layouts, descriptor pool, and the
        // per-frame light UBO + its set. Called once in newRenderer.
        std::expected<void, std::string> initDescriptorResources(Renderer& renderer)
        {
            vk::SamplerCreateInfo samplerInfo{};
            samplerInfo.magFilter = vk::Filter::eLinear;
            samplerInfo.minFilter = vk::Filter::eLinear;
            samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
            samplerInfo.addressModeU = vk::SamplerAddressMode::eRepeat;
            samplerInfo.addressModeV = vk::SamplerAddressMode::eRepeat;
            samplerInfo.addressModeW = vk::SamplerAddressMode::eRepeat;
            samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
            auto sampler = checked(renderer.device.createSampler(samplerInfo), "createSampler");
            if (!sampler)
            {
                return std::unexpected(sampler.error());
            }
            renderer.linearSampler = *sampler;

            // Set 0 is the bindless albedo array: a runtime-sized combined-image-sampler
            // array, partially bound (not every slot filled) + update-after-bind (new
            // textures written into live slots). Indexed per-instance in the shader.
            vk::DescriptorSetLayoutBinding albedoBinding{};
            albedoBinding.binding = 0;
            albedoBinding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            albedoBinding.descriptorCount = MaxBindlessTextures;
            albedoBinding.stageFlags = vk::ShaderStageFlagBits::eFragment;
            vk::DescriptorBindingFlags bindlessFlags =
                vk::DescriptorBindingFlagBits::ePartiallyBound | vk::DescriptorBindingFlagBits::eUpdateAfterBind;
            vk::DescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
            bindingFlagsInfo.setBindingFlags(bindlessFlags);
            vk::DescriptorSetLayoutCreateInfo materialLayoutInfo{};
            materialLayoutInfo.setBindings(albedoBinding);
            materialLayoutInfo.flags = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
            materialLayoutInfo.pNext = &bindingFlagsInfo;
            auto materialLayout = checked(renderer.device.createDescriptorSetLayout(materialLayoutInfo), "bindlessSetLayout");
            if (!materialLayout)
            {
                return std::unexpected(materialLayout.error());
            }
            renderer.bindlessSetLayout = *materialLayout;

            std::array<vk::DescriptorSetLayoutBinding, 4> lightBindings{};
            lightBindings[0].binding = 0;  // directional + ambient + counts UBO
            lightBindings[0].descriptorType = vk::DescriptorType::eUniformBuffer;
            lightBindings[0].descriptorCount = 1;
            lightBindings[0].stageFlags = vk::ShaderStageFlagBits::eFragment;
            lightBindings[1].binding = 1;  // punctual light storage buffer
            lightBindings[1].descriptorType = vk::DescriptorType::eStorageBuffer;
            lightBindings[1].descriptorCount = 1;
            lightBindings[1].stageFlags = vk::ShaderStageFlagBits::eFragment;
            lightBindings[2].binding = 2;  // per-cluster light lists (read)
            lightBindings[2].descriptorType = vk::DescriptorType::eStorageBuffer;
            lightBindings[2].descriptorCount = 1;
            lightBindings[2].stageFlags = vk::ShaderStageFlagBits::eFragment;
            lightBindings[3].binding = 3;  // cluster params UBO
            lightBindings[3].descriptorType = vk::DescriptorType::eUniformBuffer;
            lightBindings[3].descriptorCount = 1;
            lightBindings[3].stageFlags = vk::ShaderStageFlagBits::eFragment;
            vk::DescriptorSetLayoutCreateInfo lightLayoutInfo{};
            lightLayoutInfo.setBindings(lightBindings);
            auto lightLayout = checked(renderer.device.createDescriptorSetLayout(lightLayoutInfo), "lightSetLayout");
            if (!lightLayout)
            {
                return std::unexpected(lightLayout.error());
            }
            renderer.lightSetLayout = *lightLayout;

            std::array<vk::DescriptorSetLayoutBinding, 3> clusterBindings{};
            clusterBindings[0].binding = 0;  // cluster params UBO
            clusterBindings[0].descriptorType = vk::DescriptorType::eUniformBuffer;
            clusterBindings[0].descriptorCount = 1;
            clusterBindings[0].stageFlags = vk::ShaderStageFlagBits::eCompute;
            clusterBindings[1].binding = 1;  // punctual light storage buffer (read)
            clusterBindings[1].descriptorType = vk::DescriptorType::eStorageBuffer;
            clusterBindings[1].descriptorCount = 1;
            clusterBindings[1].stageFlags = vk::ShaderStageFlagBits::eCompute;
            clusterBindings[2].binding = 2;  // per-cluster light lists (write)
            clusterBindings[2].descriptorType = vk::DescriptorType::eStorageBuffer;
            clusterBindings[2].descriptorCount = 1;
            clusterBindings[2].stageFlags = vk::ShaderStageFlagBits::eCompute;
            vk::DescriptorSetLayoutCreateInfo clusterLayoutInfo{};
            clusterLayoutInfo.setBindings(clusterBindings);
            auto clusterLayout = checked(renderer.device.createDescriptorSetLayout(clusterLayoutInfo), "clusterSetLayout");
            if (!clusterLayout)
            {
                return std::unexpected(clusterLayout.error());
            }
            renderer.clusterSetLayout = *clusterLayout;

            vk::DescriptorSetLayoutBinding instanceBinding{};
            instanceBinding.binding = 0;
            instanceBinding.descriptorType = vk::DescriptorType::eStorageBuffer;
            instanceBinding.descriptorCount = 1;
            instanceBinding.stageFlags = vk::ShaderStageFlagBits::eVertex;
            vk::DescriptorSetLayoutCreateInfo instanceLayoutInfo{};
            instanceLayoutInfo.setBindings(instanceBinding);
            auto instanceLayout = checked(renderer.device.createDescriptorSetLayout(instanceLayoutInfo), "instanceSetLayout");
            if (!instanceLayout)
            {
                return std::unexpected(instanceLayout.error());
            }
            renderer.instanceSetLayout = *instanceLayout;

            vk::DescriptorSetLayoutBinding tonemapBinding{};
            tonemapBinding.binding = 0;  // the offscreen color as a storage image
            tonemapBinding.descriptorType = vk::DescriptorType::eStorageImage;
            tonemapBinding.descriptorCount = 1;
            tonemapBinding.stageFlags = vk::ShaderStageFlagBits::eCompute;
            vk::DescriptorSetLayoutCreateInfo tonemapLayoutInfo{};
            tonemapLayoutInfo.setBindings(tonemapBinding);
            auto tonemapLayout = checked(renderer.device.createDescriptorSetLayout(tonemapLayoutInfo), "tonemapSetLayout");
            if (!tonemapLayout)
            {
                return std::unexpected(tonemapLayout.error());
            }
            renderer.tonemapSetLayout = *tonemapLayout;

            std::array<vk::DescriptorPoolSize, 4> poolSizes{
                vk::DescriptorPoolSize{ vk::DescriptorType::eCombinedImageSampler, 1024 },
                vk::DescriptorPoolSize{ vk::DescriptorType::eUniformBuffer, 4 * MaxFramesInFlight + 8 },
                vk::DescriptorPoolSize{ vk::DescriptorType::eStorageBuffer, 8 * MaxFramesInFlight + 8 },
                vk::DescriptorPoolSize{ vk::DescriptorType::eStorageImage, 4 } };
            vk::DescriptorPoolCreateInfo poolInfo{};
            poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;  // texture sets freed on Ref drop
            poolInfo.maxSets = 1024 + 8 * MaxFramesInFlight + 16;
            poolInfo.setPoolSizes(poolSizes);
            auto pool = checked(renderer.device.createDescriptorPool(poolInfo), "descriptorPool");
            if (!pool)
            {
                return std::unexpected(pool.error());
            }
            renderer.descriptorPool = *pool;

            // The bindless set comes from its own update-after-bind pool.
            vk::DescriptorPoolSize bindlessPoolSize{ vk::DescriptorType::eCombinedImageSampler, MaxBindlessTextures };
            vk::DescriptorPoolCreateInfo bindlessPoolInfo{};
            bindlessPoolInfo.flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind;
            bindlessPoolInfo.maxSets = 1;
            bindlessPoolInfo.setPoolSizes(bindlessPoolSize);
            auto bindlessPoolResult = checked(renderer.device.createDescriptorPool(bindlessPoolInfo), "bindlessPool");
            if (!bindlessPoolResult)
            {
                return std::unexpected(bindlessPoolResult.error());
            }
            renderer.bindlessPool = *bindlessPoolResult;

            vk::DescriptorSetAllocateInfo bindlessAlloc{};
            bindlessAlloc.descriptorPool = renderer.bindlessPool;
            bindlessAlloc.setSetLayouts(renderer.bindlessSetLayout);
            auto bindlessAllocated = checked(renderer.device.allocateDescriptorSets(bindlessAlloc), "allocate bindlessSet");
            if (!bindlessAllocated)
            {
                return std::unexpected(bindlessAllocated.error());
            }
            renderer.bindlessSet = (*bindlessAllocated)[0];

            for (u32 i = 0; i < MaxFramesInFlight; i = i + 1)
            {
                VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bufferInfo.size = sizeof(LightUbo);
                bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                VmaAllocationCreateInfo allocInfo{};
                allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
                allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo mapped{};
                if (vmaCreateBuffer(renderer.allocator, &bufferInfo, &allocInfo, &renderer.lightBuffers[i], &renderer.lightAllocs[i], &mapped) != VK_SUCCESS)
                {
                    return std::unexpected(std::string{ "light UBO vmaCreateBuffer failed" });
                }
                renderer.lightMapped[i] = mapped.pMappedData;

                vk::DescriptorSetAllocateInfo setAlloc{};
                setAlloc.descriptorPool = renderer.descriptorPool;
                setAlloc.setSetLayouts(renderer.lightSetLayout);
                auto allocated = checked(renderer.device.allocateDescriptorSets(setAlloc), "allocate lightSet");
                if (!allocated)
                {
                    return std::unexpected(allocated.error());
                }
                renderer.lightSets[i] = (*allocated)[0];

                vk::DescriptorBufferInfo lightBufferInfo{ vk::Buffer{ renderer.lightBuffers[i] }, 0, sizeof(LightUbo) };
                vk::WriteDescriptorSet lightWrite{};
                lightWrite.dstSet = renderer.lightSets[i];
                lightWrite.dstBinding = 0;
                lightWrite.descriptorType = vk::DescriptorType::eUniformBuffer;
                lightWrite.setBufferInfo(lightBufferInfo);
                renderer.device.updateDescriptorSets(lightWrite, {});

                // The punctual-light buffer starts at LightListInitial capacity and is
                // grown by ensureLightCapacity; bind it now so the set is complete.
                std::expected<Ref<Buffer>, std::string> lightList =
                    makeMappedStorageBuffer(renderer, static_cast<vk::DeviceSize>(LightListInitial) * sizeof(GpuLight));
                if (!lightList)
                {
                    return std::unexpected(lightList.error());
                }
                renderer.lightListBuffers[i] = *lightList;
                renderer.lightListCapacity[i] = LightListInitial;
                vk::DescriptorBufferInfo lightListInfo{ (*lightList)->buffer, 0, (*lightList)->size };
                vk::WriteDescriptorSet lightListWrite{};
                lightListWrite.dstSet = renderer.lightSets[i];
                lightListWrite.dstBinding = 1;
                lightListWrite.descriptorType = vk::DescriptorType::eStorageBuffer;
                lightListWrite.setBufferInfo(lightListInfo);
                renderer.device.updateDescriptorSets(lightListWrite, {});

                // The instance buffer is created lazily (ensureInstanceCapacity), which
                // also writes this set; allocate the set up front so it is stable.
                vk::DescriptorSetAllocateInfo instanceAlloc{};
                instanceAlloc.descriptorPool = renderer.descriptorPool;
                instanceAlloc.setSetLayouts(renderer.instanceSetLayout);
                auto instanceAllocated = checked(renderer.device.allocateDescriptorSets(instanceAlloc), "allocate instanceSet");
                if (!instanceAllocated)
                {
                    return std::unexpected(instanceAllocated.error());
                }
                renderer.instanceSets[i] = (*instanceAllocated)[0];

                // Cluster light-list buffer (device-local, compute-written) + the
                // cluster params UBO (host-mapped, written each frame).
                std::expected<Ref<Buffer>, std::string> clusterBuffer =
                    makeDeviceStorageBuffer(renderer, ClusterCount * ClusterStride);
                if (!clusterBuffer)
                {
                    return std::unexpected(clusterBuffer.error());
                }
                renderer.clusterBuffers[i] = *clusterBuffer;

                VkBufferCreateInfo paramInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                paramInfo.size = sizeof(ClusterParams);
                paramInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                VmaAllocationCreateInfo paramAlloc{};
                paramAlloc.usage = VMA_MEMORY_USAGE_AUTO;
                paramAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo paramMapped{};
                if (vmaCreateBuffer(renderer.allocator, &paramInfo, &paramAlloc, &renderer.clusterParamBuffers[i],
                                    &renderer.clusterParamAllocs[i], &paramMapped) != VK_SUCCESS)
                {
                    return std::unexpected(std::string{ "cluster params vmaCreateBuffer failed" });
                }
                renderer.clusterParamMapped[i] = paramMapped.pMappedData;

                // Lighting set bindings 2 (cluster lists) + 3 (cluster params).
                vk::DescriptorBufferInfo clusterInfo{ (*clusterBuffer)->buffer, 0, (*clusterBuffer)->size };
                vk::DescriptorBufferInfo paramBufferInfo{ vk::Buffer{ renderer.clusterParamBuffers[i] }, 0, sizeof(ClusterParams) };
                std::array<vk::WriteDescriptorSet, 2> lightClusterWrites{};
                lightClusterWrites[0].dstSet = renderer.lightSets[i];
                lightClusterWrites[0].dstBinding = 2;
                lightClusterWrites[0].descriptorType = vk::DescriptorType::eStorageBuffer;
                lightClusterWrites[0].setBufferInfo(clusterInfo);
                lightClusterWrites[1].dstSet = renderer.lightSets[i];
                lightClusterWrites[1].dstBinding = 3;
                lightClusterWrites[1].descriptorType = vk::DescriptorType::eUniformBuffer;
                lightClusterWrites[1].setBufferInfo(paramBufferInfo);
                renderer.device.updateDescriptorSets(lightClusterWrites, {});

                // Compute cluster set: params UBO + light list (read) + cluster lists (write).
                vk::DescriptorSetAllocateInfo clusterAlloc{};
                clusterAlloc.descriptorPool = renderer.descriptorPool;
                clusterAlloc.setSetLayouts(renderer.clusterSetLayout);
                auto clusterAllocated = checked(renderer.device.allocateDescriptorSets(clusterAlloc), "allocate clusterSet");
                if (!clusterAllocated)
                {
                    return std::unexpected(clusterAllocated.error());
                }
                renderer.clusterSets[i] = (*clusterAllocated)[0];
                std::array<vk::WriteDescriptorSet, 3> clusterWrites{};
                clusterWrites[0].dstSet = renderer.clusterSets[i];
                clusterWrites[0].dstBinding = 0;
                clusterWrites[0].descriptorType = vk::DescriptorType::eUniformBuffer;
                clusterWrites[0].setBufferInfo(paramBufferInfo);
                clusterWrites[1].dstSet = renderer.clusterSets[i];
                clusterWrites[1].dstBinding = 1;
                clusterWrites[1].descriptorType = vk::DescriptorType::eStorageBuffer;
                clusterWrites[1].setBufferInfo(lightListInfo);
                clusterWrites[2].dstSet = renderer.clusterSets[i];
                clusterWrites[2].dstBinding = 2;
                clusterWrites[2].descriptorType = vk::DescriptorType::eStorageBuffer;
                clusterWrites[2].setBufferInfo(clusterInfo);
                renderer.device.updateDescriptorSets(clusterWrites, {});
            }

            // The cull compute pipeline reads/writes the cluster set layout.
            std::expected<Ref<Pipeline>, std::string> cull =
                newComputePipeline(renderer, "shaders/light_cull.spv", renderer.clusterSetLayout);
            if (!cull)
            {
                return std::unexpected(cull.error());
            }
            renderer.cullPipeline = *cull;

            // Post-process tonemap: a compute pipeline + a set binding the offscreen
            // color as a storage image (read+written in place). A layer adds the pass.
            std::expected<Ref<Pipeline>, std::string> tonemap =
                newComputePipeline(renderer, "shaders/tonemap.spv", renderer.tonemapSetLayout);
            if (!tonemap)
            {
                return std::unexpected(tonemap.error());
            }
            renderer.tonemapPipeline = *tonemap;

            vk::DescriptorSetAllocateInfo tonemapAlloc{};
            tonemapAlloc.descriptorPool = renderer.descriptorPool;
            tonemapAlloc.setSetLayouts(renderer.tonemapSetLayout);
            auto tonemapAllocated = checked(renderer.device.allocateDescriptorSets(tonemapAlloc), "allocate tonemapSet");
            if (!tonemapAllocated)
            {
                return std::unexpected(tonemapAllocated.error());
            }
            renderer.tonemapSet = (*tonemapAllocated)[0];
            updateTonemapSet(renderer);

            // Depth pre-pass: a vertex-only pipeline that reuses the mesh vertex shader.
            std::expected<Ref<Pipeline>, std::string> depthPrepass =
                makeDepthPrepassPipeline(renderer, "shaders/mesh.spv");
            if (!depthPrepass)
            {
                return std::unexpected(depthPrepass.error());
            }
            renderer.depthPrepassPipeline = *depthPrepass;
            return {};
        }
    }

    std::expected<Renderer, std::string> newRenderer(Window& window)
    {
        Renderer renderer;
        renderer.window = &window;

        u32 sdlExtensionCount = 0;
        const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);

        vkb::InstanceBuilder instanceBuilder;
        instanceBuilder
            .set_app_name("Saffron Editor")
            .set_engine_name("Saffron Engine")
            .require_api_version(1, 3, 0)
            .request_validation_layers(true)
            .use_default_debug_messenger();
        for (u32 i = 0; i < sdlExtensionCount; i = i + 1)
        {
            instanceBuilder.enable_extension(sdlExtensions[i]);
        }
        auto instanceResult = instanceBuilder.build();
        if (!instanceResult)
        {
            return std::unexpected(std::format("instance creation failed: {}", instanceResult.error().message()));
        }
        renderer.vkbInstance = instanceResult.value();
        renderer.instance = vk::Instance{ renderer.vkbInstance.instance };

        VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(window.handle, renderer.vkbInstance.instance, nullptr, &rawSurface))
        {
            return std::unexpected(std::format("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError()));
        }
        renderer.surface = vk::SurfaceKHR{ rawSurface };

        VkPhysicalDeviceVulkan11Features features11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        features11.shaderDrawParameters = VK_TRUE;  // Slang SV_VertexID emits the DrawParameters capability

        VkPhysicalDeviceVulkan13Features features13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;

        // Bindless: one global texture array indexed per-instance. Core Vulkan 1.2
        // descriptor indexing — required (selection fails with a clear error if absent).
        VkPhysicalDeviceVulkan12Features features12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        features12.runtimeDescriptorArray = VK_TRUE;
        features12.descriptorBindingPartiallyBound = VK_TRUE;
        features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

        vkb::PhysicalDeviceSelector selector{ renderer.vkbInstance };
        auto physicalResult = selector
                                  .set_minimum_version(1, 3)
                                  .set_required_features_11(features11)
                                  .set_required_features_12(features12)
                                  .set_required_features_13(features13)
                                  .set_surface(rawSurface)
                                  .select();
        if (!physicalResult)
        {
            return std::unexpected(std::format("no suitable GPU: {}", physicalResult.error().message()));
        }

        vkb::DeviceBuilder deviceBuilder{ physicalResult.value() };
        auto deviceResult = deviceBuilder.build();
        if (!deviceResult)
        {
            return std::unexpected(std::format("device creation failed: {}", deviceResult.error().message()));
        }
        renderer.vkbDevice = deviceResult.value();
        renderer.physicalDevice = vk::PhysicalDevice{ physicalResult.value().physical_device };
        renderer.device = vk::Device{ renderer.vkbDevice.device };

        auto queueResult = renderer.vkbDevice.get_queue(vkb::QueueType::graphics);
        if (!queueResult)
        {
            return std::unexpected(std::format("no graphics queue: {}", queueResult.error().message()));
        }
        renderer.graphicsQueue = vk::Queue{ queueResult.value() };
        renderer.graphicsQueueFamily = renderer.vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

        // Highest MSAA level the device supports for both color + depth framebuffers (capped at 8x).
        vk::SampleCountFlags sampleCounts = renderer.physicalDevice.getProperties().limits.framebufferColorSampleCounts &
                                            renderer.physicalDevice.getProperties().limits.framebufferDepthSampleCounts;
        if (sampleCounts & vk::SampleCountFlagBits::e8) { renderer.maxSampleCount = vk::SampleCountFlagBits::e8; }
        else if (sampleCounts & vk::SampleCountFlagBits::e4) { renderer.maxSampleCount = vk::SampleCountFlagBits::e4; }
        else if (sampleCounts & vk::SampleCountFlagBits::e2) { renderer.maxSampleCount = vk::SampleCountFlagBits::e2; }

        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.instance = renderer.vkbInstance.instance;
        allocatorInfo.physicalDevice = physicalResult.value().physical_device;
        allocatorInfo.device = renderer.vkbDevice.device;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        if (vmaCreateAllocator(&allocatorInfo, &renderer.allocator) != VK_SUCCESS)
        {
            return std::unexpected(std::string{ "vmaCreateAllocator failed" });
        }

        auto swapchainBuilt = buildSwapchain(renderer, window.width, window.height);
        if (!swapchainBuilt)
        {
            return std::unexpected(swapchainBuilt.error());
        }

        // Offscreen scene target shown in the editor Viewport panel. Same format
        // as the swapchain so the scene pipelines need no special format.
        auto offscreen = newColorImage(renderer, window.width, window.height, OffscreenColorFormat, true);
        if (!offscreen)
        {
            return std::unexpected(offscreen.error());
        }
        renderer.offscreenViewport = std::move(*offscreen);
        renderer.viewportDesiredWidth = window.width;
        renderer.viewportDesiredHeight = window.height;
        renderer.viewportGeneration = 1;

        auto depth = newDepthImage(renderer, window.width, window.height);
        if (!depth)
        {
            return std::unexpected(depth.error());
        }
        renderer.offscreenDepth = std::move(*depth);

        for (FrameData& frame : renderer.frames)
        {
            vk::CommandPoolCreateInfo poolInfo{};
            poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
            poolInfo.queueFamilyIndex = renderer.graphicsQueueFamily;
            auto pool = checked(renderer.device.createCommandPool(poolInfo), "createCommandPool");
            if (!pool)
            {
                return std::unexpected(pool.error());
            }
            frame.commandPool = *pool;

            vk::CommandBufferAllocateInfo allocInfo{};
            allocInfo.commandPool = frame.commandPool;
            allocInfo.level = vk::CommandBufferLevel::ePrimary;
            allocInfo.commandBufferCount = 1;
            auto buffers = checked(renderer.device.allocateCommandBuffers(allocInfo), "allocateCommandBuffers");
            if (!buffers)
            {
                return std::unexpected(buffers.error());
            }
            frame.commandBuffer = (*buffers)[0];

            auto imageAvailable = checked(renderer.device.createSemaphore(vk::SemaphoreCreateInfo{}), "createSemaphore");
            if (!imageAvailable)
            {
                return std::unexpected(imageAvailable.error());
            }
            frame.imageAvailable = *imageAvailable;

            vk::FenceCreateInfo fenceInfo{};
            fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled;
            auto fence = checked(renderer.device.createFence(fenceInfo), "createFence");
            if (!fence)
            {
                return std::unexpected(fence.error());
            }
            frame.inFlight = *fence;
        }

        if (std::expected<void, std::string> descriptors = initDescriptorResources(renderer); !descriptors)
        {
            return std::unexpected(descriptors.error());
        }
        setDirectionalLight(renderer, glm::vec3(-0.5f, -1.0f, -0.3f), glm::vec3(1.0f), 1.0f, 0.15f);

        const std::array<u8, 4> white{ 255, 255, 255, 255 };
        std::expected<Ref<GpuTexture>, std::string> whiteTexture = uploadTexture(renderer, white.data(), 1, 1, false);
        if (!whiteTexture)
        {
            return std::unexpected(whiteTexture.error());
        }
        renderer.defaultWhiteTexture = *whiteTexture;

        // Seed every bindless slot with the default texture so no slot is ever unbound —
        // some drivers (llvmpipe) fault sampling a partially-bound array even on slots a
        // shader never reads. Real uploads overwrite their slot afterwards.
        {
            std::vector<vk::DescriptorImageInfo> infos(MaxBindlessTextures,
                vk::DescriptorImageInfo{ renderer.linearSampler, (*whiteTexture)->view,
                                         vk::ImageLayout::eShaderReadOnlyOptimal });
            vk::WriteDescriptorSet fill{};
            fill.dstSet = renderer.bindlessSet;
            fill.dstBinding = 0;
            fill.dstArrayElement = 0;
            fill.descriptorType = vk::DescriptorType::eCombinedImageSampler;
            fill.setImageInfo(infos);
            renderer.device.updateDescriptorSets(fill, {});
        }

        logInfo(std::format("vulkan ready — gpu '{}', {} swapchain images",
                            renderer.vkbDevice.physical_device.name,
                            renderer.swapchainImages.size()));
        return renderer;
    }

    void destroyRenderer(Renderer& renderer)
    {
        if (renderer.device)
        {
            static_cast<void>(renderer.device.waitIdle());
        }

        // Drop any Refs the renderer itself still holds, plus the closure vectors
        // (which may capture Refs), before the descriptor pool / allocator / device
        // are torn down — a GpuTexture frees its material set from the pool.
        renderer.sceneDrawList = SceneDrawList{};  // drops mesh/texture/pipeline Refs
        renderer.pipelineCache.clear();            // drops the cached mesh PSOs
        renderer.sceneSubmissions.clear();
        renderer.uiSubmissions.clear();
        renderer.defaultWhiteTexture.reset();
        renderer.cullPipeline.reset();        // RAII frees the compute pipeline + layout
        renderer.thumbnailPipeline.reset();
        renderer.tonemapPipeline.reset();
        renderer.depthPrepassPipeline.reset();

        renderer.offscreenViewport.reset();  // free before the allocator/device
        renderer.offscreenDepth.reset();
        renderer.msaaColor.reset();
        renderer.msaaDepth.reset();

        for (u32 i = 0; i < MaxFramesInFlight; i = i + 1)
        {
            renderer.instanceBuffers[i].reset();  // RAII frees the SSBO before the allocator
            renderer.lightListBuffers[i].reset();
            renderer.clusterBuffers[i].reset();
            if (renderer.lightBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(renderer.allocator, renderer.lightBuffers[i], renderer.lightAllocs[i]);
                renderer.lightBuffers[i] = VK_NULL_HANDLE;
            }
            if (renderer.clusterParamBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(renderer.allocator, renderer.clusterParamBuffers[i], renderer.clusterParamAllocs[i]);
                renderer.clusterParamBuffers[i] = VK_NULL_HANDLE;
            }
        }
        if (renderer.descriptorPool)
        {
            renderer.device.destroyDescriptorPool(renderer.descriptorPool);
        }
        if (renderer.bindlessPool)
        {
            renderer.device.destroyDescriptorPool(renderer.bindlessPool);
        }
        if (renderer.bindlessSetLayout)
        {
            renderer.device.destroyDescriptorSetLayout(renderer.bindlessSetLayout);
        }
        if (renderer.lightSetLayout)
        {
            renderer.device.destroyDescriptorSetLayout(renderer.lightSetLayout);
        }
        if (renderer.instanceSetLayout)
        {
            renderer.device.destroyDescriptorSetLayout(renderer.instanceSetLayout);
        }
        if (renderer.clusterSetLayout)
        {
            renderer.device.destroyDescriptorSetLayout(renderer.clusterSetLayout);
        }
        if (renderer.tonemapSetLayout)
        {
            renderer.device.destroyDescriptorSetLayout(renderer.tonemapSetLayout);
        }
        if (renderer.linearSampler)
        {
            renderer.device.destroySampler(renderer.linearSampler);
        }

        for (FrameData& frame : renderer.frames)
        {
            renderer.device.destroyFence(frame.inFlight);
            renderer.device.destroySemaphore(frame.imageAvailable);
            renderer.device.destroyCommandPool(frame.commandPool);
        }

        destroySwapchainResources(renderer);

        if (renderer.allocator != nullptr)
        {
            vmaDestroyAllocator(renderer.allocator);
            renderer.allocator = nullptr;
        }
        if (renderer.surface)
        {
            vkb::destroy_surface(renderer.vkbInstance, static_cast<VkSurfaceKHR>(renderer.surface));
        }
        vkb::destroy_device(renderer.vkbDevice);
        vkb::destroy_instance(renderer.vkbInstance);
    }

    bool beginFrame(Renderer& renderer)
    {
        FrameData& frame = renderer.frames[renderer.frameIndex];

        static_cast<void>(renderer.device.waitForFences(frame.inFlight, VK_TRUE, UINT64_MAX));

        vk::ResultValue<u32> acquire = renderer.device.acquireNextImageKHR(
            renderer.swapchain, UINT64_MAX, frame.imageAvailable, nullptr);
        if (acquire.result == vk::Result::eErrorOutOfDateKHR)
        {
            recreateSwapchain(renderer);
            return false;
        }
        if (acquire.result != vk::Result::eSuccess && acquire.result != vk::Result::eSuboptimalKHR)
        {
            logError(std::format("vkAcquireNextImageKHR failed: {}", vk::to_string(acquire.result)));
            return false;
        }
        renderer.imageIndex = acquire.value;

        // Ensure the previous frame that used THIS image has finished before we
        // reuse the image's renderFinished semaphore.
        if (renderer.imagesInFlight[renderer.imageIndex])
        {
            static_cast<void>(renderer.device.waitForFences(renderer.imagesInFlight[renderer.imageIndex], VK_TRUE, UINT64_MAX));
        }
        renderer.imagesInFlight[renderer.imageIndex] = frame.inFlight;

        // Apply a pending Viewport resize (requested last frame). Single shared
        // target, so a full device idle is required before recreating it.
        if (renderer.viewportDesiredWidth > 0 && renderer.viewportDesiredHeight > 0 &&
            (renderer.viewportDesiredWidth != renderer.offscreenViewport.extent.width ||
             renderer.viewportDesiredHeight != renderer.offscreenViewport.extent.height))
        {
            static_cast<void>(renderer.device.waitIdle());
            auto resized = newColorImage(renderer, renderer.viewportDesiredWidth,
                                         renderer.viewportDesiredHeight, OffscreenColorFormat, true);
            if (resized)
            {
                renderer.offscreenViewport = std::move(*resized);
                renderer.viewportGeneration = renderer.viewportGeneration + 1;
                updateTonemapSet(renderer);  // the storage-image binding follows the new view
                auto resizedDepth = newDepthImage(renderer, renderer.viewportDesiredWidth, renderer.viewportDesiredHeight);
                if (resizedDepth)
                {
                    renderer.offscreenDepth = std::move(*resizedDepth);
                }
                else
                {
                    logError(resizedDepth.error());
                }
                recreateMsaaTargets(renderer);  // MSAA targets follow the offscreen extent
            }
            else
            {
                logError(resized.error());
            }
        }

        static_cast<void>(renderer.device.resetFences(frame.inFlight));
        static_cast<void>(frame.commandBuffer.reset());
        renderer.sceneDrawList = SceneDrawList{};  // last frame's geometry has presented
        renderer.sceneSubmissions.clear();
        renderer.uiSubmissions.clear();

        vk::CommandBufferBeginInfo beginInfo{};
        beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(frame.commandBuffer.begin(beginInfo));

        // Rendering scopes are opened in endFrame: pass 1 (scene → offscreen),
        // pass 2 (ui → swapchain).
        return true;
    }

    void submit(Renderer& renderer, RenderFn fn)
    {
        renderer.sceneSubmissions.push_back(std::move(fn));
    }

    void submitUi(Renderer& renderer, RenderFn fn)
    {
        renderer.uiSubmissions.push_back(std::move(fn));
    }

    void setViewportDesiredSize(Renderer& renderer, u32 width, u32 height)
    {
        renderer.viewportDesiredWidth = width;
        renderer.viewportDesiredHeight = height;
    }

    vk::ImageView viewportImageView(const Renderer& renderer)
    {
        return renderer.offscreenViewport.view;
    }

    u32 viewportGeneration(const Renderer& renderer)
    {
        return renderer.viewportGeneration;
    }

    u32 viewportWidth(const Renderer& renderer)
    {
        return renderer.offscreenViewport.extent.width;
    }

    u32 viewportHeight(const Renderer& renderer)
    {
        return renderer.offscreenViewport.extent.height;
    }

    void beginFrameGraph(Renderer& renderer)
    {
        Image& offscreen = renderer.offscreenViewport;
        Image& depth = renderer.offscreenDepth;
        const u32 f = renderer.frameIndex;
        const bool doCull = renderer.clusterDispatchPending && renderer.cullPipeline;
        renderer.clusterDispatchPending = false;

        // The frame as a render graph: declare each pass's resource usage and let the
        // graph derive the barriers + layout transitions. The offscreen color carries
        // its layout across frames (sampled by ImGui last frame → WAR into this scene).
        renderer.renderGraph = newRenderGraph();
        RenderGraph& graph = renderer.renderGraph;
        // frameSceneColor is always the offscreen (what ImGui samples + tonemap reads). With
        // MSAA the scene renders to msaaColor and resolves into it; otherwise it is the direct
        // scene target. Likewise the scene depth is the multisampled buffer when MSAA is on.
        const bool msaa = renderer.sampleCount != vk::SampleCountFlagBits::e1 && renderer.msaaColor.image;
        renderer.frameSceneColor = importImage(graph, offscreen.image, offscreen.view,
            vk::ImageAspectFlagBits::eColor, offscreen.layout, &offscreen.layout);
        RgResource sceneColorAttachment = renderer.frameSceneColor;
        if (msaa)
        {
            sceneColorAttachment = importImage(graph, renderer.msaaColor.image, renderer.msaaColor.view,
                vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined, nullptr);
        }
        Image* depthTarget = &depth;
        if (msaa)
        {
            depthTarget = &renderer.msaaDepth;
        }
        RgResource sceneDepth = importImage(graph, depthTarget->image, depthTarget->view,
            vk::ImageAspectFlagBits::eDepth, vk::ImageLayout::eUndefined, nullptr);
        renderer.frameSwapImage = importImage(graph, renderer.swapchainImages[renderer.imageIndex],
            renderer.swapchainImageViews[renderer.imageIndex], vk::ImageAspectFlagBits::eColor,
            vk::ImageLayout::eUndefined, nullptr);

        // Clustered forward: a compute pass culls the punctual lights into the froxel
        // grid; the scene fragment reads the result (the graph emits the compute→
        // fragment barrier from these declared usages).
        RgResource clusterBuffer{};
        if (doCull)
        {
            clusterBuffer = importBuffer(graph, renderer.clusterBuffers[f]->buffer);

            RgPass cull;
            cull.name = "light-cull";
            cull.kind = RgPassKind::Compute;
            cull.accesses = { RgAccess{ clusterBuffer, RgUsage::StorageWriteCompute } };
            cull.execute = [&renderer, f](vk::CommandBuffer cmd)
            {
                cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.cullPipeline->pipeline);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                    renderer.cullPipeline->layout, 0, renderer.clusterSets[f], {});
                const u32 groups = (ClusterCount + 63) / 64;
                cmd.dispatch(groups, 1, 1);
            };
            addPass(graph, std::move(cull));
        }

        // Optional depth pre-pass: lay down scene depth first, so the scene pass loads it
        // and shades only the front-most fragments. The graph derives the depth WAW
        // barrier (pre-pass write → scene write) from the two declared depth usages.
        const bool doDepthPrepass = renderer.useDepthPrepass && renderer.depthPrepassPipeline;
        if (doDepthPrepass)
        {
            RgPass depthPass;
            depthPass.name = "depth-prepass";
            depthPass.kind = RgPassKind::Graphics;
            depthPass.depth = RgAttachment{ sceneDepth, vk::AttachmentLoadOp::eClear,
                vk::AttachmentStoreOp::eStore, vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } } };
            depthPass.renderArea = offscreen.extent;
            depthPass.execute = [&renderer](vk::CommandBuffer cmd)
            {
                recordDepthPrepass(renderer, cmd);
            };
            addPass(graph, std::move(depthPass));
        }

        RgPass scene;
        scene.name = "scene";
        scene.kind = RgPassKind::Graphics;
        if (doCull)
        {
            scene.accesses = { RgAccess{ clusterBuffer, RgUsage::StorageReadFragment } };
        }
        // MSAA: render to the multisampled color, resolve into the offscreen (don't store
        // the multisampled samples). Otherwise render straight into the offscreen.
        scene.color = RgAttachment{ sceneColorAttachment, vk::AttachmentLoadOp::eClear,
            vk::AttachmentStoreOp::eStore, vk::ClearValue{ vk::ClearColorValue{ renderer.clearColor } } };
        if (msaa)
        {
            scene.color->storeOp = vk::AttachmentStoreOp::eDontCare;
            scene.color->resolve = renderer.frameSceneColor;
        }
        // Load the pre-pass depth when present; otherwise clear it here as before.
        vk::AttachmentLoadOp depthLoad = vk::AttachmentLoadOp::eClear;
        if (doDepthPrepass)
        {
            depthLoad = vk::AttachmentLoadOp::eLoad;
        }
        scene.depth = RgAttachment{ sceneDepth, depthLoad, vk::AttachmentStoreOp::eDontCare,
            vk::ClearValue{ vk::ClearDepthStencilValue{ 1.0f, 0 } } };
        scene.renderArea = offscreen.extent;
        scene.execute = [&renderer](vk::CommandBuffer cmd)
        {
            recordSceneDrawList(renderer, cmd);
            for (RenderFn& fn : renderer.sceneSubmissions)
            {
                fn(cmd);
            }
        };
        addPass(graph, std::move(scene));
    }

    RenderGraph& frameGraph(Renderer& renderer)
    {
        return renderer.renderGraph;
    }

    RgResource viewportColorResource(const Renderer& renderer)
    {
        return renderer.frameSceneColor;
    }

    void addTonemapPass(Renderer& renderer, RenderGraph& graph)
    {
        RgPass pass;
        pass.name = "tonemap";
        pass.kind = RgPassKind::Compute;
        pass.accesses = { RgAccess{ renderer.frameSceneColor, RgUsage::StorageImageRWCompute } };
        pass.execute = [&renderer](vk::CommandBuffer cmd)
        {
            cmd.bindPipeline(vk::PipelineBindPoint::eCompute, renderer.tonemapPipeline->pipeline);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                renderer.tonemapPipeline->layout, 0, renderer.tonemapSet, {});
            const vk::Extent2D extent = renderer.offscreenViewport.extent;
            cmd.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        };
        addPass(graph, std::move(pass));
    }

    void endFrame(Renderer& renderer)
    {
        FrameData& frame = renderer.frames[renderer.frameIndex];
        RenderGraph& graph = renderer.renderGraph;

        // The ui pass samples the (now post-processed) offscreen color and composites
        // ImGui into the swapchain. Added last so app-authored passes land before it.
        RgPass ui;
        ui.name = "ui";
        ui.kind = RgPassKind::Graphics;
        ui.accesses = { RgAccess{ renderer.frameSceneColor, RgUsage::SampledRead } };
        ui.color = RgAttachment{ renderer.frameSwapImage, vk::AttachmentLoadOp::eClear,
            vk::AttachmentStoreOp::eStore, vk::ClearValue{ vk::ClearColorValue{ renderer.clearColor } } };
        ui.renderArea = renderer.swapchainExtent;
        ui.execute = [&renderer](vk::CommandBuffer cmd)
        {
            for (RenderFn& fn : renderer.uiSubmissions)
            {
                fn(cmd);
            }
        };
        addPass(graph, std::move(ui));

        executeRenderGraph(graph, frame.commandBuffer);

        // The swapchain image is only safely owned in-frame, so a pending capture
        // is copied here, between the ImGui pass and present; its COLOR->PRESENT
        // transition is folded into captureImageToBuffer's toLayout.
        VkBuffer captureBuffer = VK_NULL_HANDLE;
        VmaAllocation captureAlloc = nullptr;
        VmaAllocationInfo captureInfo{};
        vk::Extent2D captureExtent{};
        bool doCapture = renderer.captureNextSwapchainPath.has_value();
        if (doCapture)
        {
            captureExtent = renderer.swapchainExtent;
            const vk::DeviceSize bytes =
                static_cast<vk::DeviceSize>(captureExtent.width) * captureExtent.height * 4;
            std::expected<void, std::string> created =
                newHostCaptureBuffer(renderer, bytes, captureBuffer, captureAlloc, captureInfo);
            if (!created)
            {
                logError(created.error());
                doCapture = false;
                renderer.captureNextSwapchainPath.reset();
            }
        }
        if (doCapture)
        {
            captureImageToBuffer(
                frame.commandBuffer, renderer.swapchainImages[renderer.imageIndex], captureExtent,
                vk::ImageLayout::eColorAttachmentOptimal,
                vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                vk::ImageLayout::ePresentSrcKHR,
                vk::PipelineStageFlagBits2::eBottomOfPipe, vk::AccessFlagBits2::eNone,
                vk::Buffer{ captureBuffer });
        }
        else
        {
            transitionImage(
                frame.commandBuffer, renderer.swapchainImages[renderer.imageIndex],
                vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR,
                vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                vk::PipelineStageFlagBits2::eBottomOfPipe, vk::AccessFlagBits2::eNone);
        }

        static_cast<void>(frame.commandBuffer.end());

        vk::Semaphore signalSemaphore = renderer.renderFinished[renderer.imageIndex];

        vk::SemaphoreSubmitInfo waitInfo{};
        waitInfo.semaphore = frame.imageAvailable;
        waitInfo.stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;

        vk::SemaphoreSubmitInfo signalInfo{};
        signalInfo.semaphore = signalSemaphore;
        signalInfo.stageMask = vk::PipelineStageFlagBits2::eAllCommands;

        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = frame.commandBuffer;

        vk::SubmitInfo2 submitInfo{};
        submitInfo.setWaitSemaphoreInfos(waitInfo);
        submitInfo.setCommandBufferInfos(cmdInfo);
        submitInfo.setSignalSemaphoreInfos(signalInfo);
        static_cast<void>(renderer.graphicsQueue.submit2(submitInfo, frame.inFlight));

        vk::PresentInfoKHR presentInfo{};
        presentInfo.setWaitSemaphores(signalSemaphore);
        presentInfo.setSwapchains(renderer.swapchain);
        presentInfo.setImageIndices(renderer.imageIndex);
        vk::Result present = renderer.graphicsQueue.presentKHR(presentInfo);
        if (present == vk::Result::eErrorOutOfDateKHR || present == vk::Result::eSuboptimalKHR)
        {
            recreateSwapchain(renderer);
        }

        // The recorded copy is now submitted; idle so it completed, then write the PNG.
        if (doCapture && captureBuffer != VK_NULL_HANDLE)
        {
            static_cast<void>(renderer.device.waitIdle());
            vmaInvalidateAllocation(renderer.allocator, captureAlloc, 0, VK_WHOLE_SIZE);
            std::expected<void, std::string> wrote = writeBufferToPng(
                static_cast<const unsigned char*>(captureInfo.pMappedData),
                captureExtent.width, captureExtent.height,
                renderer.swapchainFormat, *renderer.captureNextSwapchainPath);
            if (!wrote)
            {
                logError(wrote.error());
            }
            else
            {
                logInfo(std::format("captured window ({}x{}) to {}",
                                    captureExtent.width, captureExtent.height,
                                    *renderer.captureNextSwapchainPath));
            }
            vmaDestroyBuffer(renderer.allocator, captureBuffer, captureAlloc);
            renderer.captureNextSwapchainPath.reset();
        }

        renderer.frameIndex = (renderer.frameIndex + 1) % MaxFramesInFlight;
    }

    std::string assetPath(std::string_view relative)
    {
        const char* base = SDL_GetBasePath();  // SDL3: owned by SDL, do not free
        std::string result;
        if (base != nullptr)
        {
            result = base;
        }
        result.append(relative);
        return result;
    }

    std::expected<Ref<Pipeline>, std::string> newMeshPipeline(Renderer& renderer, std::string_view shaderName, bool unlit)
    {
        std::string path = assetPath(shaderName);
        auto moduleResult = loadShaderModule(renderer.device, path);
        if (!moduleResult)
        {
            return std::unexpected(moduleResult.error());
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
        stages[1].stage = vk::ShaderStageFlagBits::eFragment;
        stages[1].module = shaderModule;
        stages[1].pName = "fragmentMain";
        stages[1].pSpecializationInfo = &specInfo;

        vk::VertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(Vertex);
        binding.inputRate = vk::VertexInputRate::eVertex;

        std::array<vk::VertexInputAttributeDescription, 3> attributes{
            vk::VertexInputAttributeDescription{ 0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, position) },
            vk::VertexInputAttributeDescription{ 1, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, normal) },
            vk::VertexInputAttributeDescription{ 2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv0) } };

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
        raster.cullMode = vk::CullModeFlagBits::eNone;  // enable Back once winding is verified
        raster.frontFace = vk::FrontFace::eCounterClockwise;
        raster.lineWidth = 1.0f;

        vk::PipelineMultisampleStateCreateInfo multisample{};
        multisample.rasterizationSamples = renderer.sampleCount;  // match the MSAA target

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

        std::array<vk::DescriptorSetLayout, 3> setLayouts{
            renderer.bindlessSetLayout, renderer.lightSetLayout, renderer.instanceSetLayout };
        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setSetLayouts(setLayouts);
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult = checked(renderer.device.createPipelineLayout(layoutInfo), "createPipelineLayout (mesh)");
        if (!layoutResult)
        {
            renderer.device.destroyShaderModule(shaderModule);
            return std::unexpected(layoutResult.error());
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

        vk::ResultValue<vk::Pipeline> created = renderer.device.createGraphicsPipeline(nullptr, pipelineInfo);
        renderer.device.destroyShaderModule(shaderModule);
        if (created.result != vk::Result::eSuccess)
        {
            renderer.device.destroyPipelineLayout(*layoutResult);
            return std::unexpected(std::format("createGraphicsPipeline (mesh): {}", vk::to_string(created.result)));
        }

        Pipeline pipeline;
        pipeline.device = renderer.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;
        return std::make_shared<Pipeline>(std::move(pipeline));
    }

    Ref<Pipeline> requestMeshPipeline(Renderer& renderer, const Material& material)
    {
        std::string key = material.shader;
        if (material.unlit)
        {
            key = key + "|unlit";
        }
        auto found = renderer.pipelineCache.find(key);
        if (found != renderer.pipelineCache.end())
        {
            return found->second;
        }
        std::expected<Ref<Pipeline>, std::string> built = newMeshPipeline(renderer, material.shader, material.unlit);
        if (!built)
        {
            logError(built.error());
            return nullptr;
        }
        renderer.pipelineCache.emplace(key, *built);
        return *built;
    }

    u32 pipelineCount(const Renderer& renderer)
    {
        return static_cast<u32>(renderer.pipelineCache.size());
    }

    std::expected<Ref<GpuMesh>, std::string> uploadMesh(Renderer& renderer, const Mesh& mesh)
    {
        if (mesh.vertices.empty() || mesh.indices.empty())
        {
            return std::unexpected(std::string{ "uploadMesh: empty mesh" });
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
        if (vmaCreateBuffer(renderer.allocator, &stagingInfo, &stagingAlloc, &staging, &stagingAllocation, &stagingMapped) != VK_SUCCESS)
        {
            return std::unexpected(std::string{ "uploadMesh: staging vmaCreateBuffer failed" });
        }
        std::memcpy(stagingMapped.pMappedData, mesh.vertices.data(), vertexBytes);
        std::memcpy(static_cast<char*>(stagingMapped.pMappedData) + vertexBytes, mesh.indices.data(), indexBytes);
        vmaFlushAllocation(renderer.allocator, stagingAllocation, 0, VK_WHOLE_SIZE);

        auto makeDeviceBuffer = [&](vk::DeviceSize size, VkBufferUsageFlags usage, VkBuffer& outBuffer, VmaAllocation& outAlloc) -> bool
        {
            VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            info.size = size;
            info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VmaAllocationCreateInfo alloc{};
            alloc.usage = VMA_MEMORY_USAGE_AUTO;
            return vmaCreateBuffer(renderer.allocator, &info, &alloc, &outBuffer, &outAlloc, nullptr) == VK_SUCCESS;
        };

        GpuMesh gpu;
        gpu.allocator = renderer.allocator;
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
                vmaDestroyBuffer(renderer.allocator, vertexBuffer, vertexAlloc);
            }
            vmaDestroyBuffer(renderer.allocator, staging, stagingAllocation);
            return std::unexpected(std::string{ "uploadMesh: device vmaCreateBuffer failed" });
        }
        gpu.vertexBuffer = vk::Buffer{ vertexBuffer };
        gpu.vertexAlloc = vertexAlloc;
        gpu.indexBuffer = vk::Buffer{ indexBuffer };
        gpu.indexAlloc = indexAlloc;

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = renderer.frames[0].commandPool;
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(renderer.device.allocateCommandBuffers(cmdAlloc), "uploadMesh: allocateCommandBuffers");
        if (!cmds)
        {
            vmaDestroyBuffer(renderer.allocator, staging, stagingAllocation);
            return std::unexpected(cmds.error());
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
        static_cast<void>(renderer.graphicsQueue.submit2(submitInfo, nullptr));
        static_cast<void>(renderer.device.waitIdle());
        renderer.device.freeCommandBuffers(renderer.frames[0].commandPool, cmd);
        vmaDestroyBuffer(renderer.allocator, staging, stagingAllocation);

        logInfo(std::format("uploaded mesh: {} vertices, {} indices, {} submeshes",
                            mesh.vertices.size(), mesh.indices.size(), mesh.submeshes.size()));
        return std::make_shared<GpuMesh>(std::move(gpu));
    }

    // Ensures the current frame's instance buffer holds at least `count` elements,
    // growing to the next power of two (never shrinking) and rewriting its set.
    std::expected<void, std::string> ensureInstanceCapacity(Renderer& renderer, u32 frame, u32 count)
    {
        if (renderer.instanceBuffers[frame] && renderer.instanceCapacity[frame] >= count)
        {
            return {};
        }
        u32 capacity = renderer.instanceCapacity[frame];
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
        std::expected<Ref<Buffer>, std::string> buffer =
            makeMappedStorageBuffer(renderer, static_cast<vk::DeviceSize>(capacity) * sizeof(InstanceData));
        if (!buffer)
        {
            return std::unexpected(buffer.error());
        }
        renderer.instanceBuffers[frame] = *buffer;
        renderer.instanceCapacity[frame] = capacity;

        vk::DescriptorBufferInfo bufferInfo{ (*buffer)->buffer, 0, (*buffer)->size };
        vk::WriteDescriptorSet write{};
        write.dstSet = renderer.instanceSets[frame];
        write.dstBinding = 0;
        write.descriptorType = vk::DescriptorType::eStorageBuffer;
        write.setBufferInfo(bufferInfo);
        renderer.device.updateDescriptorSets(write, {});
        return {};
    }

    // Ensures the current frame's punctual-light buffer holds at least `count` lights,
    // growing to the next power of two (never shrinking) and rewriting its set.
    std::expected<void, std::string> ensureLightCapacity(Renderer& renderer, u32 frame, u32 count)
    {
        if (renderer.lightListBuffers[frame] && renderer.lightListCapacity[frame] >= count)
        {
            return {};
        }
        u32 capacity = renderer.lightListCapacity[frame];
        if (capacity == 0)
        {
            capacity = LightListInitial;
        }
        while (capacity < count)
        {
            capacity = capacity * 2;
        }
        std::expected<Ref<Buffer>, std::string> buffer =
            makeMappedStorageBuffer(renderer, static_cast<vk::DeviceSize>(capacity) * sizeof(GpuLight));
        if (!buffer)
        {
            return std::unexpected(buffer.error());
        }
        renderer.lightListBuffers[frame] = *buffer;
        renderer.lightListCapacity[frame] = capacity;

        // Both the fragment lighting set (binding 1) and the compute cluster set
        // (binding 1) read this buffer — rewrite both to the grown allocation.
        vk::DescriptorBufferInfo bufferInfo{ (*buffer)->buffer, 0, (*buffer)->size };
        std::array<vk::WriteDescriptorSet, 2> writes{};
        writes[0].dstSet = renderer.lightSets[frame];
        writes[0].dstBinding = 1;
        writes[0].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[0].setBufferInfo(bufferInfo);
        writes[1].dstSet = renderer.clusterSets[frame];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[1].setBufferInfo(bufferInfo);
        renderer.device.updateDescriptorSets(writes, {});
        return {};
    }

    void submitDrawList(Renderer& renderer, const glm::mat4& viewProj, const std::vector<DrawItem>& items)
    {
        renderer.stats = RenderStats{};
        renderer.sceneDrawList = SceneDrawList{};
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
            Ref<Pipeline> pipeline = requestMeshPipeline(renderer, item.material);
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
            bucket->instances.push_back(InstanceData{ item.model, item.normalMatrix, item.baseColor,
                                                      glm::uvec4{ textureIndex, 0, 0, 0 } });
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
        const u32 frame = renderer.frameIndex;
        if (std::expected<void, std::string> ok = ensureInstanceCapacity(renderer, frame, static_cast<u32>(instances.size())); !ok)
        {
            logError(ok.error());
            return;
        }
        const vk::DeviceSize bytes = instances.size() * sizeof(InstanceData);
        std::memcpy(renderer.instanceBuffers[frame]->mapped, instances.data(), bytes);
        vmaFlushAllocation(renderer.allocator, renderer.instanceBuffers[frame]->alloc, 0, bytes);

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
        list.lightSet = renderer.lightSets[frame];
        list.instanceSet = renderer.instanceSets[frame];
        list.valid = true;
        renderer.sceneDrawList = std::move(list);
    }

    // Record the scene's shaded geometry. All mesh PSOs share the layout, so the light +
    // instance sets and the viewProj push bind once; per batch then binds its pipeline +
    // material set and issues one instanced drawIndexed.
    void recordSceneDrawList(Renderer& renderer, vk::CommandBuffer cmd)
    {
        SceneDrawList& list = renderer.sceneDrawList;
        if (!list.valid || list.batches.empty())
        {
            return;
        }
        vk::PipelineLayout layout = list.batches[0].pipeline->layout;
        // All sets bind once: the bindless albedo array (0) + light (1) + instance (2).
        // Per-instance texture indices live in the instance buffer, so no per-batch set.
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, renderer.bindlessSet, {});
        std::array<vk::DescriptorSet, 2> frameSets{ list.lightSet, list.instanceSet };
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 1, frameSets, {});
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
        SceneDrawList& list = renderer.sceneDrawList;
        if (!list.valid || !renderer.depthPrepassPipeline)
        {
            return;
        }
        // The vertex-only pipeline needs only the instance set (set 2) + viewProj push.
        vk::PipelineLayout layout = renderer.depthPrepassPipeline->layout;
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, renderer.depthPrepassPipeline->pipeline);
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

    const Ref<GpuTexture>& defaultTexture(const Renderer& renderer)
    {
        return renderer.defaultWhiteTexture;
    }

    RenderStats renderStats(const Renderer& renderer)
    {
        return renderer.stats;
    }

    void waitGpuIdle(Renderer& renderer)
    {
        if (renderer.device)
        {
            static_cast<void>(renderer.device.waitIdle());
        }
    }

    void setDirectionalLight(Renderer& renderer, glm::vec3 direction, glm::vec3 color, f32 intensity, f32 ambient)
    {
        setSceneLighting(renderer, direction, color, intensity, ambient, {});
    }

    void setSceneLighting(Renderer& renderer, glm::vec3 direction, glm::vec3 color, f32 intensity,
                          f32 ambient, const std::vector<GpuLight>& lights)
    {
        // Write the current frame's copies; beginFrame already waited on its fence, so
        // no in-flight frame is reading them.
        const u32 frame = renderer.frameIndex;
        if (renderer.lightMapped[frame] == nullptr)
        {
            return;
        }
        const u32 count = static_cast<u32>(lights.size());
        if (count > 0)
        {
            if (std::expected<void, std::string> ok = ensureLightCapacity(renderer, frame, count); !ok)
            {
                logError(ok.error());
                return;
            }
            const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(count) * sizeof(GpuLight);
            std::memcpy(renderer.lightListBuffers[frame]->mapped, lights.data(), bytes);
            vmaFlushAllocation(renderer.allocator, renderer.lightListBuffers[frame]->alloc, 0, bytes);
        }

        LightUbo ubo;
        ubo.directionAmbient = glm::vec4(glm::normalize(direction), ambient);
        ubo.colorIntensity = glm::vec4(color, intensity);
        ubo.counts = glm::uvec4(count, 0, 0, 0);
        std::memcpy(renderer.lightMapped[frame], &ubo, sizeof(ubo));
        vmaFlushAllocation(renderer.allocator, renderer.lightAllocs[frame], 0, sizeof(ubo));
        renderer.frameLightCount = count;
    }

    void setClusterCamera(Renderer& renderer, const glm::mat4& view, const glm::mat4& proj,
                          f32 nearPlane, f32 farPlane)
    {
        const u32 frame = renderer.frameIndex;
        if (renderer.clusterParamMapped[frame] == nullptr)
        {
            return;
        }
        ClusterParams params;
        params.view = view;
        params.inverseProjection = glm::inverse(proj);
        params.gridSize = glm::uvec4(ClusterGridX, ClusterGridY, ClusterGridZ, renderer.frameLightCount);
        params.screenSize = glm::uvec4(viewportWidth(renderer), viewportHeight(renderer),
                                       renderer.useClustered ? 1u : 0u, 0u);
        params.zPlanes = glm::vec4(nearPlane, farPlane, 0.0f, 0.0f);
        std::memcpy(renderer.clusterParamMapped[frame], &params, sizeof(params));
        vmaFlushAllocation(renderer.allocator, renderer.clusterParamAllocs[frame], 0, sizeof(params));
        renderer.clusterDispatchPending = renderer.useClustered && renderer.frameLightCount > 0;
    }

    void setClustered(Renderer& renderer, bool enabled)
    {
        renderer.useClustered = enabled;
    }

    bool clusteredEnabled(const Renderer& renderer)
    {
        return renderer.useClustered;
    }

    void setPostProcess(Renderer& renderer, bool enabled)
    {
        renderer.usePostProcess = enabled;
    }

    bool postProcessEnabled(const Renderer& renderer)
    {
        return renderer.usePostProcess;
    }

    void setDepthPrepass(Renderer& renderer, bool enabled)
    {
        renderer.useDepthPrepass = enabled;
    }

    bool depthPrepassEnabled(const Renderer& renderer)
    {
        return renderer.useDepthPrepass;
    }

    void setAa(Renderer& renderer, u32 msaaSamples, bool fxaa)
    {
        vk::SampleCountFlagBits count = vk::SampleCountFlagBits::e1;
        if (msaaSamples >= 8) { count = vk::SampleCountFlagBits::e8; }
        else if (msaaSamples >= 4) { count = vk::SampleCountFlagBits::e4; }
        else if (msaaSamples >= 2) { count = vk::SampleCountFlagBits::e2; }
        if (static_cast<u32>(count) > static_cast<u32>(renderer.maxSampleCount))
        {
            count = renderer.maxSampleCount;
        }

        waitGpuIdle(renderer);
        renderer.sampleCount = count;
        renderer.fxaaEnabled = fxaa;
        recreateMsaaTargets(renderer);

        // The mesh + depth-prepass PSOs bake the sample count — rebuild them.
        renderer.pipelineCache.clear();
        std::expected<Ref<Pipeline>, std::string> depthPrepass =
            makeDepthPrepassPipeline(renderer, "shaders/mesh.spv");
        if (depthPrepass)
        {
            renderer.depthPrepassPipeline = *depthPrepass;
        }
        else
        {
            logError(depthPrepass.error());
        }
    }

    std::string aaMode(const Renderer& renderer)
    {
        if (renderer.fxaaEnabled)
        {
            return "fxaa";
        }
        const u32 n = static_cast<u32>(renderer.sampleCount);
        if (n <= 1)
        {
            return "off";
        }
        return std::format("msaa{}", n);
    }

    std::expected<Ref<GpuTexture>, std::string> uploadSvgIcon(Renderer& renderer, const std::string& svgPath,
                                                              u32 pixelSize, glm::vec4 tint)
    {
        std::ifstream in(svgPath);
        if (!in)
        {
            return std::unexpected(std::format("cannot open '{}'", svgPath));
        }
        std::string svg((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // nanosvg does not resolve the "currentColor" keyword; map it to white so
        // stroke-only icons (e.g. Lucide) rasterize, then tint below.
        for (std::size_t pos = svg.find("currentColor"); pos != std::string::npos; pos = svg.find("currentColor", pos))
        {
            svg.replace(pos, 12, "#ffffff");
        }

        NSVGimage* image = nsvgParse(svg.data(), "px", 96.0f);  // nsvgParse mutates the buffer
        if (image == nullptr || image->width <= 0.0f || image->height <= 0.0f)
        {
            if (image != nullptr)
            {
                nsvgDelete(image);
            }
            return std::unexpected(std::format("nanosvg failed to parse '{}'", svgPath));
        }
        NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
        if (rasterizer == nullptr)
        {
            nsvgDelete(image);
            return std::unexpected(std::string{ "nsvgCreateRasterizer failed" });
        }
        const f32 scale = static_cast<f32>(pixelSize) / glm::max(image->width, image->height);
        std::vector<u8> rgba(static_cast<std::size_t>(pixelSize) * pixelSize * 4, 0);
        nsvgRasterize(rasterizer, image, 0.0f, 0.0f, scale, rgba.data(),
                      static_cast<int>(pixelSize), static_cast<int>(pixelSize), static_cast<int>(pixelSize) * 4);
        nsvgDeleteRasterizer(rasterizer);
        nsvgDelete(image);

        for (std::size_t i = 0; i < rgba.size(); i = i + 4)
        {
            rgba[i + 0] = static_cast<u8>(rgba[i + 0] * tint.r);
            rgba[i + 1] = static_cast<u8>(rgba[i + 1] * tint.g);
            rgba[i + 2] = static_cast<u8>(rgba[i + 2] * tint.b);
            rgba[i + 3] = static_cast<u8>(rgba[i + 3] * tint.a);
        }
        return uploadTexture(renderer, rgba.data(), pixelSize, pixelSize, false);
    }

    // The minimal mesh-thumbnail pipeline (vertex input + a 2x mat4 push constant, no
    // descriptor sets). Color format matches the offscreen thumbnail image.
    std::expected<Ref<Pipeline>, std::string> newThumbnailPipeline(Renderer& renderer)
    {
        auto moduleResult = loadShaderModule(renderer.device, assetPath("shaders/thumbnail.spv"));
        if (!moduleResult)
        {
            return std::unexpected(moduleResult.error());
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
            vk::VertexInputAttributeDescription{ 2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, uv0) } };
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
        renderingInfo.setColorAttachmentFormats(renderer.swapchainFormat);
        renderingInfo.depthAttachmentFormat = DepthFormat;

        vk::PushConstantRange pushConstant{};
        pushConstant.stageFlags = vk::ShaderStageFlagBits::eVertex;
        pushConstant.offset = 0;
        pushConstant.size = 2 * sizeof(glm::mat4);  // mvp + normalMatrix

        vk::PipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.setPushConstantRanges(pushConstant);
        auto layoutResult = checked(renderer.device.createPipelineLayout(layoutInfo), "createPipelineLayout (thumbnail)");
        if (!layoutResult)
        {
            renderer.device.destroyShaderModule(shaderModule);
            return std::unexpected(layoutResult.error());
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

        vk::ResultValue<vk::Pipeline> created = renderer.device.createGraphicsPipeline(nullptr, pipelineInfo);
        renderer.device.destroyShaderModule(shaderModule);
        if (created.result != vk::Result::eSuccess)
        {
            renderer.device.destroyPipelineLayout(*layoutResult);
            return std::unexpected(std::format("createGraphicsPipeline (thumbnail): {}", vk::to_string(created.result)));
        }
        Pipeline pipeline;
        pipeline.device = renderer.device;
        pipeline.pipeline = created.value;
        pipeline.layout = *layoutResult;
        return std::make_shared<Pipeline>(std::move(pipeline));
    }

    std::expected<Ref<GpuTexture>, std::string> renderMeshThumbnail(Renderer& renderer, const Ref<GpuMesh>& mesh, u32 size)
    {
        if (!mesh)
        {
            return std::unexpected(std::string{ "renderMeshThumbnail: null mesh" });
        }
        if (!renderer.thumbnailPipeline)
        {
            std::expected<Ref<Pipeline>, std::string> pipeline = newThumbnailPipeline(renderer);
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            renderer.thumbnailPipeline = *pipeline;
        }

        std::expected<Image, std::string> colorImage = newColorImage(renderer, size, size, renderer.swapchainFormat);
        if (!colorImage)
        {
            return std::unexpected(colorImage.error());
        }
        Image color = std::move(*colorImage);
        std::expected<Image, std::string> depthImage = newDepthImage(renderer, size, size);
        if (!depthImage)
        {
            return std::unexpected(depthImage.error());
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
        glm::mat4 proj = glm::perspective(fovy, 1.0f, glm::max(0.01f, distance - radius * 2.0f), distance + radius * 2.0f);
        proj[1][1] *= -1.0f;  // Vulkan clip; matches the viewport so the thumbnail is upright
        struct ThumbnailPush
        {
            glm::mat4 mvp;
            glm::mat4 normalMatrix;
        } push{ proj * view, glm::mat4(1.0f) };

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = renderer.frames[0].commandPool;
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(renderer.device.allocateCommandBuffers(cmdAlloc), "renderMeshThumbnail: allocateCommandBuffers");
        if (!cmds)
        {
            return std::unexpected(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];
        vk::CommandBufferBeginInfo begin{};
        begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(begin));

        transitionImage(cmd, color.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
                        vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                        vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite);
        transitionImage(cmd, depth.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthAttachmentOptimal,
                        vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                        vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests,
                        vk::AccessFlagBits2::eDepthStencilAttachmentWrite, vk::ImageAspectFlagBits::eDepth);

        vk::RenderingAttachmentInfo colorAttach{};
        colorAttach.imageView = color.view;
        colorAttach.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        colorAttach.loadOp = vk::AttachmentLoadOp::eClear;
        colorAttach.storeOp = vk::AttachmentStoreOp::eStore;
        colorAttach.clearValue = vk::ClearValue{ vk::ClearColorValue{ std::array<f32, 4>{ 0.12f, 0.12f, 0.14f, 1.0f } } };
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
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, renderer.thumbnailPipeline->pipeline);
        cmd.pushConstants(renderer.thumbnailPipeline->layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(push), &push);
        vk::DeviceSize offset = 0;
        cmd.bindVertexBuffers(0, mesh->vertexBuffer, offset);
        cmd.bindIndexBuffer(mesh->indexBuffer, 0, vk::IndexType::eUint32);
        for (const Submesh& submesh : mesh->submeshes)
        {
            cmd.drawIndexed(submesh.indexCount, 1, submesh.firstIndex, submesh.vertexOffset, 0);
        }
        cmd.endRendering();

        transitionImage(cmd, color.image, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
                        vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
        static_cast<void>(cmd.end());

        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = cmd;
        vk::SubmitInfo2 submitInfo{};
        submitInfo.setCommandBufferInfos(cmdInfo);
        static_cast<void>(renderer.graphicsQueue.submit2(submitInfo, nullptr));
        static_cast<void>(renderer.device.waitIdle());
        renderer.device.freeCommandBuffers(renderer.frames[0].commandPool, cmd);

        // Take ownership of the color image as a sampled GpuTexture (no material set;
        // ImGui samples it via uiRegisterTexture). Null the Image's handles so it does
        // not free them on scope exit.
        GpuTexture texture;
        texture.device = renderer.device;
        texture.allocator = renderer.allocator;
        texture.image = color.image;
        texture.view = color.view;
        texture.alloc = color.alloc;
        texture.extent = color.extent;
        texture.format = color.format;
        color.image = nullptr;
        color.view = nullptr;
        color.alloc = nullptr;
        return std::make_shared<GpuTexture>(std::move(texture));
    }

    std::expected<Ref<GpuTexture>, std::string> uploadTexture(Renderer& renderer, const u8* rgba, u32 width, u32 height, bool srgb)
    {
        if (width == 0 || height == 0)
        {
            return std::unexpected(std::string{ "uploadTexture: zero-sized image" });
        }
        const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(width) * height * 4;

        VkBufferCreateInfo stagingInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        stagingInfo.size = bytes;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo stagingAlloc{};
        stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = nullptr;
        VmaAllocationInfo stagingMapped{};
        if (vmaCreateBuffer(renderer.allocator, &stagingInfo, &stagingAlloc, &staging, &stagingAllocation, &stagingMapped) != VK_SUCCESS)
        {
            return std::unexpected(std::string{ "uploadTexture: staging vmaCreateBuffer failed" });
        }
        std::memcpy(stagingMapped.pMappedData, rgba, bytes);
        vmaFlushAllocation(renderer.allocator, stagingAllocation, 0, VK_WHOLE_SIZE);

        const vk::Format format = srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm;
        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = static_cast<VkFormat>(format);
        imageInfo.extent = VkExtent3D{ width, height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VmaAllocationCreateInfo imageAlloc{};
        imageAlloc.usage = VMA_MEMORY_USAGE_AUTO;
        imageAlloc.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        VkImage rawImage = VK_NULL_HANDLE;
        VmaAllocation imageAllocation = nullptr;
        if (vmaCreateImage(renderer.allocator, &imageInfo, &imageAlloc, &rawImage, &imageAllocation, nullptr) != VK_SUCCESS)
        {
            vmaDestroyBuffer(renderer.allocator, staging, stagingAllocation);
            return std::unexpected(std::string{ "uploadTexture: vmaCreateImage failed" });
        }

        vk::CommandBufferAllocateInfo cmdAlloc{};
        cmdAlloc.commandPool = renderer.frames[0].commandPool;
        cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
        cmdAlloc.commandBufferCount = 1;
        auto cmds = checked(renderer.device.allocateCommandBuffers(cmdAlloc), "uploadTexture: allocateCommandBuffers");
        if (!cmds)
        {
            vmaDestroyImage(renderer.allocator, rawImage, imageAllocation);
            vmaDestroyBuffer(renderer.allocator, staging, stagingAllocation);
            return std::unexpected(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];
        vk::CommandBufferBeginInfo begin{};
        begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(begin));
        transitionImage(cmd, vk::Image{ rawImage },
            vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
        vk::BufferImageCopy region{};
        region.imageSubresource = vk::ImageSubresourceLayers{ vk::ImageAspectFlagBits::eColor, 0, 0, 1 };
        region.imageExtent = vk::Extent3D{ width, height, 1 };
        cmd.copyBufferToImage(vk::Buffer{ staging }, vk::Image{ rawImage }, vk::ImageLayout::eTransferDstOptimal, region);
        transitionImage(cmd, vk::Image{ rawImage },
            vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
        static_cast<void>(cmd.end());
        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = cmd;
        vk::SubmitInfo2 submitInfo{};
        submitInfo.setCommandBufferInfos(cmdInfo);
        static_cast<void>(renderer.graphicsQueue.submit2(submitInfo, nullptr));
        static_cast<void>(renderer.device.waitIdle());
        renderer.device.freeCommandBuffers(renderer.frames[0].commandPool, cmd);
        vmaDestroyBuffer(renderer.allocator, staging, stagingAllocation);

        vk::ImageViewCreateInfo viewInfo{};
        viewInfo.image = vk::Image{ rawImage };
        viewInfo.viewType = vk::ImageViewType::e2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = vk::ImageSubresourceRange{ vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
        auto view = checked(renderer.device.createImageView(viewInfo), "uploadTexture: createImageView");
        if (!view)
        {
            vmaDestroyImage(renderer.allocator, rawImage, imageAllocation);
            return std::unexpected(view.error());
        }

        // Claim a bindless slot and write the texture into the global array.
        const u32 index = renderer.nextBindlessIndex;
        renderer.nextBindlessIndex = renderer.nextBindlessIndex + 1;
        writeBindlessTexture(renderer, *view, index);

        GpuTexture texture;
        texture.device = renderer.device;
        texture.allocator = renderer.allocator;
        texture.image = vk::Image{ rawImage };
        texture.view = *view;
        texture.alloc = imageAllocation;
        texture.extent = vk::Extent2D{ width, height };
        texture.format = format;
        texture.bindlessIndex = index;
        return std::make_shared<GpuTexture>(std::move(texture));
    }

    std::expected<void, std::string> captureViewport(Renderer& renderer, const std::string& path)
    {
        Image& img = renderer.offscreenViewport;
        const u32 width = img.extent.width;
        const u32 height = img.extent.height;
        const vk::DeviceSize byteSize = static_cast<vk::DeviceSize>(width) * height * 4;

        // The offscreen image may still be sampled by an in-flight frame; idle so
        // the capture's layout transition cannot race that read.
        static_cast<void>(renderer.device.waitIdle());

        VkBuffer rawBuffer = VK_NULL_HANDLE;
        VmaAllocation bufferAllocation = nullptr;
        VmaAllocationInfo bufferAllocInfo{};
        if (auto created = newHostCaptureBuffer(renderer, byteSize, rawBuffer, bufferAllocation, bufferAllocInfo); !created)
        {
            return std::unexpected(created.error());
        }

        vk::CommandBufferAllocateInfo allocInfo{};
        allocInfo.commandPool = renderer.frames[0].commandPool;
        allocInfo.level = vk::CommandBufferLevel::ePrimary;
        allocInfo.commandBufferCount = 1;
        auto cmds = checked(renderer.device.allocateCommandBuffers(allocInfo), "capture: allocateCommandBuffers");
        if (!cmds)
        {
            vmaDestroyBuffer(renderer.allocator, rawBuffer, bufferAllocation);
            return std::unexpected(cmds.error());
        }
        vk::CommandBuffer cmd = (*cmds)[0];

        vk::CommandBufferBeginInfo beginInfo{};
        beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        static_cast<void>(cmd.begin(beginInfo));

        // Leave the image in ShaderReadOnly so the next frame's producer barrier holds.
        vk::PipelineStageFlags2 fromStage = vk::PipelineStageFlagBits2::eFragmentShader;
        vk::AccessFlags2 fromAccess = vk::AccessFlagBits2::eShaderSampledRead;
        if (img.layout != vk::ImageLayout::eShaderReadOnlyOptimal)
        {
            fromStage = vk::PipelineStageFlagBits2::eTopOfPipe;
            fromAccess = vk::AccessFlagBits2::eNone;
        }
        captureImageToBuffer(
            cmd, img.image, img.extent,
            img.layout, fromStage, fromAccess,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
            vk::Buffer{ rawBuffer });
        img.layout = vk::ImageLayout::eShaderReadOnlyOptimal;

        static_cast<void>(cmd.end());

        vk::CommandBufferSubmitInfo cmdInfo{};
        cmdInfo.commandBuffer = cmd;
        vk::SubmitInfo2 submitInfo{};
        submitInfo.setCommandBufferInfos(cmdInfo);
        static_cast<void>(renderer.graphicsQueue.submit2(submitInfo, nullptr));
        static_cast<void>(renderer.device.waitIdle());
        renderer.device.freeCommandBuffers(renderer.frames[0].commandPool, cmd);
        vmaInvalidateAllocation(renderer.allocator, bufferAllocation, 0, VK_WHOLE_SIZE);

        std::expected<void, std::string> wrote = writeBufferToPng(
            static_cast<const unsigned char*>(bufferAllocInfo.pMappedData), width, height, img.format, path);
        vmaDestroyBuffer(renderer.allocator, rawBuffer, bufferAllocation);
        if (!wrote)
        {
            return std::unexpected(wrote.error());
        }
        logInfo(std::format("captured viewport ({}x{}) to {}", width, height, path));
        return {};
    }

    std::expected<void, std::string> requestWindowCapture(Renderer& renderer, std::string path)
    {
        if (!renderer.swapchainCaptureSupported)
        {
            return std::unexpected(std::string{ "window capture unsupported: surface lacks TRANSFER_SRC usage" });
        }
        renderer.captureNextSwapchainPath = std::move(path);
        return {};
    }
}
