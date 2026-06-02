// Point-light shadow: renders the scene's WORLD-SPACE DISTANCE from the light into one
// face of a color cubemap (6 passes, one per face). The mesh fragment then samples the
// cube by the light->fragment direction and compares linear distance. Instanced like the
// mesh/depth-prepass (set 2 = per-instance data); the push constant carries this face's
// viewProj + the light world position.

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

struct Push
{
    float4x4 viewProj;  // this cube face's world -> clip
    float4 lightPos;    // xyz = light world position
};
[[vk::push_constant]] Push pc;

struct VertexInput
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 worldPos : POSITION0;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint instanceIndex : SV_VulkanInstanceID)
{
    Instance inst = instances[instanceIndex];
    VertexOutput output;
    float4 worldPos = mul(inst.model, float4(input.position, 1.0));
    output.worldPos = worldPos.xyz;
    output.position = mul(pc.viewProj, worldPos);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    float dist = length(input.worldPos - pc.lightPos.xyz);
    return float4(dist, 0.0, 0.0, 1.0);
}
