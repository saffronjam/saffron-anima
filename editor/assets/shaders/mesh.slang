// Instanced mesh shader. Vertex inputs come from an interleaved buffer
// (position/normal/uv0); per-instance model + normal matrix + base color come from
// a storage buffer (set 2) indexed by the absolute Vulkan InstanceIndex
// (firstInstance + instance). SV_VulkanInstanceID is that builtin — plain
// SV_InstanceID is base-relative (excludes firstInstance), which would break batching.
// The push constant is the camera viewProj. Set 0 is albedo; set 1 is the lighting set:
// directional UBO (0), punctual light list (1), per-cluster light lists (2), cluster
// params UBO (3). Punctual lights are clustered-forward: the fragment loops only its
// froxel cluster's lights (params.screenSize.z != 0), or every light otherwise.

static const uint MAX_LIGHTS_PER_CLUSTER = 64;

struct VertexInput
{
    [[vk::location(0)]] float3 position;
    [[vk::location(1)]] float3 normal;
    [[vk::location(2)]] float2 uv0;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 worldPos : POSITION0;
    float3 worldNormal : NORMAL;
    float2 uv0 : TEXCOORD0;
    float4 baseColor : COLOR0;
    nointerpolation uint textureIndex : TEXCOORD1;
    nointerpolation float2 mr : TEXCOORD2;        // x = metallic, y = roughness
    nointerpolation float3 emissive : TEXCOORD3;  // emissive radiance (strength baked in)
};

struct Camera
{
    float4x4 viewProj;
};
[[vk::push_constant]] Camera camera;

// Bindless: one global albedo array (set 0) indexed per-instance. Partially bound +
// update-after-bind on the C++ side; sampled with NonUniformResourceIndex. The size
// matches MaxBindlessTextures in renderer.cppm (a bounded array, not a runtime one).
[[vk::binding(0, 0)]] Sampler2D albedoTextures[1024];

struct LightGlobals
{
    float4 directionAmbient;     // xyz = travel direction, w = ambient
    float4 colorIntensity;       // rgb = color, a = intensity
    uint4 counts;                // x = punctual count, y = directional shadow, z = IBL, w = SSAO
    float4 eyePosition;          // xyz = world-space camera position (for the view vector)
    float4x4 shadowViewProj;     // world -> directional light clip space
    float4x4 spotShadowViewProj; // world -> shadowed spot light clip space (perspective)
    uint4 spotShadow;            // x = shadowed spot's light index, y = enabled (0/1)
    float4 pointShadow;          // xyz = shadowed point light world pos, w = far plane
    uint4 pointShadowMeta;       // x = shadowed point's light index, y = enabled (0/1)
    uint4 screenFlags;           // x = contact shadows, y = SSGI, z = DDGI (AO is counts.w)
    float4 ddgiVolumeMin;        // xyz = DDGI probe volume world min
    float4 ddgiVolumeExtent;     // xyz = DDGI probe volume world size
    uint4 ddgiProbeCount;        // xyz = probes per axis, w = irradiance tile interior
};
[[vk::binding(0, 1)]] ConstantBuffer<LightGlobals> globals;

struct GpuLight
{
    float4 positionRange;   // xyz = world position, w = range
    float4 colorIntensity;  // rgb = color, a = intensity
    float4 directionType;   // xyz = world direction (spot), w = type (0 point, 1 spot)
    float4 spotCos;         // x = cos(inner half-angle), y = cos(outer half-angle)
};
[[vk::binding(1, 1)]] StructuredBuffer<GpuLight> lights;

struct Cluster
{
    uint count;
    uint indices[MAX_LIGHTS_PER_CLUSTER];
};
[[vk::binding(2, 1)]] StructuredBuffer<Cluster> clusters;

struct ClusterParams
{
    float4x4 view;
    float4x4 inverseProjection;
    uint4 gridSize;       // xyz = grid dims, w = light count
    uint4 screenSize;     // xy = pixels, z = clustered flag
    float4 zPlanes;       // x = near, y = far
};
[[vk::binding(3, 1)]] ConstantBuffer<ClusterParams> clusterParams;

// Directional + spot shadow maps: depth textures sampled with a comparison (PCF) sampler.
[[vk::binding(4, 1)]] Sampler2DShadow shadowMap;
[[vk::binding(5, 1)]] Sampler2DShadow spotShadowMap;
// Point shadow: an omnidirectional cube of world distance-to-light (linear sampler).
[[vk::binding(6, 1)]] SamplerCube pointShadowMap;

