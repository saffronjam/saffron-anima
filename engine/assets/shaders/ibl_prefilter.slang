// Prefiltered specular environment: GGX importance-sample the environment for a given
// roughness (one dispatch per mip; the push constant carries that mip's roughness).
// Set 0: binding 0 = environment cube (sampler), binding 1 = this mip's cube storage view.

static const float PI = 3.14159265359;

[[vk::binding(0, 0)]] SamplerCube envCube;
[[vk::image_format("rgba16f")]]
[[vk::binding(1, 0)]] RWTexture2DArray<float4> outCube;

struct Push
{
    float roughness;
};
[[vk::push_constant]] Push push;

float3 cubeFaceDir(uint face, float2 uv)
{
    float3 d;
    if (face == 0)      { d = float3( 1.0, -uv.y, -uv.x); }
    else if (face == 1) { d = float3(-1.0, -uv.y,  uv.x); }
    else if (face == 2) { d = float3( uv.x,  1.0,  uv.y); }
    else if (face == 3) { d = float3( uv.x, -1.0, -uv.y); }
    else if (face == 4) { d = float3( uv.x, -uv.y,  1.0); }
    else                { d = float3(-uv.x, -uv.y, -1.0); }
    return normalize(d);
}

float radicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 hammersley(uint i, uint n)
{
    return float2(float(i) / float(n), radicalInverseVdC(i));
}

float3 importanceSampleGGX(float2 xi, float3 n, float roughness)
{
    float a = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cosT = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinT = sqrt(1.0 - cosT * cosT);
    float3 h = float3(cos(phi) * sinT, sin(phi) * sinT, cosT);
    float3 up = abs(n.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tx = normalize(cross(up, n));
    float3 ty = cross(n, tx);
    return normalize(tx * h.x + ty * h.y + n * h.z);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint width;
    uint height;
    uint layers;
    outCube.GetDimensions(width, height, layers);
    if (tid.x >= width || tid.y >= height || tid.z >= layers)
    {
        return;
    }
    float2 uv = (float2(tid.xy) + 0.5) / float2(width, height) * 2.0 - 1.0;
    float3 n = cubeFaceDir(tid.z, uv);
    float3 r = n;
    float3 v = n;  // split-sum approximation: view = normal = reflection

    const uint sampleCount = 64;
    float3 prefiltered = float3(0.0);
    float totalWeight = 0.0;
    for (uint i = 0; i < sampleCount; i = i + 1)
    {
        float2 xi = hammersley(i, sampleCount);
        float3 h = importanceSampleGGX(xi, n, push.roughness);
        float3 l = normalize(2.0 * dot(v, h) * h - v);
        float ndotl = max(dot(n, l), 0.0);
        if (ndotl > 0.0)
        {
            prefiltered += envCube.SampleLevel(l, 0.0).rgb * ndotl;
            totalWeight += ndotl;
        }
    }
    prefiltered = totalWeight > 0.0 ? prefiltered / totalWeight : envCube.SampleLevel(n, 0.0).rgb;
    outCube[tid] = float4(prefiltered, 1.0);
}
