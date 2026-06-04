// Atmosphere envCube fill: samples the sky-view LUT by the (azimuth, elevation) of each cube
// direction and adds the sun disk, writing into the 6-layer rgba16f environment cube. Same
// output contract as ibl_skygen (set 0 binding 1 = storage cube), so the IBL convolution chain
// and the visible-sky pass consume it unchanged. Binding 0 = sky-view LUT sampler.

[[vk::binding(0, 0)]] Sampler2D<float4> skyViewLut;
[[vk::image_format("rgba16f")]]
[[vk::binding(1, 0)]] RWTexture2DArray<float4> outCube;

struct AtmosParams
{
    float4 sunDir;    // xyz = dir to sun, w = sun intensity
    float4 rayleigh;
    float4 ozone;
    float4 params0;   // planetRadius, atmosphereHeight, mieScaleHeight, mieAnisotropy
    float4 params1;   // sunDiskAngularRadius, sunDiskIntensity, cameraAltitude, unused
};
[[vk::push_constant]] AtmosParams pc;

static const float PI = 3.14159265;

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

// Inverse of the sky-view LUT mapping: world dir -> (u = azimuth, v = horizon-densified elevation).
float2 dirToSkyViewUv(float3 dir)
{
    float azimuth = atan2(dir.z, dir.x);
    if (azimuth < 0.0) { azimuth += 2.0 * PI; }
    float elevation = asin(clamp(dir.y, -1.0, 1.0));
    float u = azimuth / (2.0 * PI);
    // Invert v = 0.5 +/- sqrt(|el| / (pi/2)) * 0.5.
    float a = abs(elevation) / (PI * 0.5);
    float v = 0.5 + sign(elevation) * sqrt(saturate(a)) * 0.5;
    return float2(u, v);
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

    float2 svUv = dirToSkyViewUv(dir);
    float3 sky = skyViewLut.SampleLevel(svUv, 0.0).rgb;

    // Sun disk: a smooth cap around the sun direction within sunDiskAngularRadius.
    float3 sunDir = normalize(pc.sunDir.xyz);
    float cosAngle = dot(dir, sunDir);
    float cosRadius = cos(pc.params1.x);
    float disk = smoothstep(cosRadius, cosRadius + 0.0002, cosAngle);
    sky += disk * pc.params1.y * pc.sunDir.w;

    outCube[tid] = float4(sky, 1.0);
}
