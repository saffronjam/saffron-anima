// Captures the scene's resolved linear-HDR color into the persistent previous-frame color
// image, for next frame's SSGI to gather as incoming radiance. Runs after the scene + AA
// resolve, before the in-place tonemap (so it stores LINEAR HDR, not display-referred).
// Set 0: 0 = scene color sampler, 1 = prev-color storage (rgba16f).

[[vk::binding(0, 0)]] Sampler2D sceneColor;
[[vk::image_format("rgba16f")]]
[[vk::binding(1, 0)]] RWTexture2D<float4> prevColorOut;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint width;
    uint height;
    prevColorOut.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height)
    {
        return;
    }
    float2 uv = (float2(tid.xy) + 0.5) / float2(width, height);
    prevColorOut[tid.xy] = float4(sceneColor.SampleLevel(uv, 0.0).rgb, 1.0);
}
