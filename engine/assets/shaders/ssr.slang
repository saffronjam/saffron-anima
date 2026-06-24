// Screen-space reflections. For each pixel, reflect the view ray about the G-buffer normal
// and march that mirror ray in view space against the stored depth; on a hit, sample the
// PREVIOUS frame's lit HDR color as the reflected radiance. Outputs a reflection map
// (rgb = radiance, a = hit confidence, edge-faded); the mesh shader blends it over the
// prefiltered-env specular weighted by (1 - roughness) so only smooth surfaces use it —
// rough pixels keep the cube fallback. The per-frame index (params.w) rotates the dither so
// TAA averages the march noise. Set 0: 0 = G-buffer sampler (normal.xyz, viewZ.a), 1 =
// previous-frame color sampler, 2 = SSR out (rgba16f).

[[vk::binding(0, 0)]] Sampler2D gbuffer;
[[vk::binding(1, 0)]] Sampler2D prevColor;
[[vk::image_format("rgba16f")]]
[[vk::binding(2, 0)]] RWTexture2D<float4> outSsr;

struct Push
{
    float4x4 projection;     // view -> clip
    float4x4 invProjection;  // clip -> view
    float4 params;           // x = max ray distance (view units), y = intensity, z = step count, w = frame index
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
    outSsr.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height)
    {
        return;
    }
    float2 texel = 1.0 / float2(width, height);
    float2 uv = (float2(tid.xy) + 0.5) * texel;
    float4 g = gbuffer.SampleLevel(uv, 0.0);
    float viewZ = g.a;
    if (viewZ > -1e-4)  // background reflects nothing in screen space
    {
        outSsr[tid.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    float3 p = viewPosFromUv(uv, viewZ);
    float3 n = normalize(g.xyz);
    float3 viewDir = normalize(p);            // camera (origin) -> surface
    float3 refl = reflect(viewDir, n);        // mirror reflection ray in view space

    float maxDist = pc.params.x;
    uint steps = max(uint(pc.params.z), 1u);
    // Interleaved gradient noise rotated by the frame index, so the per-pixel march start
    // jitters and TAA averages the stair-stepping instead of leaving a fixed seam.
    float ign = frac(52.9829189 * frac(0.06711056 * float(tid.x) + 0.00583715 * float(tid.y)));
    float jitter = frac(ign + pc.params.w * 0.6180339887);

    float stepLen = maxDist / float(steps);
    float3 hitColor = float3(0.0);
    float conf = 0.0;
    float2 hitUv = float2(0.0);
    for (uint s = 1; s <= steps; s = s + 1)
    {
        float3 sp = p + refl * stepLen * (float(s) + jitter);
        float4 clip = mul(pc.projection, float4(sp, 1.0));
        if (clip.w <= 0.0) { break; }
        float2 suv = (clip.xy / clip.w) * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) { break; }
        float surfZ = gbuffer.SampleLevel(suv, 0.0).a;
        if (surfZ > -1e-4) { continue; }       // ray passed over background
        float diff = surfZ - sp.z;             // >0 when the stored surface is nearer than the ray
        if (diff > 0.0 && diff < stepLen * 2.0)
        {
            hitUv = suv;
            conf = 1.0;
            break;
        }
    }
    if (conf > 0.0)
    {
        hitColor = prevColor.SampleLevel(hitUv, 0.0).rgb;
        // Fade confidence toward the screen edges so a reflection sliding off-screen
        // dissolves into the env fallback instead of snapping.
        float2 edge = smoothstep(0.0, 0.1, hitUv) * smoothstep(0.0, 0.1, 1.0 - hitUv);
        conf *= edge.x * edge.y;
        // Fade rays facing back toward the camera (grazing) — they carry the least
        // reliable screen information.
        conf *= saturate(-viewDir.z);
    }
    outSsr[tid.xy] = float4(hitColor * pc.params.y, conf);
}
