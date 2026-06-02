// Per-pixel screen-space motion vectors for TAA (camera motion; geometry assumed static
// — per-instance previous-model tracking is a later add). Renders the instanced scene
// depth-tested; the fragment outputs (prevUV - curUV), i.e. the offset from this pixel's
// current UV to where the same surface point was last frame. RG16F. Set 2 = instances;
// push = current + previous camera viewProj.

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
    float4x4 curViewProj;
    float4x4 prevViewProj;
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
    float4 curClip : TEXCOORD0;
    float4 prevClip : TEXCOORD1;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint instanceIndex : SV_VulkanInstanceID)
{
    Instance inst = instances[instanceIndex];
    VertexOutput output;
    float4 worldPos = mul(inst.model, float4(input.position, 1.0));
    output.position = mul(pc.curViewProj, worldPos);
    output.curClip = output.position;
    output.prevClip = mul(pc.prevViewProj, worldPos);
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    float2 curNdc = input.curClip.xy / input.curClip.w;
    float2 prevNdc = input.prevClip.xy / input.prevClip.w;
    // NDC -> UV is *0.5 (+0.5); the offset is the NDC delta scaled by 0.5. Both clips use
    // the same Y-flipped projection, so the Y sign is consistent with the sampled images.
    float2 motionUv = (prevNdc - curNdc) * 0.5;
    return float4(motionUv, 0.0, 0.0);
}