// Visibility for the shadowed point light: compare the fragment's distance to the light
// against the nearest occluder distance stored in the cube (sampled by light->frag dir).
float pointShadow(float3 worldPos)
{
    float3 toFrag = worldPos - globals.pointShadow.xyz;
    float dist = length(toFrag);
    float stored = pointShadowMap.SampleLevel(normalize(toFrag), 0.0).r;
    const float bias = 0.08;  // matches PointShadowDistanceBias (renderer_detail.cppm)
    return dist - bias <= stored ? 1.0 : 0.0;
}

// 3x3 PCF visibility for a light-space projection (1 = lit, 0 = shadowed). Off-map and
// beyond-far-plane samples are treated as lit. `map` selects which shadow texture.
float pcfShadow(Sampler2DShadow map, float4x4 viewProj, float3 worldPos)
{
    float4 clip = mul(viewProj, float4(worldPos, 1.0));
    float3 ndc = clip.xyz / clip.w;
    float2 uv = ndc.xy * 0.5 + 0.5;
    if (clip.w <= 0.0 || uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || ndc.z > 1.0)
    {
        return 1.0;
    }
    const float texel = 1.0 / 2048.0;  // matches ShadowMapSize
    float sum = 0.0;
    for (int x = -1; x <= 1; x = x + 1)
    {
        for (int y = -1; y <= 1; y = y + 1)
        {
            sum += map.SampleCmp(uv + float2(x, y) * texel, ndc.z);
        }
    }
    return sum / 9.0;
}

struct Instance
{
    float4x4 model;
    float4x4 normalMatrix;
    float4 baseColor;
    uint4 texture;    // x = bindless albedo index
    float4 pbr;       // x = metallic, y = roughness
    float4 emissive;  // rgb = emissive radiance (strength baked in)
};
[[vk::binding(0, 2)]] StructuredBuffer<Instance> instances;

// Set 3: image-based lighting — diffuse irradiance cube, prefiltered specular cube, and
// the split-sum BRDF LUT. Sampled for the ambient term when globals.counts.z != 0.
[[vk::binding(0, 3)]] SamplerCube irradianceMap;
[[vk::binding(1, 3)]] SamplerCube prefilteredMap;
[[vk::binding(2, 3)]] Sampler2D brdfLut;
static const float IblPrefilterMaxMip = 4.0;  // == IblPrefilterMips - 1 (renderer_detail.cppm)

// Set 4: screen-space maps, all sampled by screen UV. AO (factor, 1 = open) modulates the
// indirect/ambient term (counts.w); contact shadows (factor, 1 = lit) darken the
// directional direct term (screenFlags.x); SSGI (rgba16f radiance) adds one-bounce indirect
// (screenFlags.y).
[[vk::binding(0, 4)]] Sampler2D aoMap;
[[vk::binding(1, 4)]] Sampler2D contactMap;
[[vk::binding(2, 4)]] Sampler2D ssgiMap;

// Set 5: DDGI probe atlases (octahedral irradiance + distance moments), sampled for
// multi-bounce indirect when screenFlags.z != 0.
[[vk::binding(0, 5)]] Sampler2D ddgiIrradiance;
[[vk::binding(1, 5)]] Sampler2D ddgiDistance;

float2 ddgiOctEncode(float3 d)
{
    d /= (abs(d.x) + abs(d.y) + abs(d.z));
    float2 o = d.z >= 0.0 ? d.xy : (1.0 - abs(d.yx)) * float2(d.x >= 0.0 ? 1.0 : -1.0, d.y >= 0.0 ? 1.0 : -1.0);
    return o * 0.5 + 0.5;
}

// Atlas UV for probe `p` sampling direction `dir`, given the tile interior + gutter.
float2 ddgiAtlasUv(uint3 p, float3 dir, uint interior, uint2 atlasSize)
{
    uint tile = interior + 2;
    uint tileX = p.x + p.y * globals.ddgiProbeCount.x;
    uint tileY = p.z;
    float2 oct = ddgiOctEncode(normalize(dir));
    float2 interiorPx = oct * float(interior) + 1.0;
    float2 atlasPx = float2(tileX, tileY) * float(tile) + interiorPx;
    return (atlasPx + 0.5) / float2(atlasSize);
}

