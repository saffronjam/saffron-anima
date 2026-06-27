// Depth-aware (bilateral) 5x5 blur of the raw SSGI radiance map — the spatial-denoise step
// (mirrors ao_blur for the rgba16f one-bounce signal). Weights neighbors by view-Z
// similarity (read from the G-buffer .a) so the blur does not bleed indirect light across
// depth discontinuities. Set 0: 0 = raw SSGI sampler, 1 = G-buffer sampler, 2 = denoised
// SSGI storage (rgba16f).

[[vk::binding(0, 0)]] Sampler2D ssgiRaw;
[[vk::binding(1, 0)]] Sampler2D gbuffer;
[[vk::image_format("rgba16f")]]
[[vk::binding(2, 0)]] RWTexture2D<float4> ssgiOut;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint width;
    uint height;
    ssgiOut.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height)
    {
        return;
    }
    float2 texel = 1.0 / float2(width, height);
    float2 uv = (float2(tid.xy) + 0.5) * texel;
    float centerZ = gbuffer.SampleLevel(uv, 0.0).a;

    float3 sum = float3(0.0);
    float weight = 0.0;
    for (int y = -2; y <= 2; y = y + 1)
    {
        for (int x = -2; x <= 2; x = x + 1)
        {
            float2 suv = uv + float2(x, y) * texel;
            float z = gbuffer.SampleLevel(suv, 0.0).a;
            // Gaussian-ish spatial weight * a depth-similarity weight.
            float w = exp(-float(x * x + y * y) / 4.0);
            w *= exp(-abs(z - centerZ) * 4.0);
            sum += ssgiRaw.SampleLevel(suv, 0.0).rgb * w;
            weight += w;
        }
    }
    float3 result = weight > 0.0 ? sum / weight : ssgiRaw.SampleLevel(uv, 0.0).rgb;
    // Carry this pixel's view-Z in the alpha channel (the mesh samples only .rgb). The temporal
    // accumulation reads it back through the motion vector for a per-pixel disocclusion test.
    ssgiOut[tid.xy] = float4(result, centerZ);
}
