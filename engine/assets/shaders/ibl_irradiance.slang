// Diffuse irradiance convolution: for each output cube texel (a surface normal), cosine-
// weight-integrate the environment over the hemisphere. Set 0: binding 0 = environment
// cube (sampler), binding 1 = irradiance cube (rgba16f storage, 6-layer array view).

static const float PI = 3.14159265359;

[[vk::binding(0, 0)]] SamplerCube envCube;
[[vk::image_format("rgba16f")]]
[[vk::binding(1, 0)]] RWTexture2DArray<float4> outCube;

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
    float3 n = cubeFaceDir(tid.z, uv);

    float3 up = abs(n.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 right = normalize(cross(up, n));
    up = cross(n, right);

    float3 irradiance = float3(0.0);
    uint samples = 0;
    const uint phiSteps = 64;
    const uint thetaSteps = 16;
    for (uint p = 0; p < phiSteps; p = p + 1)
    {
        float phi = 2.0 * PI * (float(p) + 0.5) / float(phiSteps);
        for (uint t = 0; t < thetaSteps; t = t + 1)
        {
            float theta = 0.5 * PI * (float(t) + 0.5) / float(thetaSteps);
            float3 tangent = float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            float3 sampleDir = tangent.x * right + tangent.y * up + tangent.z * n;
            irradiance += envCube.SampleLevel(sampleDir, 0.0).rgb * cos(theta) * sin(theta);
            samples = samples + 1;
        }
    }
    irradiance = PI * irradiance / float(samples);
    outCube[tid] = float4(irradiance, 1.0);
}
