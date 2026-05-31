// FXAA 3 (console variant) as a compute post-process. Reads the scene result from a
// sampled source image and writes the anti-aliased result to the offscreen storage
// image. Set 0: binding 0 = source (combined image sampler), binding 1 = target (rgba8).
// Luma-based edge detect + a directional blend along the edge.

static const float EDGE_THRESHOLD_MIN = 0.0312;
static const float EDGE_THRESHOLD_MAX = 0.125;
static const float REDUCE_MIN = 1.0 / 128.0;
static const float REDUCE_MUL = 1.0 / 8.0;
static const float SPAN_MAX = 8.0;

[[vk::binding(0, 0)]] Sampler2D source;
[[vk::image_format("rgba8")]]
[[vk::binding(1, 0)]] RWTexture2D<float4> target;

float luma(float3 c)
{
    return dot(c, float3(0.299, 0.587, 0.114));
}

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

    float2 texel = 1.0 / float2(width, height);
    float2 uv = (float2(tid.xy) + 0.5) * texel;

    float3 rgbM = source.SampleLevel(uv, 0).rgb;
    float lumaM = luma(rgbM);
    float lumaNW = luma(source.SampleLevel(uv + float2(-1, -1) * texel, 0).rgb);
    float lumaNE = luma(source.SampleLevel(uv + float2(1, -1) * texel, 0).rgb);
    float lumaSW = luma(source.SampleLevel(uv + float2(-1, 1) * texel, 0).rgb);
    float lumaSE = luma(source.SampleLevel(uv + float2(1, 1) * texel, 0).rgb);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    float range = lumaMax - lumaMin;

    // Flat region: keep the original (no blur).
    if (range < max(EDGE_THRESHOLD_MIN, lumaMax * EDGE_THRESHOLD_MAX))
    {
        target[tid.xy] = float4(rgbM, 1.0);
        return;
    }

    // Edge direction from the diagonal luma gradient.
    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y = ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25 * REDUCE_MUL, REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -SPAN_MAX, SPAN_MAX) * texel;

    float3 rgbA = 0.5 * (source.SampleLevel(uv + dir * (1.0 / 3.0 - 0.5), 0).rgb +
                         source.SampleLevel(uv + dir * (2.0 / 3.0 - 0.5), 0).rgb);
    float3 rgbB = rgbA * 0.5 + 0.25 * (source.SampleLevel(uv + dir * -0.5, 0).rgb +
                                       source.SampleLevel(uv + dir * 0.5, 0).rgb);
    float lumaB = luma(rgbB);

    // The wider 4-tap blend can overshoot the edge's luma range; fall back to the 2-tap.
    float3 result = rgbB;
    if (lumaB < lumaMin || lumaB > lumaMax)
    {
        result = rgbA;
    }
    target[tid.xy] = float4(result, 1.0);
}
