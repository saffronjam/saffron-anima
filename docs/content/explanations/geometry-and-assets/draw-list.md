+++
title = 'Draw list'
weight = 8
+++

# Draw list

A draw list is a flat snapshot of everything the scene wants drawn this frame, gathered once
from the ECS into plain data and then replayed by every geometry pass. It sits between the
scene description and the Vulkan commands: the scene is walked one time into a list of items,
and the [render graph](../../frame-and-render-graph/render-graph-overview/) passes consume
that list rather than the live scene.

Decoupling the gather from the record keeps the scene traversal in one place. A frame draws
the same geometry several times — shaded color, depth, shadows, G-buffer, motion — and each
pass reads from the same prepared list instead of walking the registry again.

## Gather: ECS into DrawItem

`renderScene` iterates every entity with a `TransformComponent` and a `MeshComponent`,
resolves the mesh through [`loadMeshAsset`](../asset-server-and-catalog/), reads the optional
`MaterialComponent`, and emits one `DrawItem`:

```cpp
struct DrawItem
{
    Ref<GpuMesh> mesh;
    Ref<GpuTexture> texture;     // null => default white
    glm::mat4 model, normalMatrix;
    glm::vec4 baseColor;
    f32 metallic, roughness;
    glm::vec3 emissive; f32 emissiveStrength;
    Material material;           // selects the PSO variant (e.g. unlit)
};
```

The same loop also accumulates the world-space scene bounds: it transforms each mesh's local
AABB by its model matrix, and those bounds fit the
[directional shadow](../../shadows-and-culling/directional-shadows/) frustum and the DDGI
probe volume. The draw list and the scene extents are gathered in one pass. `renderScene`
also collects lights, sets the shadow/cluster/SSAO camera state, and finally calls
`submitDrawList(renderer, viewProjection, items)`.

## Bucket: DrawItem into instanced batches

`submitDrawList` groups items into batches keyed on `(pipeline, mesh)`. The key omits the
texture. Albedo is [bindless](../../materials-and-pipelines/bindless-textures/) — a
per-instance index into one global texture array, carried in the instance data — so two
items that differ only by texture still batch together. Each item becomes one `InstanceData`
(model matrix, normal matrix, base color, bindless texture index, metallic/roughness,
emissive). The buckets flatten into one contiguous instance array plus per-batch
`(baseInstance, instanceCount)` ranges, written into the frame's instance SSBO.

The result is stored on the frame, not recorded immediately:

```cpp
struct SceneDrawList
{
    glm::mat4 viewProj;
    std::vector<DrawBatch> batches;
    std::vector<Ref<GpuTexture>> liveTextures;  // pins indexed textures for the frame
    vk::DescriptorSet lightSet, instanceSet;
    bool valid;
};
```

`liveTextures` holds a `Ref` to every texture an instance indexed, so a texture cannot be
freed mid-frame while a bindless slot still points at it.

## Replay: one list, many passes

A single `SceneDrawList` feeds every geometry pass in the frame, each recording the same
batches with a different pipeline and push constant:

```mermaid
flowchart TD
    A[renderScene gathers DrawItems] --> B[submitDrawList buckets + stores SceneDrawList]
    B --> C[recordSceneDrawList — shaded color]
    B --> D[recordDepthPrepass — depth only]
    B --> E[recordShadowDepth — light-space depth]
    B --> F[recordGbuffer — view normal + Z]
    B --> G[recordMotion — motion vectors]
    B --> H[recordPointShadow — cube faces]
```

The shaded pass `recordSceneDrawList` binds the bindless, light, instance, IBL, and
screen-space sets once, pushes the camera `viewProj`, then per batch binds its pipeline and
mesh buffers and issues one `drawIndexed` per submesh with the batch's `instanceCount` and
`baseInstance`. The depth, shadow, G-buffer, and motion passes are vertex-only variants of
the same loop: they bind only the instance set, push a different matrix, and skip the
material binds. All iterate `batch.mesh->submeshes` identically.

## Stats

`submitDrawList` fills `RenderStats` while flattening: draw calls (one `drawIndexed` per
submesh per batch), batch count, and total instances. These are inspectable through the
control plane, which verifies instanced batching live — two textured cubes collapsing to one
batch show up as `batches = 1`.

## In the code

| What | File | Symbols |
|---|---|---|
| Gather ECS → items | `assets.cppm` | `renderScene` |
| Bucket + store | `renderer_drawlist.cpp` | `submitDrawList`, `DrawBatch`, `SceneDrawList` |
| Per-instance data | `renderer_types.cppm` | `InstanceData` |
| Instance buffer growth | `renderer_drawlist.cpp` | `ensureInstanceCapacity` |
| Shaded replay | `renderer_drawlist.cpp` | `recordSceneDrawList` |
| Vertex-only replays | `renderer_drawlist.cpp` | `recordDepthPrepass`, `recordShadowDepth`, `recordGbuffer`, `recordMotion`, `recordPointShadow` |

> [!NOTE]
> The draw loop iterates `batch.mesh->submeshes` and ignores each submesh's `materialSlot`.
> The material (PSO variant, base color, albedo) is per-`DrawItem`, i.e. per entity, so a
> multi-submesh mesh draws every submesh with the same material. Per-submesh materials are
> reserved, not wired; see [vertex layout](../mesh-and-vertex-layout/).

## Related

- [Asset catalog](../asset-server-and-catalog/) — resolves each item's mesh + texture
- [Bindless textures](../../materials-and-pipelines/bindless-textures/) — why texture isn't a batch key
- [Material and PSO selection](../../materials-and-pipelines/material-and-pso-selection/) — the per-item PSO
- [Render graph](../../frame-and-render-graph/render-graph-overview/) — the passes that replay the list
- [Render commands](../../tooling-and-control/render-commands/) — reading the batch/draw stats live
