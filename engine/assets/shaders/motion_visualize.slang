// Motion-vector visualization: read the rg16f motion target (per-pixel prevUv - curUv) and
// overwrite the offscreen color with a colorized result — direction maps to hue, magnitude to
// intensity. A compute pass run after tonemap when the active view mode is MotionVectors and
// the motion target exists (TAA or SSGI on). Binding shape matches the copy_color set
// (compute2: one sampler + one storage image).

[[vk::binding(0, 0)]] Sampler2D motionMap;
[[vk::binding(1, 0)]] RWTexture2D<float4> target;

struct Push
{
    uint2 size;  // viewport pixels
};
[[vk::push_constant]] Push pc;

static const float PI = 3.14159265;

float3 hsv2rgb(float3 c)
{
    float3 p = abs(frac(c.xxx + float3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return c.z * lerp(float3(1.0), saturate(p - 1.0), c.y);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.size.x || tid.y >= pc.size.y)
    {
        return;
    }
    float2 uv = (float2(tid.xy) + 0.5) / float2(pc.size);
    float2 m = motionMap.SampleLevel(uv, 0.0).rg;
    float mag = length(m);
    float angle = atan2(m.y, m.x) / (2.0 * PI) + 0.5;  // [0,1]
    float intensity = saturate(mag * 40.0);            // exaggerate sub-pixel motion
    float3 col = hsv2rgb(float3(angle, 1.0, intensity));
    target[tid.xy] = float4(col, 1.0);
}
