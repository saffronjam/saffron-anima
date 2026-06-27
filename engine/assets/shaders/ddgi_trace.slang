// DDGI ray trace (software): one thread per (probe, ray). Marches a ray from the probe
// center through the voxel proxy; on a hit, the radiance is the hit voxel's albedo lit by a
// crude sun+sky term PLUS last-frame probe irradiance sampled at the hit (free multi-bounce);
// on a miss, the sky color. Writes radiance (rgb) + hit distance (a) into the ray image.
// Set 0: b0 = voxel proxy (sampler-as-storage read), b1 = prev irradiance atlas (sampler),
// b2 = ray image (rgba16f storage). Push = volume + probe grid + lighting + frame.

[[vk::image_format("rgba16f")]]
[[vk::binding(0, 0)]] RWTexture3D<float4> voxels;
[[vk::binding(1, 0)]] Sampler2D irradianceAtlas;
[[vk::image_format("rgba16f")]]
[[vk::binding(2, 0)]] RWTexture2D<float4> rayOut;

struct Push
{
    uint4 probeCount;     // xyz = probes per axis, w = rays per probe
    uint4 gridCount;      // xyz = voxel resolution, w = irradiance tile interior size
    float4 volumeMin;     // xyz = volume min corner
    float4 volumeExtent;  // xyz = volume size
    float4 sunDir;        // xyz = direction the sun travels, w = sun intensity
    float4 sunColor;      // rgb sun color, w = frame index (for ray rotation)
    float4 skyColor;      // rgb ambient sky radiance
};
[[vk::push_constant]] Push pc;

static const float PI = 3.14159265;

// Fibonacci-sphere direction i of n, rotated by a per-frame golden offset.
float3 sphericalFibonacci(uint i, uint n, float rot)
{
    float phi = 2.0 * PI * frac(float(i) * 0.618033988749895 + rot);
    float cosT = 1.0 - (2.0 * float(i) + 1.0) / float(n);
    float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
    return float3(cos(phi) * sinT, sin(phi) * sinT, cosT);
}

float3 probeWorldPos(uint3 p)
{
    float3 t = (float3(p) + 0.5) / float3(pc.probeCount.xyz);
    return pc.volumeMin.xyz + t * pc.volumeExtent.xyz;
}

// Octahedral encode a unit direction to [0,1]^2.
float2 octEncode(float3 d)
{
    d /= (abs(d.x) + abs(d.y) + abs(d.z));
    float2 o = d.z >= 0.0 ? d.xy : (1.0 - abs(d.yx)) * float2(d.x >= 0.0 ? 1.0 : -1.0, d.y >= 0.0 ? 1.0 : -1.0);
    return o * 0.5 + 0.5;
}

// Sample a probe's previous irradiance in direction `dir` from the atlas (with gutter).
float3 sampleProbeIrradiance(uint3 p, float3 dir)
{
    uint tile = pc.gridCount.w + 2;  // interior + 1-texel gutter each side
    uint tilesX = pc.probeCount.x * pc.probeCount.y;
    uint tileX = p.x + p.y * pc.probeCount.x;
    uint tileY = p.z;
    float2 oct = octEncode(normalize(dir));
    float2 interior = (oct * float(pc.gridCount.w) + 1.0);  // into [1, tile-1]
    float2 atlasPx = float2(tileX, tileY) * float(tile) + interior;
    uint atlasW = tilesX * tile;
    uint atlasH = pc.probeCount.z * tile;
    float2 uv = (atlasPx + 0.5) / float2(atlasW, atlasH);
    return irradianceAtlas.SampleLevel(uv, 0.0).rgb;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint ray = tid.x;
    uint total = pc.probeCount.x * pc.probeCount.y * pc.probeCount.z;
    // Round-robin probe budget: the dispatch covers only DDGI_PROBE_BUDGET probes, starting at the
    // offset in skyColor.w and wrapping, so the whole volume refreshes over several frames.
    uint probeIndex = (tid.y + uint(pc.skyColor.w)) % total;
    if (ray >= pc.probeCount.w)
    {
        return;
    }
    uint3 p;
    p.x = probeIndex % pc.probeCount.x;
    p.y = (probeIndex / pc.probeCount.x) % pc.probeCount.y;
    p.z = probeIndex / (pc.probeCount.x * pc.probeCount.y);

    float3 origin = probeWorldPos(p);
    float rot = frac(pc.sunColor.w * 0.618033988749895);
    float3 dir = sphericalFibonacci(ray, pc.probeCount.w, rot);

    // DDA-ish fixed-step march through the voxel volume.
    float3 voxelSize = pc.volumeExtent.xyz / float3(pc.gridCount.xyz);
    float stepLen = min(voxelSize.x, min(voxelSize.y, voxelSize.z)) * 0.5;
    float maxDist = length(pc.volumeExtent.xyz);
    float3 radiance = float3(0.0);
    float hitDist = maxDist;
    bool hit = false;
    float traveled = stepLen;
    for (uint s = 0; s < 256; s = s + 1)
    {
        if (traveled >= maxDist) { break; }
        float3 wp = origin + dir * traveled;
        float3 t = (wp - pc.volumeMin.xyz) / pc.volumeExtent.xyz;
        if (any(t < 0.0) || any(t > 1.0)) { break; }
        int3 vc = int3(t * float3(pc.gridCount.xyz));
        float4 voxel = voxels[vc];
        if (voxel.a > 0.5)
        {
            // Hit: crude lit color = albedo * (sky + sun) + last-frame bounce from this point.
            float3 lit = voxel.rgb * (pc.skyColor.rgb + pc.sunColor.rgb * pc.sunDir.w * 0.5);
            // Multi-bounce: gather this hit's nearest probe's prior irradiance in -dir.
            float3 bounce = sampleProbeIrradiance(p, dir) * voxel.rgb * 0.25;
            radiance = lit + bounce;
            hitDist = traveled;
            hit = true;
            break;
        }
        traveled += stepLen;
    }
    if (!hit)
    {
        radiance = pc.skyColor.rgb;  // ray escaped to sky
    }
    rayOut[uint2(ray, probeIndex)] = float4(radiance, hitDist);
}
