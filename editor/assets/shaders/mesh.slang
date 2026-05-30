// Mesh shader: vertex inputs from an interleaved buffer (position/normal/uv0).
// Push constants carry the mvp, the model's upper-left 3x3 (as columns, for the
// world normal), and the base color factor. Set 0 is the albedo texture; set 1 is
// the directional light. Shading is base color * albedo * (ambient + N.L).

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
    float2 uv0 : TEXCOORD0;
};

struct PushConstants
{
    float4x4 mvp;
    float4 n0;  // columns of mat3(model)
    float4 n1;
    float4 n2;
    float4 baseColor;
};
[[vk::push_constant]] PushConstants pc;

[[vk::binding(0, 0)]] Sampler2D albedo;

struct Light
{
    float4 directionAmbient;  // xyz = travel direction, w = ambient
    float4 colorIntensity;    // rgb = color, a = intensity
};
[[vk::binding(0, 1)]] ConstantBuffer<Light> light;

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    output.position = mul(pc.mvp, float4(input.position, 1.0));
    output.worldNormal = pc.n0.xyz * input.normal.x + pc.n1.xyz * input.normal.y + pc.n2.xyz * input.normal.z;
    output.uv0 = input.uv0;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    float3 n = normalize(input.worldNormal);
    float3 lightDir = normalize(light.directionAmbient.xyz);
    float ndotl = max(dot(n, -lightDir), 0.0);
    float ambient = light.directionAmbient.w;
    float3 lit = light.colorIntensity.rgb * light.colorIntensity.a * ndotl + ambient;
    float4 tex = albedo.Sample(input.uv0);
    float3 color = tex.rgb * pc.baseColor.rgb * lit;
    return float4(color, tex.a * pc.baseColor.a);
}
