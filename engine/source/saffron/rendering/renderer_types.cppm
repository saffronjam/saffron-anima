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
        // FXAA: when on, the scene renders to offscreenScratch (1x sampled), and a compute
        // pass edge-blurs it into the offscreen. set 0: 0 = source sampler, 1 = target storage.
        Image offscreenScratch;
        Ref<Pipeline> fxaaPipeline;
        vk::DescriptorSetLayout fxaaSetLayout;
        vk::DescriptorSet fxaaSet;
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
    // (e.g. post-process) declares its reads/writes against this handle.
    auto viewportColorResource(const Renderer& renderer) -> RgResource;
    // Add an in-place post-process tonemap pass on the offscreen color (app-authored;
    // called from a layer's onRenderGraph when post-process is enabled).
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
    auto clusteredEnabled(const Renderer& renderer) -> bool;
    void setPostProcess(Renderer& renderer, bool enabled);
    auto postProcessEnabled(const Renderer& renderer) -> bool;
    void setDepthPrepass(Renderer& renderer, bool enabled);
    auto depthPrepassEnabled(const Renderer& renderer) -> bool;
    // Anti-aliasing: msaaSamples is 1 (off) / 2 / 4 / 8 (clamped to the device cap); fxaa
    // toggles the post-process pass. Recreates the MSAA targets + rebuilds scene PSOs.
    void setAa(Renderer& renderer, u32 msaaSamples, bool fxaa);
    auto aaMode(const Renderer& renderer) -> std::string;  // "off" | "fxaa" | "msaa2|4|8"

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
