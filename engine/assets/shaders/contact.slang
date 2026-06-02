// Screen-space contact shadows for the directional light: from each pixel, march a short
// ray toward the sun in view space against the G-buffer depth; if it passes behind nearer
// geometry, the pixel is in a fine contact shadow. A cheap supplement to the phase-3 shadow
// map for small-scale contact detail. Output r8 occlusion (1 = lit). Set 0: 0 = G-buffer
// sampler, 1 = occlusion storage.

[[vk::binding(0, 0)]] Sampler2D gbuffer;
[[vk::image_format("r8")]]
[[vk::binding(1, 0)]] RWTexture2D<float> outShadow;

struct Push
{
    float4x4 projection;     // view -> clip (to project marched view positions to screen)
    float4x4 invProjection;  // clip -> view (reconstruct view position)
    float4 lightDirView;     // xyz = direction TO the light, in view space
    float4 params;           // x = ray length (view units), y = step count, z = thickness
};
[[vk::push_constant]] Push pc;

float3 viewPosFromUv(float2 uv, float viewZ)
{
    float2 ndc = uv * 2.0 - 1.0;
    float4 r = mul(pc.invProjection, float4(ndc, 1.0, 1.0));
    float3 ray = r.xyz / r.w;
    return ray * (viewZ / ray.z);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint width;
    uint height;
    outShadow.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height)
    {
        return;
    }
    float2 texel = 1.0 / float2(width, height);
    float2 uv = (float2(tid.xy) + 0.5) * texel;
    float4 g = gbuffer.SampleLevel(uv, 0.0);
    float viewZ = g.a;
    if (viewZ > -1e-4)  // background: lit
    {
        outShadow[tid.xy] = 1.0;
        return;
    }
    float3 p = viewPosFromUv(uv, viewZ);
    float3 n = normalize(g.xyz);
    float3 l = normalize(pc.lightDirView.xyz);

    float rayLen = pc.params.x;
    uint steps = uint(pc.params.y);
    float thickness = pc.params.z;
    // Offset off the surface along the normal to avoid self-occlusion.
    float3 origin = p + n * 0.02;

    float occluded = 0.0;
    for (uint i = 1; i <= steps; i = i + 1)
    {
        float3 s = origin + l * rayLen * (float(i) / float(steps));
        float4 clip = mul(pc.projection, float4(s, 1.0));
        if (clip.w <= 0.0)
        {
            break;
        }
        float2 suv = (clip.xy / clip.w) * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0)
        {
            break;
        }
        float surfZ = gbuffer.SampleLevel(suv, 0.0).a;
        if (surfZ > -1e-4)
        {
            continue;  // background at the sample
        }
        // The stored surface is in front of (nearer than) the ray sample by more than the
        // bias but within the thickness window -> a real occluder between p and the light.
        float diff = surfZ - s.z;  // >0 when surface is nearer (less negative) than the ray
        if (diff > 0.01 && diff < thickness)
        {
            occluded = 1.0;
            break;
        }
    }
    outShadow[tid.xy] = 1.0 - occluded;
}