// Sample the 8-probe cage around worldPos: trilinear weight * soft backface * Chebyshev
// visibility (kills light leaking). Returns irradiance to add to the indirect term.
float3 ddgiSampleIrradiance(float3 worldPos, float3 n)
{
    uint3 pc = globals.ddgiProbeCount.xyz;
    uint interior = globals.ddgiProbeCount.w;
    uint2 irrAtlas = uint2(pc.x * pc.y * (interior + 2), pc.z * (interior + 2));
    uint2 distAtlas = uint2(pc.x * pc.y * (16u + 2u), pc.z * (16u + 2u));

    float3 t = (worldPos - globals.ddgiVolumeMin.xyz) / globals.ddgiVolumeExtent.xyz;  // [0,1] in volume
    float3 grid = t * float3(pc - 1u);                 // probe-space coordinate
    int3 base = int3(floor(grid));
    float3 frac = grid - float3(base);

    float3 sum = float3(0.0);
    float wsum = 0.0;
    for (uint c = 0; c < 8; c = c + 1)
    {
        int3 off = int3(c & 1u, (c >> 1) & 1u, (c >> 2) & 1u);
        int3 pi = clamp(base + off, int3(0), int3(pc) - 1);
        float3 probePos = globals.ddgiVolumeMin.xyz + (float3(pi) + 0.5) / float3(pc) * globals.ddgiVolumeExtent.xyz;

        // Trilinear weight.
        float3 tw = lerp(1.0 - frac, frac, float3(off));
        float weight = tw.x * tw.y * tw.z;

        // Soft backface: prefer probes the surface normal faces.
        float3 dirToProbe = probePos - worldPos;
        float distToProbe = length(dirToProbe);
        dirToProbe = distToProbe > 1e-4 ? dirToProbe / distToProbe : n;
        weight *= max(0.05, (dot(dirToProbe, n) * 0.5 + 0.5));

        // Chebyshev visibility: compare distToProbe against the probe's stored moments in
        // the surface->probe direction (kills leaking through occluders).
        float2 distUv = ddgiAtlasUv(uint3(pi), -dirToProbe, 16u, distAtlas);
        float2 moments = ddgiDistance.SampleLevel(distUv, 0.0).rg;
        float meanDist = moments.x;
        if (distToProbe > meanDist)
        {
            float variance = abs(moments.x * moments.x - moments.y);
            float d = distToProbe - meanDist;
            float cheb = variance / (variance + d * d);
            cheb = max(cheb * cheb * cheb, 0.0);  // sharpen
            weight *= cheb;
        }

        float2 irrUv = ddgiAtlasUv(uint3(pi), n, interior, irrAtlas);
        sum += ddgiIrradiance.SampleLevel(irrUv, 0.0).rgb * weight;
        wsum += weight;
    }
    return wsum > 1e-4 ? sum / wsum : float3(0.0);
}

float3 fresnelSchlickRoughness(float cosT, float3 F0, float roughness)
{
    return F0 + (max(float3(1.0 - roughness), F0) - F0) * pow(1.0 - cosT, 5.0);
}

[shader("vertex")]
VertexOutput vertexMain(VertexInput input, uint instanceIndex : SV_VulkanInstanceID)
{
    Instance inst = instances[instanceIndex];
    VertexOutput output;
    float4 worldPos = mul(inst.model, float4(input.position, 1.0));
    output.worldPos = worldPos.xyz;
    output.position = mul(camera.viewProj, worldPos);
    output.worldNormal = mul((float3x3)inst.normalMatrix, input.normal);
    output.uv0 = input.uv0;
    output.baseColor = inst.baseColor;
    output.textureIndex = inst.texture.x;
    output.mr = inst.pbr.xy;
    output.emissive = inst.emissive.rgb;
    return output;
}

// UE4-style punctual attenuation: inverse-square with a smooth range window.
float distanceAttenuation(float dist, float range)
{
    float invSquare = 1.0 / max(dist * dist, 0.0001);
    float t = saturate(1.0 - pow(dist / range, 4.0));
    return invSquare * t * t;
}

// Cook-Torrance metallic/roughness BRDF (energy-conserving). `radiance` is the
// incoming light's color * intensity * attenuation * cone; the cosine + diffuse/PI
// are applied here. Shared by the directional and the punctual lights.
static const float PI = 3.14159265;

