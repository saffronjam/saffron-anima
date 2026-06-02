// Writes the procedural HDR sky into the environment cubemap (6 layers of a 2D array
// view). One invocation per output texel; tid.z is the cube face. Set 0 binding 0 is the
// cube as an rgba16f storage image. Helpers are inlined per-file (repo shader convention).

[[vk::image_format("rgba16f")]]
[[vk::binding(0, 0)]] RWTexture2DArray<float4> outCube;

// Sun parameters from the scene's directional light (so the baked sky + IBL follow it).
struct SkyParams
{
    float4 sunDir;    // xyz = direction TO the sun, w = intensity scale
    float4 sunColor;  // rgb = sun tint, a unused
};
[[vk::push_constant]] SkyParams sky;

// Cube face + in-face uv (in [-1,1]) -> world direction (hardware cube convention).
float3 cubeFaceDir(uint face, float2 uv)
{
    float3 d;
    if (face == 0)      { d = float3( 1.0, -uv.y, -uv.x); }
    else if (face == 1) { d = float3(-1.0, -uv.y,  uv.x); }
    else if (face == 2) { d = float3( uv.x,  1.0,  uv.y); }
    else if (face == 3) { d = float3( uv.x, -1.0, -uv.y); }
    else if (face == 4) { d = float3( uv.x, -uv.y,  1.0); }
    else                { d = float3(-uv.x, -uv.y, -1.0); }
    return normalize(d);
}

// An analytic HDR sky: a horizon->zenith gradient over a dim ground + a bright sun disk.
float3 proceduralSky(float3 dir)
{
    const float3 sunDir = normalize(sky.sunDir.xyz);
    const float sunI = sky.sunDir.w;
    const float3 sunTint = sky.sunColor.rgb;
    const float3 zenith = float3(0.10, 0.26, 0.62);
    const float3 horizon = float3(0.62, 0.70, 0.86);
    const float3 ground = float3(0.16, 0.14, 0.12);

    float up = dir.y;
    float3 col;
    if (up >= 0.0)
    {
        col = lerp(horizon, zenith, pow(saturate(up), 0.6));
    }
    else
    {
        col = lerp(horizon, ground, saturate(-up * 3.0));
    }
    col *= 1.6;

    float s = max(dot(normalize(dir), sunDir), 0.0);
    col += pow(s, 1200.0) * float3(22.0, 20.0, 17.0) * sunTint * sunI;
    col += pow(s, 6.0) * float3(0.30, 0.26, 0.20) * sunTint * sunI;
    return col;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    uint width;
    uint height;
    uint layers;
    outCube.GetDimensions(width, height, layers);
    if (tid.x >= width || tid.y >= height || tid.z >= layers)
    {
        return;
    }
    float2 uv = (float2(tid.xy) + 0.5) / float2(width, height) * 2.0 - 1.0;
    float3 dir = cubeFaceDir(tid.z, uv);
    outCube[tid] = float4(proceduralSky(dir), 1.0);
}
