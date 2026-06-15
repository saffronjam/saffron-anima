+++
title = 'Custom Slang shader'
weight = 2
+++

# Custom Slang shader

This tutorial writes a new Slang shader, lets CMake compile it to SPIR-V, and draws scene
meshes with it through the renderer's PSO cache. The shader matches the engine's mesh I/O
contract, so it slots into the existing scene pass. Changing the fragment math changes how
every mesh using that material looks.

## How shaders get compiled

The build globs every `*.slang` under `engine/assets/shaders/` and compiles each to
`bin/shaders/<name>.spv` with `slangc`, wired in `engine/CMakeLists.txt`:

```cmake
saffron_compile_shaders(SaffronAnima
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders
    ${SAFFRON_RUNTIME_DIR}/shaders)
```

`saffron_compile_shaders` (in `cmake/CompileShaders.cmake`) runs, per file:

```
slangc <shader>.slang -profile glsl_450 -target spirv -emit-spirv-directly \
        -fvk-use-entrypoint-name -matrix-layout-column-major -o <shader>.spv
```

Adding a `.slang` to that folder compiles it with no CMake edit. The glob uses
`CONFIGURE_DEPENDS`, so re-running the build picks up the new file. Both entry points live
in one `.slang` module, named by their `[shader(...)]` tag.

## Write the shader

A shader the scene pass can draw with honors the contract `mesh.slang` defines. The vertex
stage consumes the interleaved vertex buffer (`position`/`normal`/`uv0`), reads per-instance
data from the storage buffer on set 2 indexed by `SV_VulkanInstanceID`, and takes the camera
`viewProj` as a push constant. The fragment stage returns `SV_Target`.

Create `engine/assets/shaders/flat.slang`:

```hlsl
// Draws each instance in its base color, lit by one hard-coded headlight.
// Matches mesh.slang's vertex inputs, set 2 (instances), and the camera push
// constant so the scene pass can draw with it unchanged.

struct VertexInput
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 worldNormal : NORMAL;
    float4 baseColor : COLOR0;
};

struct Camera { float4x4 viewProj; };
[[vk::push_constant]] Camera camera;

struct Instance
{
    float4x4 model;
    float4x4 normalMatrix;
    float4 baseColor;
    uint4 texture;
    float4 pbr;
    float4 emissive;
};
[[vk::binding(0, 2)]] StructuredBuffer<Instance> instances;

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint instanceIndex : SV_VulkanInstanceID)
{
    Instance inst = instances[instanceIndex];
    VertexOutput output;
    float4 worldPos = mul(inst.model, float4(input.position, 1.0));
    output.position = mul(camera.viewProj, worldPos);
    output.worldNormal = mul((float3x3)inst.normalMatrix, input.normal);
    output.baseColor = inst.baseColor;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    float3 n = normalize(input.worldNormal);
    float ndotl = saturate(dot(n, normalize(float3(0.3, 0.6, 1.0))));
    float3 shade = input.baseColor.rgb * (0.2 + 0.8 * ndotl);
    return float4(shade, input.baseColor.a);
}
```

The `Instance` struct must keep the same field order and layout as `mesh.slang`'s: the
renderer fills that storage buffer from its `DrawItem` instances regardless of which shader
draws them. Unused fields can be ignored (here: texture, pbr, emissive), but the order is
fixed.

> [!NOTE]
> The entry points must be named `vertexMain` and `fragmentMain` — that's what
> `newMeshPipeline` looks up when it builds the stage create-infos
> (`renderer_pipelines.cpp`). The vertex input layout (3 attributes) and set 2 (instances)
> are baked into that pipeline layout.

## Build it

Rebuild so the new `.slang` compiles, then confirm the SPIR-V landed:

```sh
./cmd/sa start --build      # CONFIGURE_DEPENDS sees flat.slang, builds it
ls build/debug/bin/shaders/flat.spv
```

If `slangc` rejects the file, the build fails with the line and message. The `.spv` under
`bin/shaders/` is what the renderer loads at run time via `assetPath("shaders/flat.spv")`.

## Draw with it

The renderer picks a pipeline per material. A `Material` carries a `shader` path (default
`"shaders/mesh.spv"`), and `requestMeshPipeline` builds and caches one PSO per distinct
`(shader, unlit)` key. Point a material at the new shader:

```cpp
Material flat;
flat.shader = "shaders/flat.spv";   // the .spv you just compiled
// attach `flat` to the DrawItems for the meshes you want drawn with it
```

The PSO cache builds `flat.spv` on first use and reuses it after. Check the pipeline count
once a mesh draws with it:

```sh
./cmd/sa render-stats       # "pipelines" increments when flat.spv's PSO is built
```

> [!NOTE]
> `Material.shader` is selected in engine code, not over the CLI — there's no `sa
> set-shader`. To see your shader live: assign the `Material` in the layer that builds the
> scene draw list, or edit the engine's `mesh.slang` in place so every mesh redraws with
> your changes on the next build. See
> [material and PSO selection](../../explanations/materials-and-pipelines/material-and-pso-selection/).

## Next

- [Übershader](../../explanations/materials-and-pipelines/ubershader-and-specialization/) — one shader for many materials via a spec constant.
- [Material and PSO selection](../../explanations/materials-and-pipelines/material-and-pso-selection/) — how `Material.shader` becomes a cached pipeline.
- [Vertex layout](../../explanations/geometry-and-assets/mesh-and-vertex-layout/) — the vertex inputs your shader has to match.
- [Descriptor sets](../../explanations/materials-and-pipelines/descriptor-sets/) — what sets 0–7 bind.
- [Render seams](../../explanations/app-lifecycle-and-window/the-submit-and-rendergraph-seams/) — adding a whole new pass from a layer.
