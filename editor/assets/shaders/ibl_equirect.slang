// Projects an equirectangular panorama into the IBL environment cube (6 layers of a 2D
// array view). One invocation per output texel; tid.z is the cube face. Set 0 binding 1 is
// the cube as an rgba16f storage image; binding 0 is the user panorama (HDR or LDR).

[[vk::binding(0, 0)]] Sampler2D<float4> panorama;
[[vk::image_format("rgba16f")]]
[[vk::binding(1, 0)]] RWTexture2DArray<float4> outCube;

struct EquirectParams
{
    float4 params;  // x = rotation (yaw radians), y = intensity, zw unused
};
[[vk::push_constant]] EquirectParams pc;

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
    float yaw = atan2(dir.z, dir.x) + pc.params.x;
    float u = yaw / (2.0 * 3.14159265) + 0.5;
    float v = acos(clamp(dir.y, -1.0, 1.0)) / 3.14159265;  // 0 at +Y zenith
    float3 c = panorama.SampleLevel(float2(u, v), 0.0).rgb * pc.params.y;
    outCube[tid] = float4(c, 1.0);
}
