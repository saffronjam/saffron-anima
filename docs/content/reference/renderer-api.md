+++
title = 'Renderer API'
weight = 4
math = false
+++

# Renderer API

Exported entry points of `Saffron.Rendering` (the `:Types` partition). All take a `Renderer&`. Fallible calls return `Result<…>`.

## Lifecycle
| Symbol | Signature | Effect |
|---|---|---|
| `newRenderer` | `auto newRenderer(Window&) -> Result<Renderer>` | instance/device/swapchain/allocator + descriptors + IBL bake |
| `destroyRenderer` | `void destroyRenderer(Renderer&)` | waits idle, frees in teardown order |
| `waitGpuIdle` | `void waitGpuIdle(Renderer&)` | block until all submitted work finishes |

## Per-frame
| Symbol | Signature | Effect |
|---|---|---|
| `beginFrame` | `auto beginFrame(Renderer&) -> bool` | acquire image; `false` if it recreated the swapchain/targets (skip the frame) |
| `submit` | `void submit(Renderer&, RenderFn)` | record a closure into the scene (offscreen) pass |
| `submitUi` | `void submitUi(Renderer&, RenderFn)` | record a closure into the ui (swapchain) pass |
| `beginFrameGraph` | `void beginFrameGraph(Renderer&)` | build the frame graph (cull + scene + AA + tonemap) before layer passes |
| `frameGraph` | `auto frameGraph(Renderer&) -> RenderGraph&` | the in-progress graph |
| `viewportColorResource` | `auto viewportColorResource(const Renderer&) -> RgResource` | offscreen color handle for an app pass |
| `addTonemapPass` | `void addTonemapPass(Renderer&, RenderGraph&)` | the mandatory HDR→display pass |
| `endFrame` | `void endFrame(Renderer&)` | finish graph (ui pass), execute, present, run pending window capture |

`RenderFn` is `std::function<void(vk::CommandBuffer)>`.

## Viewport target
| Symbol | Signature |
|---|---|
| `setViewportDesiredSize` | `void setViewportDesiredSize(Renderer&, u32 w, u32 h)` |
| `viewportImageView` | `auto viewportImageView(const Renderer&) -> vk::ImageView` |
| `viewportGeneration` | `auto viewportGeneration(const Renderer&) -> u32` |
| `viewportWidth` / `viewportHeight` | `auto …(const Renderer&) -> u32` |

## Resource upload
| Symbol | Signature |
|---|---|
| `uploadMesh` | `auto uploadMesh(Renderer&, const Mesh&) -> Result<Ref<GpuMesh>>` |
| `uploadTexture` | `auto uploadTexture(Renderer&, const u8* rgba, u32 w, u32 h, bool srgb) -> Result<Ref<GpuTexture>>` |
| `uploadSvgIcon` | `auto uploadSvgIcon(Renderer&, const std::string& svgPath, u32 pixelSize, glm::vec4 tint) -> Result<Ref<GpuTexture>>` |
| `renderMeshThumbnail` | `auto renderMeshThumbnail(Renderer&, const Ref<GpuMesh>&, u32 size) -> Result<Ref<GpuTexture>>` |
| `defaultTexture` | `auto defaultTexture(const Renderer&) -> const Ref<GpuTexture>&` |

## Pipelines (PSO cache)
| Symbol | Signature | Effect |
|---|---|---|
| `requestMeshPipeline` | `auto requestMeshPipeline(Renderer&, const Material&) -> Ref<Pipeline>` | cache front door; build on miss |
| `newMeshPipeline` | `auto newMeshPipeline(Renderer&, std::string_view shaderName, bool unlit = false) -> Result<Ref<Pipeline>>` | build a mesh PSO |
| `pipelineCount` | `auto pipelineCount(const Renderer&) -> u32` | distinct cached mesh PSOs |

## Draw list
| Symbol | Signature | Effect |
|---|---|---|
| `submitDrawList` | `void submitDrawList(Renderer&, const glm::mat4& viewProj, const std::vector<DrawItem>&)` | resolve materials → batch by (pipeline, mesh) → upload instance buffer |
| `recordSceneDrawList` | `void recordSceneDrawList(Renderer&, vk::CommandBuffer)` | scene-pass body |
| `recordDepthPrepass` | `void recordDepthPrepass(Renderer&, vk::CommandBuffer)` | depth-pre-pass body |
| `renderStats` | `auto renderStats(const Renderer&) -> RenderStats` | last frame's draw counters |

## Lighting
| Symbol | Signature |
|---|---|
| `setDirectionalLight` | `void setDirectionalLight(Renderer&, glm::vec3 direction, glm::vec3 color, f32 intensity, f32 ambient)` |
| `setSceneLighting` | `void setSceneLighting(Renderer&, glm::vec3 dir, glm::vec3 color, f32 intensity, f32 ambient, glm::vec3 eye, const std::vector<GpuLight>&)` |
| `setClusterCamera` | `void setClusterCamera(Renderer&, const glm::mat4& view, const glm::mat4& proj, f32 near, f32 far)` |
| `setClustered` / `clusteredEnabled` | toggle / query clustered culling |

