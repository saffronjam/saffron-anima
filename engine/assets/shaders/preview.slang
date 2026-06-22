// Material preview shader: renders a sphere with a candidate material under fixed studio lighting,
// for the editor's material preview pane and cached asset thumbnails. Self-contained — only the
// bindless texture array (set 0, shared with the übershader) + a push constant; no scene / light /
// IBL / GI sets. This is a controlled studio look (a key directional light + hemisphere ambient),
// not the full forward+ übershader — matching how DCC tools preview materials. The view direction
// is a constant that matches the camera framing in renderMaterialPreview.

struct PreviewPush
{
    float4x4 viewProj;
    float4 baseColor;
    uint4 tex;   // x = albedo, y = metallic-roughness, z = normal, w = feature bits
    float4 pbr;  // x = metallic, y = roughness, z = normalStrength
};
[[vk::push_constant]] PreviewPush push;

[[vk::binding(0, 0)]] Sampler2D textures[1024];

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
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertexMain(VertexInput input)
{
    VertexOutput output;
    output.position = mul(push.viewProj, float4(input.position, 1.0));
    output.worldPos = input.position;
    output.normal = input.normal;
    output.uv = input.uv0;
    return output;
}

static const uint FEATURE_NORMAL = 1u;
static const float PI = 3.14159265;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    float2 uv = input.uv;
    float4 base = textures[NonUniformResourceIndex(push.tex.x)].Sample(uv);
    float3 albedo = base.rgb * push.baseColor.rgb;
    float4 mr = textures[NonUniformResourceIndex(push.tex.y)].Sample(uv);
    float metallic = saturate(push.pbr.x * mr.b);
    float roughness = clamp(push.pbr.y * mr.g, 0.045, 1.0);

    float3 n = normalize(input.normal);
    if ((push.tex.w & FEATURE_NORMAL) != 0u)
    {
        float3 dp1 = ddx(input.worldPos);
        float3 dp2 = ddy(input.worldPos);
        float2 du1 = ddx(uv);
        float2 du2 = ddy(uv);
        float3 dp2p = cross(dp2, n);
        float3 dp1p = cross(n, dp1);
        float3 T = dp2p * du1.x + dp1p * du2.x;
        float3 B = dp2p * du1.y + dp1p * du2.y;
        float inv = rsqrt(max(dot(T, T), dot(B, B)));
        float3x3 tbn = float3x3(T * inv, B * inv, n);
        float3 mapN = textures[NonUniformResourceIndex(push.tex.z)].Sample(uv).xyz * 2.0 - 1.0;
        mapN.xy *= push.pbr.z;
        n = normalize(mul(mapN, tbn));
    }

    // Fixed studio lighting: a key directional light + hemisphere ambient. V matches the camera dir.
    float3 V = normalize(float3(0.3, 0.4, 1.0));
    float3 L = normalize(float3(0.5, 0.6, 0.6));
    float3 H = normalize(L + V);
    float ndotl = max(dot(n, L), 0.0);
    float ndotv = max(dot(n, V), 1e-3);
    float ndoth = max(dot(n, H), 0.0);
    float hdotv = max(dot(H, V), 0.0);

    float3 F0 = lerp(float3(0.04), albedo, metallic);
    float a = roughness * roughness;
    float a2 = a * a;
    float d = ndoth * ndoth * (a2 - 1.0) + 1.0;
    float D = a2 / max(PI * d * d, 1e-5);
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float G = (ndotv / (ndotv * (1.0 - k) + k)) * (ndotl / (ndotl * (1.0 - k) + k));
    float3 F = F0 + (1.0 - F0) * pow(1.0 - hdotv, 5.0);
    float3 spec = D * G * F / max(4.0 * ndotv * ndotl, 1e-4);
    float3 kd = (1.0 - F) * (1.0 - metallic);
    float3 lit = (kd * albedo / PI + spec) * ndotl * 2.5;

    float hemi = 0.5 + 0.5 * n.y;
    float3 ambient = albedo * (1.0 - metallic) * lerp(float3(0.08, 0.08, 0.10), float3(0.30, 0.32, 0.38), hemi);

    float3 color = lit + ambient;
    color = color / (color + 1.0);  // Reinhard tonemap into [0,1]
    return float4(color, 1.0);
}
