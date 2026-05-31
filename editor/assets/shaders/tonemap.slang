// In-place post-process on the offscreen color (bound as an rgba8 storage image).
// One invocation per pixel: read the lit color, apply a Reinhard tonemap + gamma 2.2,
// write it back. The render graph transitions the offscreen Color -> General before
// this pass and General -> ShaderReadOnly after (for ImGui to sample). set 0 binding 0.

[[vk::image_format("rgba8")]]
[[vk::binding(0, 0)]] RWTexture2D<float4> target;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint width;
    uint height;
    target.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height)
    {
        return;
    }

    uint2 px = tid.xy;
    float4 color = target[px];
    float3 mapped = color.rgb / (color.rgb + 1.0);
    mapped = pow(mapped, float3(1.0 / 2.2));
    target[px] = float4(mapped, color.a);
}