## Feature toggles (paired set/query)
| Symbol | Signature |
|---|---|
| `setExposure(Renderer&, f32 ev)` / `exposureEv(const Renderer&) -> f32` | tonemap exposure in stops |
| `setIbl(Renderer&, bool)` / `iblEnabled(const Renderer&) -> bool` | IBL ambient vs flat |
| `setSsao` / `ssaoEnabled` | GTAO |
| `setContactShadows` / `contactShadowsEnabled` | screen-space contact shadows |
| `setSsgi` / `ssgiEnabled` | screen-space GI |
| `screenEffectsEnabled(const Renderer&) -> bool` | any screen effect on |
| `setDdgi` / `ddgiEnabled` | DDGI probe GI |
| `setShadows` / `shadowsEnabled` | directional shadow map |
| `setDepthPrepass` / `depthPrepassEnabled` | depth pre-pass |
| `setRtShadows` / `rtShadowsEnabled`, `rtSupported`, `rtBlasCount` | hardware ray-query shadows |
| `setRestir` / `restirEnabled` | ReSTIR many-light direct |
| `setAa(Renderer&, u32 msaaSamples, bool fxaa, bool taa)` / `aaMode(const Renderer&) -> std::string` | anti-aliasing (`"off"`/`"fxaa"`/`"taa"`/`"msaa2\|4\|8"`) |

## Shadow / screen-space arming (per frame)
| Symbol | Signature |
|---|---|
| `setDirectionalShadow` | `void setDirectionalShadow(Renderer&, const glm::mat4& lightViewProj, bool casting)` |
| `setSpotShadow` | `void setSpotShadow(Renderer&, const glm::mat4& lightViewProj, u32 lightIndex, bool casting)` |
| `setPointShadow` | `void setPointShadow(Renderer&, glm::vec3 lightPos, f32 farPlane, u32 lightIndex, bool casting)` |
| `recordShadowDepth` | `void recordShadowDepth(Renderer&, vk::CommandBuffer, const glm::mat4& lightViewProj)` |
| `recordPointShadow` | `void recordPointShadow(Renderer&, vk::CommandBuffer, glm::vec3 lightPos, f32 farPlane)` |
| `setSsaoCamera` | `void setSsaoCamera(Renderer&, const glm::mat4& view, const glm::mat4& proj, glm::vec3 sunDirWorld)` |
| `recordGbuffer` | `void recordGbuffer(Renderer&, vk::CommandBuffer)` |
| `recordMotion` | `void recordMotion(Renderer&, vk::CommandBuffer)` |
| `setDdgiScene` | `void setDdgiScene(Renderer&, boxMins, boxMaxs, boxAlbedos, volumeMin, volumeExtent, sunDir, sunColor, sunIntensity)` |
| `setRtScene` | `void setRtScene(Renderer&, std::vector<glm::mat4> models, std::vector<Ref<GpuMesh>> meshes)` |
| `buildTlas` | `void buildTlas(Renderer&, vk::CommandBuffer, const std::vector<glm::mat4>& models, const std::vector<Ref<GpuMesh>>& meshes)` |

## Capture
| Symbol | Signature | Effect |
|---|---|---|
| `captureViewport` | `auto captureViewport(Renderer&, const std::string& path) -> Result<void>` | synchronous PNG of the offscreen |
| `requestWindowCapture` | `auto requestWindowCapture(Renderer&, std::string path) -> Result<void>` | PNG of the next presented frame (in `endFrame`) |
| `assetPath` | `auto assetPath(std::string_view relative) -> std::string` | resolve a path next to the exe |

## Constants
| Symbol | Value |
|---|---|
| `MaxFramesInFlight` | `2` |
| `MaxBindlessTextures` | `1024` |
| `DepthFormat` | `vk::Format::eD32Sfloat` |
| `OffscreenColorFormat` | `vk::Format::eR16G16B16A16Sfloat` |

## Key data structs
| Type | Fields (abridged) |
|---|---|
| `Material` | `std::string shader = "shaders/mesh.spv"`; `bool unlit = false` |
| `DrawItem` | `Ref<GpuMesh> mesh`; `Ref<GpuTexture> texture`; `glm::mat4 model`, `normalMatrix`; `glm::vec4 baseColor`; `f32 metallic`, `roughness`; `glm::vec3 emissive`; `f32 emissiveStrength`; `Material material` |
| `RenderStats` | `u32 drawCalls`, `batches`, `instances` |
| `InstanceData` | `glm::mat4 model`, `normalMatrix`; `glm::vec4 baseColor`; `glm::uvec4 texture` (.x = bindless index); `glm::vec4 pbr` (x=metallic, y=roughness); `glm::vec4 emissive` |
| `GpuLight` | `glm::vec4 positionRange`, `colorIntensity`, `directionType` (w: 0=point, 1=spot), `spotCos` |

## Related
- [Render seams](../../explanations/app-lifecycle-and-window/the-submit-and-rendergraph-seams/) — how `submit`/`onRenderGraph` feed the frame
- [Material and PSO selection](../../explanations/materials-and-pipelines/material-and-pso-selection/) — what `requestMeshPipeline` keys on
- [Meta-layer resources](../../explanations/vulkan-foundation/meta-layer-resources/) — `Ref<GpuMesh>`/`GpuTexture`/`Pipeline` ownership
