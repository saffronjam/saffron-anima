// The split-sum BRDF integration LUT: x = N·V, y = roughness -> (scale, bias) for the
// environment specular term. Environment-independent. Written into an rgba16f 2D storage
// image (only RG used). Set 0 binding 0.

static const float PI = 3.14159265359;

[[vk::image_format("rgba16f")]]
[[vk::binding(0, 0)]] RWTexture2D<float4> outLut;

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

// Smith geometry term for IBL (k = a^2 / 2).
float geometrySchlickGGX(float ndotv, float roughness)
{
    float a = roughness * roughness;
    float k = a * a / 2.0;
    return ndotv / (ndotv * (1.0 - k) + k);
}

float2 integrateBRDF(float ndotv, float roughness)
{
    float3 v = float3(sqrt(1.0 - ndotv * ndotv), 0.0, ndotv);
    float3 n = float3(0.0, 0.0, 1.0);
    float a = 0.0;
    float b = 0.0;
    const uint sampleCount = 512;
    for (uint i = 0; i < sampleCount; i = i + 1)
    {
        float2 xi = hammersley(i, sampleCount);
        float3 h = importanceSampleGGX(xi, n, roughness);
        float3 l = normalize(2.0 * dot(v, h) * h - v);
        float ndotl = max(l.z, 0.0);
        float ndoth = max(h.z, 0.0);
        float vdoth = max(dot(v, h), 0.0);
        if (ndotl > 0.0)
        {
            float g = geometrySchlickGGX(ndotl, roughness) * geometrySchlickGGX(ndotv, roughness);
            float gVis = g * vdoth / (ndoth * ndotv);
            float fc = pow(1.0 - vdoth, 5.0);
            a += (1.0 - fc) * gVis;
            b += fc * gVis;
        }
    }
    return float2(a, b) / float(sampleCount);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint width;
    uint height;
    outLut.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height)
    {
        return;
    }
    float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    float ndotv = max(uv.x, 1e-3);
    float roughness = max(uv.y, 1e-3);
    outLut[tid.xy] = float4(integrateBRDF(ndotv, roughness), 0.0, 1.0);
}
