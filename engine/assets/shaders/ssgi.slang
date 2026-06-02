// Screen-space global illumination (one bounce). For each pixel, march a few cosine-
// weighted hemisphere rays in view space against the G-buffer depth; on a hit, gather the
// PREVIOUS frame's lit HDR color as incoming indirect radiance. Output an indirect-radiance
// map the mesh adds into its ambient term (modulated by albedo there). Temporally smoothed
// by reusing the previous frame's color (the TAA-era history). Set 0: 0 = G-buffer sampler
// (normal.xyz, viewZ.a), 1 = previous-frame color sampler, 2 = SSGI out (rgba16f).

[[vk::binding(0, 0)]] Sampler2D gbuffer;
[[vk::binding(1, 0)]] Sampler2D prevColor;
[[vk::image_format("rgba16f")]]
[[vk::binding(2, 0)]] RWTexture2D<float4> outGi;

struct Push
{
    float4x4 projection;     // view -> clip
    float4x4 invProjection;  // clip -> view
    float4 params;           // x = radius (view units), y = intensity, z = step count
};
[[vk::push_constant]] Push pc;

static const float PI = 3.14159265;

float3 viewPosFromUv(float2 uv, float viewZ)
{
    float2 ndc = uv * 2.0 - 1.0;
    float4 r = mul(pc.invProjection, float4(ndc, 1.0, 1.0));
    float3 ray = r.xyz / r.w;
    return ray * (viewZ / ray.z);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint width;
    uint height;
    outGi.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height)
    {
        return;
    }
    float2 texel = 1.0 / float2(width, height);
    float2 uv = (float2(tid.xy) + 0.5) * texel;
    float4 g = gbuffer.SampleLevel(uv, 0.0);
    float viewZ = g.a;
    if (viewZ > -1e-4)  // background contributes no indirect
    {
        outGi[tid.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    float3 p = viewPosFromUv(uv, viewZ);
    float3 n = normalize(g.xyz);

    // Build a tangent basis to orient the cosine hemisphere around the view-space normal.
    float3 up = abs(n.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 t = normalize(cross(up, n));
    float3 b = cross(n, t);

    float radius = pc.params.x;
    uint steps = uint(pc.params.z);
    float rnd = frac(sin(dot(float2(tid.xy), float2(12.9898, 78.233))) * 43758.5453);

    const uint rayCount = 4;
    float3 indirect = float3(0.0);
    for (uint r = 0; r < rayCount; r = r + 1)
    {
        // Cosine-weighted hemisphere sample (concentric-ish via the hash + golden angle).
        float u1 = frac(rnd + float(r) / float(rayCount));
        float u2 = frac(rnd * 1.618 + float(r) * 0.382);
        float r2 = sqrt(u1);
        float phi = 2.0 * PI * u2;
        float3 local = float3(r2 * cos(phi), r2 * sin(phi), sqrt(max(0.0, 1.0 - u1)));
        float3 dir = normalize(local.x * t + local.y * b + local.z * n);

        // March the ray; on the first step where it dips behind stored geometry, gather.
        for (uint s = 1; s <= steps; s = s + 1)
        {
            float3 sp = p + dir * radius * (float(s) / float(steps));
            float4 clip = mul(pc.projection, float4(sp, 1.0));
            if (clip.w <= 0.0) { break; }
            float2 suv = (clip.xy / clip.w) * 0.5 + 0.5;
            if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) { break; }
            float surfZ = gbuffer.SampleLevel(suv, 0.0).a;
            if (surfZ > -1e-4) { continue; }
            float diff = surfZ - sp.z;  // >0 when the stored surface is nearer than the ray
            if (diff > 0.02 && diff < radius * 0.5)
            {
                // Hit: the surface at suv faces back toward us -> its lit color bounces here.
                indirect += prevColor.SampleLevel(suv, 0.0).rgb;
                break;
            }
        }
    }
    indirect = indirect / float(rayCount) * pc.params.y;
    outGi[tid.xy] = float4(indirect, 1.0);
}
