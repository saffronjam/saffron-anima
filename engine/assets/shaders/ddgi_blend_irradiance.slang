// Blend traced ray radiance into the irradiance atlas. One thread per INTERIOR octahedral
// texel of each probe tile: integrate all rays weighted by max(0, dot(texelDir, rayDir)),
// then temporally lerp into the previous atlas value (hysteresis). Set 0: b0 = ray image
// (sampler), b1 = irradiance atlas (rgba16f storage). Push = probe grid + tile + hysteresis.

[[vk::binding(0, 0)]] Sampler2D rayImage;
[[vk::image_format("rgba16f")]]
[[vk::binding(1, 0)]] RWTexture2D<float4> atlas;

struct Push
{
    uint4 probeCount;  // xyz = probes per axis, w = rays per probe
    uint4 tile;        // x = interior size, y = 1 if first frame (no history)
    float4 params;     // x = hysteresis (history weight)
};
[[vk::push_constant]] Push pc;

static const float PI = 3.14159265;

float3 octDecode(float2 uv)  // [0,1]^2 -> unit dir
{
    float2 e = uv * 2.0 - 1.0;
    float3 v = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0)
    {
        v.xy = (1.0 - abs(v.yx)) * float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(v);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint interior = pc.tile.x;
    uint full = interior + 2;
    uint tilesX = pc.probeCount.x * pc.probeCount.y;
    uint atlasW = tilesX * full;
    uint atlasH = pc.probeCount.z * full;
    if (tid.x >= atlasW || tid.y >= atlasH)
    {
        return;
    }
    // Which probe tile + the local texel within it; skip gutter texels (filled by border).
    uint2 local = uint2(tid.x % full, tid.y % full);
    if (local.x == 0 || local.y == 0 || local.x > interior || local.y > interior)
    {
        return;
    }
    uint tileX = tid.x / full;
    uint tileY = tid.y / full;
    uint probeIndex = (tileX % pc.probeCount.x) + (tileX / pc.probeCount.x) * pc.probeCount.x
                      + tileY * pc.probeCount.x * pc.probeCount.y;
    uint total = pc.probeCount.x * pc.probeCount.y * pc.probeCount.z;
    if (probeIndex >= total)
    {
        return;
    }

    float2 octUv = (float2(local) - 0.5) / float(interior);  // texel center -> [0,1]
    float3 texelDir = octDecode(octUv);

    float3 sum = float3(0.0);
    float weightSum = 0.0;
    uint rays = pc.probeCount.w;
    float2 rayImgSize = float2(rays, total);
    for (uint r = 0; r < rays; r = r + 1)
    {
        float2 ruv = (float2(r, probeIndex) + 0.5) / rayImgSize;
        float4 rd = rayImage.SampleLevel(ruv, 0.0);
        // Reconstruct the ray direction the same way the trace did (Fibonacci sphere).
        float phi = 2.0 * PI * frac(float(r) * 0.618033988749895);  // rotation folded out: weight only needs relative dirs
        float cosT = 1.0 - (2.0 * float(r) + 1.0) / float(rays);
        float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
        float3 rayDir = float3(cos(phi) * sinT, sin(phi) * sinT, cosT);
        float w = max(0.0, dot(texelDir, rayDir));
        sum += rd.rgb * w;
        weightSum += w;
    }
    float3 irradiance = weightSum > 0.0 ? sum / weightSum : float3(0.0);

    uint2 px = uint2(tid.xy);
    float3 prev = atlas[px].rgb;
    float hysteresis = pc.tile.y != 0 ? 0.0 : pc.params.x;  // first frame: no history
    float3 blended = lerp(irradiance, prev, hysteresis);
    atlas[px] = float4(blended, 1.0);
}
