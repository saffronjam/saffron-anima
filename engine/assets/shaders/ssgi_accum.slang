// Temporal accumulation of the (spatially denoised) SSGI radiance. Reprojects the previous
// frame's SSGI history through the motion vector, neighborhood-clamps it to the current 3x3
// range (rejects ghosting / handles disocclusion), and exponentially blends with this
// frame. Writes the result to BOTH the resolved map the mesh samples and the next-frame
// history. Runs only when TAA is on (it shares the motion target + history parity). Set 0:
// 0 current (sampler), 1 history (sampler), 2 motion (sampler), 3 resolved out (storage),
// 4 history-write out (storage).

[[vk::binding(0, 0)]] Sampler2D current;
[[vk::binding(1, 0)]] Sampler2D history;
[[vk::binding(2, 0)]] Sampler2D motion;
[[vk::image_format("rgba16f")]]
[[vk::binding(3, 0)]] RWTexture2D<float4> outResolved;
[[vk::image_format("rgba16f")]]
[[vk::binding(4, 0)]] RWTexture2D<float4> outHistory;

struct Push
{
    float4 params;  // x = history weight (0..1), y = 1 if history is valid this frame
};
[[vk::push_constant]] Push push;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint width;
    uint height;
    outResolved.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height)
    {
        return;
    }
    float2 texel = 1.0 / float2(width, height);
    float2 uv = (float2(tid.xy) + 0.5) * texel;

    float4 curSample = current.SampleLevel(uv, 0.0);
    float3 cur = curSample.rgb;
    float curZ = curSample.a; // this frame's view-Z, carried in alpha by ssgi-blur

    // 3x3 neighborhood min/max of the current frame — the AABB history is clamped to.
    float3 nmin = cur;
    float3 nmax = cur;
    for (int y = -1; y <= 1; y = y + 1)
    {
        for (int x = -1; x <= 1; x = x + 1)
        {
            float3 c = current.SampleLevel(uv + float2(x, y) * texel, 0.0).rgb;
            nmin = min(nmin, c);
            nmax = max(nmax, c);
        }
    }

    float2 mv = motion.SampleLevel(uv, 0.0).xy;
    float2 histUv = uv + mv;
    float weight = push.params.x;
    // Disocclusion / first frame: reprojected outside the screen -> no valid history.
    if (push.params.y < 0.5 || histUv.x < 0.0 || histUv.x > 1.0 || histUv.y < 0.0 || histUv.y > 1.0)
    {
        weight = 0.0;
    }
    float4 hist4 = history.SampleLevel(histUv, 0.0);
    // Per-pixel disocclusion: the reprojected history carries the view-Z it was written at; if that
    // diverges from this pixel's view-Z the motion vector landed on a different surface (a newly
    // revealed edge), so reset rather than smear the occluder's indirect light across the seam. The
    // 3x3 neighborhood clamp below only bounds magnitude — this rejects the wrong surface outright.
    float histZ = hist4.a;
    if (weight > 0.0 && abs(curZ - histZ) > 0.1 * max(abs(curZ), 0.01))
    {
        weight = 0.0;
    }
    float3 hist = clamp(hist4.rgb, nmin, nmax); // neighborhood clamp rejects residual ghosting

    float3 result = lerp(cur, hist, weight);
    // Propagate this frame's view-Z so next frame's reprojection can run the same test.
    outResolved[tid.xy] = float4(result, curZ);
    outHistory[tid.xy] = float4(result, curZ);
}
