// ReSTIR DI — initial candidate sampling. Per pixel: reconstruct the world surface from the
// G-buffer, draw K candidate lights from this pixel's froxel cluster list (reuse the
// clustered-forward candidate set), RIS-weight each by its unshadowed contribution, and keep
// one in the reservoir (weighted reservoir sampling). No shadow ray here — visibility is
// resolved at shade time. Writes the per-pixel reservoir.
//
// Reservoir layout (4x float4 = 64B, std430): see Reservoir struct. Set 0:
// b0 = G-buffer (view normal.xyz, viewZ.a) sampler, b1 = punctual light SSBO,
// b2 = cluster light-list SSBO, b3 = reservoir out SSBO. Push = camera + grid + counts.

static const uint MAX_LIGHTS_PER_CLUSTER = 64;

struct GpuLight
{
    float4 positionRange;
    float4 colorIntensity;
    float4 directionType;
    float4 spotCos;
};
struct Cluster
{
    uint count;
    uint indices[MAX_LIGHTS_PER_CLUSTER];
};
// One reservoir (RIS over lights). Packed into 2x float4 (32B).
struct Reservoir
{
    float4 a;  // x = chosen light index (as float, -1 = none), y = W (unbiased weight),
               // z = wSum (running), w = M (sample count)
    float4 b;  // reserved (target pdf etc.)
};

[[vk::binding(0, 0)]] Sampler2D gbuffer;
[[vk::binding(1, 0)]] StructuredBuffer<GpuLight> lights;
[[vk::binding(2, 0)]] StructuredBuffer<Cluster> clusters;
[[vk::binding(3, 0)]] RWStructuredBuffer<Reservoir> reservoirs;

struct Push
{
    float4x4 invView;       // view -> world (reconstruct world pos/normal from G-buffer)
    float4x4 invProjection; // clip -> view
    uint4 gridSize;         // xyz = froxel grid dims, w = punctual light count
    uint4 screenSize;       // xy = pixels, z = candidate count K, w = frame index (RNG seed)
    float4 zPlanes;         // x = near, y = far
};
[[vk::push_constant]] Push pc;

// Cheap hash RNG.
uint hashU(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
float rng(inout uint s) { s = hashU(s); return float(s) * 2.3283064365386963e-10; }

float3 viewPos(float2 uv, float viewZ)
{
    float2 ndc = uv * 2.0 - 1.0;
    float4 r = mul(pc.invProjection, float4(ndc, 1.0, 1.0));
    float3 ray = r.xyz / r.w;
    return ray * (viewZ / ray.z);
}

uint clusterIndexFor(float2 pixel, float viewZ)
{
    float near = pc.zPlanes.x;
    float far = pc.zPlanes.y;
    float depth = max(-viewZ, near);
    uint zSlice = uint(log(depth / near) / log(far / near) * float(pc.gridSize.z));
    zSlice = min(zSlice, pc.gridSize.z - 1);
    float2 tileSize = float2(pc.screenSize.xy) / float2(pc.gridSize.xy);
    uint tileX = min(uint(pixel.x / tileSize.x), pc.gridSize.x - 1);
    uint tileY = min(uint(pixel.y / tileSize.y), pc.gridSize.y - 1);
    return tileX + tileY * pc.gridSize.x + zSlice * pc.gridSize.x * pc.gridSize.y;
}

// Unshadowed scalar contribution of light `li` at the world surface (target function p-hat).
float targetContribution(uint li, float3 worldPos, float3 n)
{
    GpuLight lt = lights[li];
    float3 toLight = lt.positionRange.xyz - worldPos;
    float dist = length(toLight);
    if (dist > lt.positionRange.w) { return 0.0; }
    float3 l = toLight / max(dist, 1e-4);
    float ndotl = max(dot(n, l), 0.0);
    float invSq = 1.0 / max(dist * dist, 1e-4);
    float t = saturate(1.0 - pow(dist / lt.positionRange.w, 4.0));
    float atten = invSq * t * t;
    float cone = 1.0;
    if (lt.directionType.w > 0.5)
    {
        float3 sd = normalize(lt.directionType.xyz);
        cone = smoothstep(lt.spotCos.y, lt.spotCos.x, dot(sd, -l));
    }
    float intensity = lt.colorIntensity.a * dot(lt.colorIntensity.rgb, float3(0.299, 0.587, 0.114));
    return intensity * ndotl * atten * cone;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.screenSize.x || tid.y >= pc.screenSize.y) { return; }
    uint pixelIdx = tid.y * pc.screenSize.x + tid.x;
    float2 uv = (float2(tid.xy) + 0.5) / float2(pc.screenSize.xy);
    float4 g = gbuffer.SampleLevel(uv, 0.0);

    Reservoir res;
    res.a = float4(-1.0, 0.0, 0.0, 0.0);
    res.b = float4(0.0);
    if (g.a > -1e-4)  // background: empty reservoir
    {
        reservoirs[pixelIdx] = res;
        return;
    }
    float3 vp = viewPos(uv, g.a);
    float3 worldPos = mul(pc.invView, float4(vp, 1.0)).xyz;
    float3 vn = normalize(g.xyz);
    float3 n = normalize(mul((float3x3)pc.invView, vn));

    uint lightCount = pc.gridSize.w;
    if (lightCount == 0) { reservoirs[pixelIdx] = res; return; }
    uint cluster = clusterIndexFor(float2(tid.xy), g.a);
    uint clusterCount = clusters[cluster].count;
    uint poolCount = clusterCount > 0 ? clusterCount : lightCount;  // fall back to all lights

    uint seed = hashU(pixelIdx ^ (pc.screenSize.w * 0x9e3779b9u));
    uint K = max(pc.screenSize.z, 1u);
    float wSum = 0.0;
    float chosen = -1.0;
    float chosenTarget = 0.0;
    for (uint i = 0; i < K; i = i + 1)
    {
        // Pick a candidate light index from the cluster pool (uniform source pdf).
        uint slot = uint(rng(seed) * float(poolCount)) % poolCount;
        uint li = clusterCount > 0 ? clusters[cluster].indices[slot] : slot;
        float pHat = targetContribution(li, worldPos, n);
        float sourcePdf = 1.0 / float(poolCount);
        float w = sourcePdf > 0.0 ? pHat / sourcePdf : 0.0;
        wSum += w;
        if (wSum > 0.0 && rng(seed) < w / wSum)
        {
            chosen = float(li);
            chosenTarget = pHat;
        }
    }
    float W = (chosenTarget > 0.0) ? (wSum / float(K)) / chosenTarget : 0.0;
    res.a = float4(chosen, W, wSum, float(K));
    res.b = float4(chosenTarget, 0.0, 0.0, 0.0);
    reservoirs[pixelIdx] = res;
}
