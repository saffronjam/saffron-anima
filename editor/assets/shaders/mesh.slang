// Mesh shader: vertex inputs come from an interleaved buffer (position/normal/uv0)
// and the model-view-projection is a push constant. The fragment colors by the
// object-space normal, which renders a cube's six faces as six distinct colors.

struct VertexInput
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
};

struct PushConstants
{
    float4x4 mvp;
};
[[vk::push_constant]] PushConstants pc;

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    output.position = mul(pc.mvp, float4(input.position, 1.0));
    output.normal = input.normal;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    float3 n = normalize(input.normal);
    return float4(n * 0.5 + 0.5, 1.0);
}