float3 fresnelSchlick(float cosT, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosT, 5.0);
}

float distributionGGX(float ndoth, float a)  // a = roughness^2
{
    float a2 = a * a;
    float d = ndoth * ndoth * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-5);
}

float visibilitySmithGGX(float ndotv, float ndotl, float a)  // height-correlated Smith
{
    float a2 = a * a;
    float v = ndotl * sqrt(ndotv * ndotv * (1.0 - a2) + a2);
    float l = ndotv * sqrt(ndotl * ndotl * (1.0 - a2) + a2);
    return 0.5 / max(v + l, 1e-5);
}

float3 brdf(float3 n, float3 v, float3 l, float3 albedo, float metallic, float roughness, float3 radiance)
{
    float3 h = normalize(v + l);
    float ndotl = max(dot(n, l), 0.0);
    float ndotv = max(dot(n, v), 1e-4);
    float ndoth = max(dot(n, h), 0.0);
    float a = roughness * roughness;
    float3 F0 = lerp(float3(0.04), albedo, metallic);
    float3 F = fresnelSchlick(max(dot(h, v), 0.0), F0);
    float D = distributionGGX(ndoth, a);
    float Vis = visibilitySmithGGX(ndotv, ndotl, a);
    float3 spec = D * Vis * F;
    float3 kd = (1.0 - F) * (1.0 - metallic);
    float3 diff = kd * albedo / PI;
    return (diff + spec) * radiance * ndotl;
}

// One punctual light's Cook-Torrance contribution at a world-space surface point.
// `lightIndex` lets the shadowed spot light apply its shadow-map visibility.
float3 punctual(GpuLight lt, uint lightIndex, float3 worldPos, float3 n, float3 v,
                float3 albedo, float metallic, float roughness)
{
    float3 toLight = lt.positionRange.xyz - worldPos;
    float dist = length(toLight);
    if (dist > lt.positionRange.w)
    {
        return float3(0.0);
    }
    float3 l = toLight / max(dist, 0.0001);
    float attenuation = distanceAttenuation(dist, lt.positionRange.w);

    float cone = 1.0;
    float shadow = 1.0;
    if (lt.directionType.w > 0.5)
    {
        float3 spotDir = normalize(lt.directionType.xyz);
        float cosAngle = dot(spotDir, -l);
        cone = smoothstep(lt.spotCos.y, lt.spotCos.x, cosAngle);
        if (globals.spotShadow.y != 0 && lightIndex == globals.spotShadow.x)
        {
            shadow = pcfShadow(spotShadowMap, globals.spotShadowViewProj, worldPos);
        }
    }
    else if (globals.pointShadowMeta.y != 0 && lightIndex == globals.pointShadowMeta.x)
    {
        shadow = pointShadow(worldPos);
    }
    float3 radiance = lt.colorIntensity.rgb * lt.colorIntensity.a * attenuation * cone * shadow;
    return brdf(n, v, l, albedo, metallic, roughness, radiance);
}

// Froxel cluster index for a fragment, from its pixel position + view-space depth.
uint clusterIndexFor(float2 pixel, float viewZ)
{
    float near = clusterParams.zPlanes.x;
    float far = clusterParams.zPlanes.y;
    float depth = max(-viewZ, near);
    uint zSlice = uint(log(depth / near) / log(far / near) * float(clusterParams.gridSize.z));
    zSlice = min(zSlice, clusterParams.gridSize.z - 1);

    float2 tileSize = float2(clusterParams.screenSize.xy) / float2(clusterParams.gridSize.xy);
    uint tileX = min(uint(pixel.x / tileSize.x), clusterParams.gridSize.x - 1);
    uint tileY = min(uint(pixel.y / tileSize.y), clusterParams.gridSize.y - 1);
    return tileX + tileY * clusterParams.gridSize.x + zSlice * clusterParams.gridSize.x * clusterParams.gridSize.y;
}

