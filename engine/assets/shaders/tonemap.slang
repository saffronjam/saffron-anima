// The mandatory HDR->display tonemap on the offscreen color (an rgba16f storage
// image). One invocation per pixel: read the linear HDR radiance, apply exposure, a
// Reinhard tonemap + gamma 2.2, write it back. The render graph transitions the
// offscreen Color -> General before this pass and General -> ShaderReadOnly after
// (for the present blit to sample). set 0 binding 0; the exposure multiplier is a push constant.

[[vk::image_format("rgba16f")]]
[[vk::binding(0, 0)]] RWTexture2D<float4> target;

struct Push
{
    float exposure;  // linear multiplier (exp2 of the EV stop)
};
[[vk::push_constant]] Push push;

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
    float3 hdr = color.rgb * push.exposure;
    float3 mapped = hdr / (hdr + 1.0);
    mapped = pow(mapped, float3(1.0 / 2.2));
    target[px] = float4(mapped, color.a);
}
