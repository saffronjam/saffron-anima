// Lit Wireframe overlay: re-draws the scene geometry as edges (PolygonMode::LINE) in a flat
// color over the already-shaded, tonemapped scene. Instanced like the depth prepass (set 2 =
// per-instance data); the push constant is the camera viewProj. Depth-tested (LESS_OR_EQUAL,
// no write) against the persisted 1× scene depth so hidden edges are occluded.

struct Instance
{
    float4x4 model;
    float4x4 normalMatrix;
    float4x4 prevModel;
    float4 baseColor;
    uint4 texture;
    float4 pbr;
    float4 emissive;
};
[[vk::binding(0, 2)]] StructuredBuffer<Instance> instances;

struct Push
{
    float4x4 viewProj;  // world -> clip
};
[[vk::push_constant]] Push pc;

struct VertexInput
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv0;
};

[shader("vertex")]
float4 vertexMain(VertexInput input, uint instanceIndex : SV_VulkanInstanceID) : SV_Position
{
    Instance inst = instances[instanceIndex];
    float4 worldPos = mul(inst.model, float4(input.position, 1.0));
    return mul(pc.viewProj, worldPos);
}

[shader("fragment")]
float4 fragmentMain() : SV_Target
{
    return float4(0.0, 0.85, 0.35, 1.0);  // a bright green edge color
}