// Übershader permutation: a Vulkan specialization constant baked per PSO. The PSO cache
// builds one variant per value (lit vs unlit), so a material picks a pipeline by flag.
[[vk::constant_id(0)]] const bool kUnlit = false;

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    float3 n = normalize(input.worldNormal);
    float4 tex = albedoTextures[NonUniformResourceIndex(input.textureIndex)].Sample(input.uv0);
    float3 albedo = tex.rgb * input.baseColor.rgb;

    // Unlit: albedo * base color + emissive, no lighting (a different cached PSO via kUnlit).
    if (kUnlit)
    {
        return float4(albedo + input.emissive, tex.a * input.baseColor.a);
    }

    float metallic = saturate(input.mr.x);
    float roughness = clamp(input.mr.y, 0.045, 1.0);  // clamp away from 0 (specular singularity)
    float3 v = normalize(globals.eyePosition.xyz - input.worldPos);

    // Screen UV for sampling the screen-space effect maps.
    float2 screenUv = input.position.xy / float2(clusterParams.screenSize.xy);

    // Directional light, evaluated through the same BRDF as the punctual lights, then
    // attenuated by the shadow map + screen-space contact shadows (directional only in v1).
    float3 lDir = -normalize(globals.directionAmbient.xyz);
    float shadow = 1.0;
    if (globals.counts.y != 0)
    {
        shadow = pcfShadow(shadowMap, globals.shadowViewProj, input.worldPos);
    }
    if (globals.screenFlags.x != 0)
    {
        shadow *= contactMap.SampleLevel(screenUv, 0.0).r;  // fine contact detail
    }
    float3 lo = brdf(n, v, lDir, albedo, metallic, roughness,
                     globals.colorIntensity.rgb * globals.colorIntensity.a) * shadow;

    // Punctual lights — clustered when enabled, else loop every light.
    if (clusterParams.screenSize.z != 0)
    {
        float viewZ = mul(clusterParams.view, float4(input.worldPos, 1.0)).z;
        uint clusterIndex = clusterIndexFor(input.position.xy, viewZ);
        uint count = clusters[clusterIndex].count;
        for (uint i = 0; i < count; i = i + 1)
        {
            uint li = clusters[clusterIndex].indices[i];
            lo += punctual(lights[li], li, input.worldPos, n, v, albedo, metallic, roughness);
        }
    }
    else
    {
        for (uint i = 0; i < globals.counts.x; i = i + 1)
        {
            lo += punctual(lights[i], i, input.worldPos, n, v, albedo, metallic, roughness);
        }
    }

    // Indirect (ambient): split-sum IBL when enabled, else the flat scalar fallback.
    float3 ambient;
    if (globals.counts.z != 0)
    {
        float3 F0 = lerp(float3(0.04), albedo, metallic);
        float ndotv = max(dot(n, v), 1e-4);
        float3 F = fresnelSchlickRoughness(ndotv, F0, roughness);
        float3 kd = (1.0 - F) * (1.0 - metallic);
        float3 irradiance = irradianceMap.SampleLevel(n, 0.0).rgb;
        float3 diffuseIBL = kd * irradiance * albedo;
        float3 R = reflect(-v, n);
        float3 prefiltered = prefilteredMap.SampleLevel(R, roughness * IblPrefilterMaxMip).rgb;
        float2 ab = brdfLut.SampleLevel(float2(ndotv, roughness), 0.0).rg;
        float3 specularIBL = prefiltered * (F0 * ab.x + ab.y);
        ambient = diffuseIBL + specularIBL;
    }
    else
    {
        ambient = albedo * (1.0 - metallic) * globals.directionAmbient.w;
    }
    // Screen-space AO darkens only the indirect (ambient) term, sampled at this pixel.
    if (globals.counts.w != 0)
    {
        ambient *= aoMap.SampleLevel(screenUv, 0.0).r;
    }
    // SSGI adds one-bounce indirect: the gathered radiance acts as extra incoming light on
    // the diffuse albedo (modulated by AO so occluded creases don't over-bounce).
    if (globals.screenFlags.y != 0)
    {
        float ao = globals.counts.w != 0 ? aoMap.SampleLevel(screenUv, 0.0).r : 1.0;
        float3 gi = ssgiMap.SampleLevel(screenUv, 0.0).rgb;
        ambient += gi * albedo * (1.0 - metallic) * ao;
    }
    // DDGI adds world-space multi-bounce indirect from the probe cage (diffuse only).
    if (globals.screenFlags.z != 0)
    {
        float3 probeIrradiance = ddgiSampleIrradiance(input.worldPos, n);
        ambient += probeIrradiance * albedo * (1.0 - metallic);
    }
    float3 color = lo + ambient + input.emissive;
    return float4(color, tex.a * input.baseColor.a);
}
