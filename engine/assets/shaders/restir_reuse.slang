// ReSTIR DI — temporal + spatial reuse. Combines this pixel's initial reservoir with the
// previous frame's reservoir (reprojected via the motion vector) and a few spatial neighbors
// (depth/normal-similar), via weighted reservoir combination with M-clamping for bias
// control. Reads the initial + previous reservoirs, writes the combined reservoir.
// Set 0: b0 = G-buffer sampler, b1 = motion sampler, b2 = initial reservoirs (in),
// b3 = previous reservoirs (in), b4 = punctual light SSBO, b5 = combined reservoirs (out).

static const uint MAX_LIGHTS_PER_CLUSTER = 64;

struct GpuLight
{
    float4 positionRange;
    float4 colorIntensity;
    float4 directionType;
    float4 spotCos;
};
struct Reservoir { float4 a; float4 b; };

[[vk::binding(0, 0)]] Sampler2D gbuffer;
[[vk::binding(1, 0)]] Sampler2D motion;
[[vk::binding(2, 0)]] StructuredBuffer<Reservoir> initial;
[[vk::binding(3, 0)]] StructuredBuffer<Reservoir> previous;
[[vk::binding(4, 0)]] StructuredBuffer<GpuLight> lights;
[[vk::binding(5, 0)]] RWStructuredBuffer<Reservoir> combined;

struct Push
{
    float4x4 invView;
    float4x4 invProjection;
    uint4 screenSize;   // xy = pixels, z = max M (clamp), w = frame index + validity (bit0)
    float4 params;      // x = spatial radius (px), y = 1 if temporal history valid
};
[[vk::push_constant]] Push pc;

uint hashU(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
float rng(inout uint s) { s = hashU(s); return float(s) * 2.3283064365386963e-10; }

float3 viewPos(float2 uv, float viewZ)
{
    float2 ndc = uv * 2.0 - 1.0;
    float4 r = mul(pc.invProjection, float4(ndc, 1.0, 1.0));
    float3 ray = r.xyz / r.w;
    return ray * (viewZ / ray.z);
}

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

// Merge reservoir `other` (with its own target pHat at THIS pixel) into `res` via WRS.
void combineInto(inout Reservoir res, inout float wSum, inout uint seed, float otherChosen, float otherW, float otherM, float pHat)
{
    float w = pHat * otherW * otherM;
    wSum += w;
    res.a.w += otherM;  // accumulate M
    if (otherChosen >= 0.0 && wSum > 0.0 && rng(seed) < w / wSum)
    {
        res.a.x = otherChosen;
        res.b.x = pHat;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.screenSize.x || tid.y >= pc.screenSize.y) { return; }
    uint pixelIdx = tid.y * pc.screenSize.x + tid.x;
    float2 uv = (float2(tid.xy) + 0.5) / float2(pc.screenSize.xy);
    float4 g = gbuffer.SampleLevel(uv, 0.0);

    Reservoir cur = initial[pixelIdx];
    if (g.a > -1e-4)
    {
        combined[pixelIdx] = cur;
        return;
    }
    float3 vp = viewPos(uv, g.a);
    float3 worldPos = mul(pc.invView, float4(vp, 1.0)).xyz;
    float3 n = normalize(mul((float3x3)pc.invView, normalize(g.xyz)));

    // Start the combined reservoir from the current frame's initial reservoir.
    Reservoir res;
    res.a = float4(cur.a.x, 0.0, 0.0, cur.a.w);
    res.b = float4(cur.b.x, 0.0, 0.0, 0.0);
    uint seed = hashU(pixelIdx ^ (pc.screenSize.w * 0x85ebca6bu));
    float wSum = cur.b.x * cur.a.y * cur.a.w;  // pHat * W * M for the current reservoir

    float maxM = float(pc.screenSize.z);

    // Temporal: reproject + merge the previous frame's reservoir.
    if (pc.params.y > 0.5)
    {
        float2 mv = motion.SampleLevel(uv, 0.0).xy;
        float2 puv = uv + mv;
        if (puv.x >= 0.0 && puv.x <= 1.0 && puv.y >= 0.0 && puv.y <= 1.0)
        {
            uint2 ppx = uint2(puv * float2(pc.screenSize.xy));
            uint pIdx = ppx.y * pc.screenSize.x + ppx.x;
            Reservoir prev = previous[pIdx];
            if (prev.a.x >= 0.0)
            {
                float prevM = min(prev.a.w, maxM);  // clamp history to bound bias
                float pHat = targetContribution(uint(prev.a.x), worldPos, n);
                combineInto(res, wSum, seed, prev.a.x, prev.a.y, prevM, pHat);
            }
        }
    }

    // Spatial: a few neighbors at a small radius (depth-similar).
    const uint spatialCount = 4;
    float radius = pc.params.x;
    for (uint i = 0; i < spatialCount; i = i + 1)
    {
        float ang = rng(seed) * 6.2831853;
        float rad = rng(seed) * radius;
        int2 off = int2(cos(ang) * rad, sin(ang) * rad);
        int2 npx = int2(tid.xy) + off;
        if (npx.x < 0 || npx.y < 0 || npx.x >= int(pc.screenSize.x) || npx.y >= int(pc.screenSize.y)) { continue; }
        float2 nuv = (float2(npx) + 0.5) / float2(pc.screenSize.xy);
        float4 ng = gbuffer.SampleLevel(nuv, 0.0);
        if (ng.a > -1e-4) { continue; }
        if (abs(ng.a - g.a) > 0.5) { continue; }       // depth similarity
        if (dot(normalize(ng.xyz), normalize(g.xyz)) < 0.9) { continue; }  // normal similarity
        Reservoir nb = initial[npx.y * pc.screenSize.x + npx.x];
        if (nb.a.x < 0.0) { continue; }
        float nbM = min(nb.a.w, maxM);
        float pHat = targetContribution(uint(nb.a.x), worldPos, n);
        combineInto(res, wSum, seed, nb.a.x, nb.a.y, nbM, pHat);
    }

    // Finalize unbiased weight W = wSum / (M * pHat_chosen).
    float W = (res.b.x > 0.0 && res.a.w > 0.0) ? (wSum / res.a.w) / res.b.x : 0.0;
    res.a.y = W;
    res.a.z = wSum;
    combined[pixelIdx] = res;
}
